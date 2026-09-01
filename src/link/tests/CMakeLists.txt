set(_PP_AMP_TEST_SUPPORT
  ${PP_AMP_SOURCE_ROOT}/tests/support/mesh_harness_support.cpp
  ${PP_AMP_SOURCE_ROOT}/tests/support/mesh_test_harness.cpp)

pp_amp_add_layer_tests(pp_amp_link pp_amp_link_test
  EXTRA_SOURCES ${_PP_AMP_TEST_SUPPORT})
