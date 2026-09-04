set(_PP_AMP_PERF_SUPPORT
  ${PP_AMP_SOURCE_ROOT}/tests/support/mesh_harness_support.cpp
  ${PP_AMP_SOURCE_ROOT}/tests/support/mesh_test_harness.cpp
  ${PP_AMP_SOURCE_ROOT}/tests/support/amp_integration_harness.cpp)

add_executable(pp_amp_perf
  perf_main.cpp
  perf_more.cpp
  perf_finish.cpp
  ${_PP_AMP_PERF_SUPPORT})

target_include_directories(pp_amp_perf PRIVATE
  ${PP_AMP_SOURCE_ROOT}/include
  ${PP_AMP_SOURCE_ROOT}/tests
  ${PP_AMP_SOURCE_ROOT}/tests/perf
  ${PP_AMP_SOURCE_ROOT}/src)

target_link_libraries(pp_amp_perf PRIVATE
  pp_amp_link
  pp_crypto
  sodium
  pp_common)

add_test(NAME pp_amp_perf COMMAND pp_amp_perf)
