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

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <vector>
#include <optional>

namespace vadelve::concurrency {

namespace {

template<class T>
class RaiiCounter {
 public:
  RaiiCounter(T& counter)
    : counter_(++counter) { }

  ~RaiiCounter() { --counter_; }

  RaiiCounter(const RaiiCounter&) = delete;
  RaiiCounter(RaiiCounter&&) = delete;

  T& counter_;
};

}  // namespace

//
// CircularBufferNaive  thread safe class that allows to push and pop data in a queue manner.
//                      uses lock to read and write data.
//
// push     adds data the end of the container.
//          if the buffer is full then previous buffer_.size() amount of records becomes lost.
//
// pop      gets value from the front of the buffer.
//          if there are no data to get waits on a conditinal_variable for specified amount of time.
//
template<class T>
class CircularBufferNaive {
 public:
  using Elem = T;

  CircularBufferNaive(size_t size)
    : buffer_(size)
  { }

  size_t size() const { return buffer_.size(); }

  void push(T data) {
    size_t waiters;
    {
      std::unique_lock l(m_);
      buffer_[writer_] = std::move(data);
      postfix_increment(writer_);
      if (writer_ == reader_) {
        full_ = true;
      }
      waiters = waiters_;
    }
    if (waiters) {
      cv_.notify_one();
    }
  }

  //
  // retuns std::nullopt on a timeout
  //
  template<class Clock, class Duration>
  std::optional<T> pop(const std::chrono::time_point<Clock, Duration>& timeout_time) {
    std::unique_lock l(m_);
    if (reader_ == writer_) {
      if (full_) {
        full_ = false;
        return std::move(buffer_[postfix_increment(reader_)]);
      }
      const auto& _ = RaiiCounter{waiters_};
      if (false == cv_.wait_until(l, timeout_time, [&](){ return reader_ != writer_ || full_ == true; })) {
        return std::nullopt; // timeout
      }
    }
    full_ = false;
    return std::move(buffer_[postfix_increment(reader_)]);
  }

 private:
  // return previous value
  inline size_t postfix_increment(size_t& index) {
    auto res = index;
    index = (index + 1) % buffer_.size();
    return res;
  }

  std::vector<T> buffer_;
  std::mutex m_;
  std::condition_variable cv_;
  size_t writer_ = 0;
  size_t reader_ = 0;
  size_t waiters_ = 0;
  bool full_ = false;
};


//
// CircularBufferFast   is a lock free class that allow to push and pop data in a queue manner.
//                      Doesn't go into the kernal at any point.
//
// push   adds data to the end of the buffer
//
// pop    gets value from the front of the container
//
// Assumptions: readers and writers never overlap, so there is no need for writer to wait for readers.
//              amount of simultaneous pushes will never be more than the size of the container.
//              amount of push operations will never be more than uint64_t can hold.
//
// ATTENTION: may have a bug.
//
// TODO:      maybe fix false sharing on the done_flag_.
//
template<class T>
class CircularBufferFast {
 public:
  using Elem = T;

  CircularBufferFast(size_t size)
    : buffer_(size)
    , done_flag_(size)
  { }

  size_t size() const { return buffer_.size(); }

  void push(T data) {
    auto writer_index = writer_started_.fetch_add(1);
    buffer_[writer_index % buffer_.size()] = std::move(data);
    release_writer(writer_index);
  }

  //
  // never timeouts
  //
  template<class Clock, class Duration>
  std::optional<T> pop(const std::chrono::time_point<Clock, Duration>& /* timeout_time */) {
    auto reader_index = reader_.fetch_add(1);
    while (reader_index + 1 > writer_finished_.load()) {
      // wait for a writer to finish.
    }
    return std::move(buffer_[reader_index % buffer_.size()]);
  }

 private:
  inline void release_writer(size_t index) {
    // we can use one flag vector because writers and readers tickets never intersects
    done_flag_[index % buffer_.size()].store(true);
    size_t cur_value = index;
    while (writer_finished_.compare_exchange_weak(cur_value, cur_value + 1)) {
      // help other workers to move finished flag if they finished before us.
      done_flag_[index % buffer_.size()].store(false);
      cur_value = (cur_value + 1) % buffer_.size();
      if (!done_flag_[cur_value].load()) {
        break;
      }
    }
  }

  std::vector<T> buffer_;
  std::vector<std::atomic<bool>> done_flag_;
  std::atomic<uint64_t> writer_started_ = 0;
  std::atomic<uint64_t> writer_finished_ = 0;
  std::atomic<uint64_t> reader_ = 0;
};


//
// CircularBuffer  is a thread safe class (not lock free) that allows to push and pop data
//                 in a queue manner.
//                 Doesn't go into the kernal at any point.
//
// push   adds data to the end of the buffer and may block if trying to access memory that pop
//        currently reads from. Also push blocks when there are too much values that wasn't read from
//        container.
//        Without readers only size - 1 pushes may be done before push blocks.
//
// pop    gets value from the front of the container and may block and wait for data to appear.
//
// ATTENTION: may have a bug.
//
// TODO:      maybe fix false sharing on the done_flag_.
//            also maybe need to optimize traffic between caches.
//
template<class T>
class CircularBuffer {
 public:
  using Elem = T;

  CircularBuffer(size_t size)
   : buffer_(size)
   , done_flag_(size)
  { }

  size_t size() const { return buffer_.size(); }

  void push(T data) {
    size_t writer_ticket;
    size_t next_writer;
    do {
      auto writer_ticket = writer_started_.load();
      // (writer + 1) is needed to distinguish between full and empty buffer_ cases.
      // it allows to push only size - 1 elems before blocking on push() without readers.
      while ((writer_ticket + 1) % buffer_.size() == reader_started_.load()) {
        writer_ticket = writer_started_.load();
      }
      next_writer = (writer_ticket + 1) % buffer_.size();
    } while (!writer_started_.compare_exchange_weak(writer_ticket, next_writer));

    buffer_[writer_ticket] = std::move(data);

    release_ticket(writer_finished_, writer_ticket);
  }

  //
  // never timeouts
  //
  template<class Clock, class Duration>
  std::optional<T> pop(const std::chrono::time_point<Clock, Duration>& /* timeout_time */) {
    size_t reader_ticket;
    size_t next_reader;
    do {
      reader_ticket = reader_started_.load();
      while (reader_ticket == writer_started_.load()) {
        reader_ticket = reader_started_.load();
      }
      next_reader = (reader_ticket + 1) % buffer_.size();
    } while(!reader_started_.compare_exchange_weak(reader_ticket, next_reader));

    auto res = std::move(buffer_[reader_ticket]);
    release_ticket(reader_finished_, reader_ticket);
    return std::move(res);
  }

 private:
  inline void release_ticket(std::atomic<size_t>& finished, size_t ticket) {
    // we can use one flag vector because writers and readers tickets never intersects
    done_flag_[ticket].store(true);
    auto cur_value = ticket;
    while (finished.compare_exchange_strong(cur_value, (cur_value + 1) % buffer_.size())) {
      // help other workers to move finished flag if they finished before us.
      done_flag_[cur_value].store(false);
      cur_value = (cur_value + 1) % buffer_.size();
      if (!done_flag_[cur_value].load()) {
        break;
      }
    }
  }

  std::vector<T> buffer_;
  std::vector<std::atomic<bool>> done_flag_;
  std::atomic<size_t> writer_started_ = 0;
  std::atomic<size_t> writer_finished_ = 0;
  std::atomic<size_t> reader_started_ = 0;
  std::atomic<size_t> reader_finished_ = 0;
};

} // namespace vadelve::concurrency
