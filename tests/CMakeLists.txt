if(NOT PP_AMP_BUILD_TESTS)
  return()
endif()

add_subdirectory(integration)
add_subdirectory(perf)
