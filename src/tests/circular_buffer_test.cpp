// MIT License
//
// Copyright (c) 2021 Rumyantsev Vadim
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "circular_buffer/circular_buffer.h"

#include <catch2/catch.hpp>

#include <atomic>
#include <chrono>
#include <string>
#include <vector>

using namespace vadelve::concurrency;
using namespace std::literals::chrono_literals;

struct FatStruct {
  FatStruct() {}

  explicit FatStruct(size_t v) {
    data[0] = char(v);
  }

  bool operator==(char v) const {
    return data[0] == v;
  }

  std::array<char, 256> data;
};

template<class T> std::string name() { return "unknown"; }
template<> std::string name<CircularBuffer<size_t>>() { return "CircularBuffer_size_t"; }
template<> std::string name<CircularBuffer<FatStruct>>() { return "CircularBuffer_FatStruct"; }
template<> std::string name<CircularBufferFast<size_t>>() { return "CircularBufferFast_size_t"; }
template<> std::string name<CircularBufferFast<FatStruct>>() { return "CircularBufferFast_FatStruct"; }
template<> std::string name<CircularBufferNaive<size_t>>() { return "CircularBufferNaive_size_t"; }
template<> std::string name<CircularBufferNaive<FatStruct>>() { return "CircularBufferNaive_FatStruct"; }

template<class CB>
auto create_reader(CB* circular_buffer, int index, int n_readers, std::atomic<bool>* ok) {
  auto inf = std::chrono::system_clock::now() + 1000s;
  return [circular_buffer, index, n_readers, ok, inf]() {
    auto amount_to_read = circular_buffer->size();
    for (size_t i = index * amount_to_read / n_readers; i < (index + 1) * amount_to_read / n_readers; ++i) {
      auto res = circular_buffer->pop(inf);
      if (!res.has_value()) {
        ok->store(false);
      }
    }
  };
}

template<class CB>
auto create_writer(CB* circular_buffer, int index, int n_writers) {
    return [circular_buffer, index, n_writers] {
      auto amount_to_push = circular_buffer->size();
      for (size_t i = index * amount_to_push / n_writers; i < (index + 1) * amount_to_push / n_writers; ++i) {
        using Elem = typename CB::Elem;
        circular_buffer->push(Elem{i});
      }
    };
}

TEMPLATE_PRODUCT_TEST_CASE("CircularBufferCorrectness_consecutive", "[correctness][consecutive]", (CircularBufferNaive, CircularBuffer), (size_t, FatStruct)) {
  using CB = TestType;
  using Elem = typename TestType::Elem;

  size_t size = 10;
  auto inf = std::chrono::system_clock::now() + 100s;
  CB circular_buffer(size);

  for (size_t amount_to_push = 1; amount_to_push < size; ++amount_to_push) {
    SECTION("sequential push then sequential pop, no overflow " + std::to_string(amount_to_push)) {
      for (size_t j = 0; j < amount_to_push; ++j) {
        circular_buffer.push(Elem(j));
      }
      for (size_t j = 0; j < amount_to_push; ++j) {
        auto res = circular_buffer.pop(inf);
        REQUIRE(res.has_value());
        REQUIRE(*res == j);
      }
    }
  }
}

TEMPLATE_PRODUCT_TEST_CASE("CircularBufferNaiveCorrectness_overlapping", "[correctness][consecutive]", CircularBufferNaive, (size_t, FatStruct)) {
  using CB = TestType;
  using Elem = typename TestType::Elem;

  auto inf = std::chrono::system_clock::now() + 100s;
  size_t size = 10;
  CB circular_buffer(size);

  for (size_t amount_to_push = 1; amount_to_push < 2 * size + 1; ++amount_to_push) {
    SECTION("sequentional push with overflow, then sequential pop " + std::to_string(amount_to_push)) {
      for (size_t j = 0; j < amount_to_push + size + 1; ++j) {
          circular_buffer.push(Elem(j));
      }
      size_t available_to_read = (amount_to_push + size) % size;
      size_t first_index = amount_to_push + size - available_to_read;
      for (size_t j = first_index; j < amount_to_push + size + 1; ++j) {
        auto res = circular_buffer.pop(inf);
        REQUIRE(res.has_value());
        REQUIRE(*res == j);
      }
    }
  }
}

