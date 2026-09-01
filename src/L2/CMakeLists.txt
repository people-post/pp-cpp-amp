pp_amp_add_library(pp_amp_l2
  MshHandshake.cpp
  MshMessages.cpp
  Session.cpp
  SessionControl.cpp
  SessionAead.cpp
  SessionCrypto.cpp
  SessionKeys.cpp)

target_link_libraries(pp_amp_l2 PUBLIC pp_amp_l1 pp_crypto sodium)

if(PP_AMP_BUILD_TESTS)
  add_subdirectory(tests)
endif()
