cmake_minimum_required(VERSION 3.15)
project(vadelve_concurrency)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_compile_definitions(CATCH_CONFIG_ENABLE_BENCHMARKING)

string(APPEND CMAKE_CXX_FLAGS " -std=c++20 -Wall -Wno-narrowing -g ")


enable_testing()

add_subdirectory(external)
add_subdirectory(src)