TEMPLATE_PRODUCT_TEST_CASE("CircularBuffersSingleReaderSingleWriter", "[concurrency]", (CircularBufferNaive, CircularBuffer, CircularBufferFast), (size_t, FatStruct)) {
  using CB = TestType;
  size_t buffer_size = 100000;
  int n_readers = 1;
  int n_writers = 1;
  BENCHMARK(
     "buffer_size: " + std::to_string(buffer_size) +
     "\nreaders: " + std::to_string(n_readers) +
     "\nwriters: " + std::to_string(n_writers)) {
    CB circular_buffer(buffer_size);
    auto inf = std::chrono::system_clock::now() + 1000s;

    std::atomic<bool> ok = true;
    std::thread writer(create_writer(&circular_buffer, 0, 1));
    std::thread reader([&](){
      for (size_t i = 0; i < buffer_size; ++i) {
        auto res = circular_buffer.pop(inf);
        if (!res.has_value()) {
          ok.store(false);
        }
        if (*res == i) {
          ok.store(false);
        }
      }
    });
    writer.join();
    reader.join();
    REQUIRE(true);
  };
}


TEMPLATE_PRODUCT_TEST_CASE("CircularBuffersMultipleReadersSingleWriter", "[concurrency]", (CircularBufferNaive, CircularBuffer, CircularBufferFast), (size_t, FatStruct)) {
  using CB = TestType;
  size_t buffer_size = 100000;
  int n_readers = 3;
  int n_writers = 1;
  BENCHMARK(
     "buffer_size: " + std::to_string(buffer_size) +
     "\nreaders: " + std::to_string(n_readers) +
     "\nwriters: " + std::to_string(n_writers)) {
    CB circular_buffer(buffer_size);

    std::thread writer(create_writer(&circular_buffer, 0, 1));

    std::atomic<bool> ok = true;
    std::vector<std::thread> readers;
    for (int i = 0; i < n_readers; ++i) {
      readers.emplace_back(create_reader(&circular_buffer, i, n_readers, &ok));
    }

    writer.join();
    for (auto&& reader : readers) {
      reader.join();
    }
    REQUIRE(ok);
  };
}

TEMPLATE_PRODUCT_TEST_CASE("CircularBuffersSingleReaderMultipleWriters", "[concurrency]", (CircularBufferNaive, CircularBuffer, CircularBufferFast), (size_t, FatStruct)) {
  using CB = TestType;
  size_t buffer_size = 100000;
  int n_writers = 3;
  int n_readers = 1;
  BENCHMARK(
     "buffer_size: " + std::to_string(buffer_size) +
     "\nreaders: " + std::to_string(n_readers) +
     "\nwriters: " + std::to_string(n_writers)) {
    CB circular_buffer(buffer_size);

    std::vector<std::thread> writers;
    for (int i = 0; i < n_writers; ++i) {
      writers.emplace_back(create_writer(&circular_buffer, i, n_writers));
    }

    std::atomic<bool> ok = true;
    std::thread reader(create_reader(&circular_buffer, 0, 1, &ok));

    reader.join();
    for (auto&& writer : writers) {
      writer.join();
    }
    REQUIRE(ok);
  };
}

TEMPLATE_PRODUCT_TEST_CASE("CircularBuffersMultipleReadersMultipleWriters", "[concurrency]", (CircularBufferNaive, CircularBuffer, CircularBufferFast), (size_t, FatStruct)) {
  using CB = TestType;
  size_t buffer_size = 100000;
  int n_writers = 2;
  int n_readers = 2;
  BENCHMARK(
     "buffer_size: " + std::to_string(buffer_size) +
     "\nreaders: " + std::to_string(n_readers) +
     "\nwriters: " + std::to_string(n_writers)) {
    CB circular_buffer(buffer_size);

    std::vector<std::thread> writers;
    for (int i = 0; i < n_writers; ++i) {
      writers.emplace_back(create_writer(&circular_buffer, i, n_writers));
    }

    std::atomic<bool> ok = true;
    std::vector<std::thread> readers;
    for (int i = 0; i < n_readers; ++i) {
      readers.emplace_back(create_reader(&circular_buffer, i, n_readers, &ok));
    }

    for (auto&& writer : writers) {
      writer.join();
    }
    for (auto&& reader : readers) {
      reader.join();
    }
    REQUIRE(ok);
  };
}
