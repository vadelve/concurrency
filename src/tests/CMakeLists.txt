add_executable(circular_buffer_test circular_buffer_test.cpp)
target_link_libraries(circular_buffer_test PRIVATE circular_buffer_mt pthread Catch2::Catch2WithMain)

add_test(
  NAME circular_buffer_t
  COMMAND ${CMAKE_BINARY_DIR}/bin/circular_buffer_test
)
