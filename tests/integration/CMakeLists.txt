set(_PP_AMP_TEST_SUPPORT
  ${CMAKE_SOURCE_DIR}/tests/support/mesh_harness_support.cpp
  ${CMAKE_SOURCE_DIR}/tests/support/mesh_test_harness.cpp
  ${CMAKE_SOURCE_DIR}/tests/support/amp_integration_harness.cpp)

add_executable(pp_amp_integration_test
  amp_integration_test.cpp
  ${_PP_AMP_TEST_SUPPORT})

target_include_directories(pp_amp_integration_test PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/tests)

target_link_libraries(pp_amp_integration_test PRIVATE
  GTest::gtest
  GTest::gtest_main
  pp_amp_link
  pp_crypto
  sodium
  pp_common)

include(GoogleTest)
gtest_discover_tests(pp_amp_integration_test)
