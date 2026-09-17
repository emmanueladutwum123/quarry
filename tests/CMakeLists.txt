add_executable(quarry_tests
  test_main.cpp
  test_bitpack.cpp
  test_encoding.cpp
  test_expression.cpp
  test_segment.cpp
)
target_link_libraries(quarry_tests PRIVATE quarry quarry_flags)
target_include_directories(quarry_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME quarry_tests COMMAND quarry_tests)
