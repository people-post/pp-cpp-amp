# Helpers for pp-cpp-amp libraries and layer unit tests.

function(pp_amp_add_library target)
  add_library(${target} STATIC ${ARGN})
  target_include_directories(${target} PUBLIC
    $<BUILD_INTERFACE:${PP_AMP_SOURCE_ROOT}/include>
    $<INSTALL_INTERFACE:include>)
  target_link_libraries(${target} PUBLIC pp_common)
  if(MSVC)
    target_compile_definitions(${target} PUBLIC NOMINMAX)
  endif()
endfunction()

function(pp_amp_add_layer_tests lib_target test_target)
  cmake_parse_arguments(ARG "" "" "EXTRA_SOURCES;LINK_LIBS" ${ARGN})
  if(NOT PP_AMP_BUILD_TESTS)
    return()
  endif()

  file(GLOB _test_sources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp")
  if(NOT _test_sources)
    return()
  endif()

  add_executable(${test_target} ${_test_sources} ${ARG_EXTRA_SOURCES})
  target_include_directories(${test_target} PRIVATE
    ${PP_AMP_SOURCE_ROOT}/include
    ${PP_AMP_SOURCE_ROOT}/tests
    ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(${test_target} PRIVATE
    GTest::gtest
    GTest::gtest_main
    ${lib_target}
    pp_common
    ${ARG_LINK_LIBS})
  include(GoogleTest)
  gtest_discover_tests(${test_target})
endfunction()
