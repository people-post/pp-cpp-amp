# pp-cpp-common + pp-cpp-crypto (sibling checkout or FetchContent).

include(FetchContent)

set(PP_CPP_COMMON_SOURCE_DIR "" CACHE PATH
  "Optional local checkout of pp-cpp-common (overrides FetchContent)")
set(PP_CPP_CRYPTO_SOURCE_DIR "" CACHE PATH
  "Optional local checkout of pp-cpp-crypto (overrides FetchContent)")

set(PP_CPP_COMMON_GIT_REPOSITORY "https://github.com/people-post/pp-cpp-common.git"
  CACHE STRING "Git remote for pp-cpp-common")
set(PP_CPP_COMMON_GIT_TAG "v0.2.0"
  CACHE STRING "Release tag on pp-cpp-common")
set(PP_CPP_CRYPTO_GIT_REPOSITORY "https://github.com/people-post/pp-cpp-crypto.git"
  CACHE STRING "Git remote for pp-cpp-crypto")
set(PP_CPP_CRYPTO_GIT_TAG "v0.2.0"
  CACHE STRING "Release tag on pp-cpp-crypto")

set(PP_COMMON_BUILD_TESTS OFF CACHE BOOL "Build pp-cpp-common tests" FORCE)
set(PP_CRYPTO_BUILD_TESTS OFF CACHE BOOL "Build pp-cpp-crypto tests" FORCE)

if(NOT TARGET pp_common)
  if(PP_CPP_COMMON_SOURCE_DIR)
    add_subdirectory("${PP_CPP_COMMON_SOURCE_DIR}"
                     "${CMAKE_BINARY_DIR}/_deps/pp_cpp_common-build" EXCLUDE_FROM_ALL)
  elseif(EXISTS "${CMAKE_SOURCE_DIR}/../pp-cpp-common/CMakeLists.txt")
    add_subdirectory("${CMAKE_SOURCE_DIR}/../pp-cpp-common"
                     "${CMAKE_BINARY_DIR}/_deps/pp_cpp_common-build" EXCLUDE_FROM_ALL)
  else()
    FetchContent_Declare(
      pp_cpp_common
      GIT_REPOSITORY ${PP_CPP_COMMON_GIT_REPOSITORY}
      GIT_TAG ${PP_CPP_COMMON_GIT_TAG}
    )
    FetchContent_MakeAvailable(pp_cpp_common)
  endif()
endif()

if(NOT TARGET pp_crypto)
  if(PP_CPP_CRYPTO_SOURCE_DIR)
    add_subdirectory("${PP_CPP_CRYPTO_SOURCE_DIR}"
                     "${CMAKE_BINARY_DIR}/_deps/pp_cpp_crypto-build" EXCLUDE_FROM_ALL)
  elseif(EXISTS "${CMAKE_SOURCE_DIR}/../pp-cpp-crypto/CMakeLists.txt")
    add_subdirectory("${CMAKE_SOURCE_DIR}/../pp-cpp-crypto"
                     "${CMAKE_BINARY_DIR}/_deps/pp_cpp_crypto-build" EXCLUDE_FROM_ALL)
  else()
    FetchContent_Declare(
      pp_cpp_crypto
      GIT_REPOSITORY ${PP_CPP_CRYPTO_GIT_REPOSITORY}
      GIT_TAG ${PP_CPP_CRYPTO_GIT_TAG}
    )
    FetchContent_MakeAvailable(pp_cpp_crypto)
  endif()
endif()

if(NOT TARGET pp_common)
  message(FATAL_ERROR "pp-cpp-amp requires target pp_common")
endif()
if(NOT TARGET pp_crypto OR NOT TARGET sodium)
  message(FATAL_ERROR "pp-cpp-amp requires pp_crypto and sodium")
endif()
