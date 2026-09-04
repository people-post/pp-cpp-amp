set(_PP_AMP_PERF_SOURCES
  perf_main.cpp)

add_executable(pp_amp_perf ${_PP_AMP_PERF_SOURCES})

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
