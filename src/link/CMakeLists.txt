pp_amp_add_library(pp_amp_link
  AdpMultiaddr.cpp
  AmpAdpCarrier.cpp
  MeshPump.cpp
  MeshRuntime.cpp
  AmpStack.cpp
  MshAdpHandshake.cpp
  PeerLink.cpp
  PeerLinkManager.cpp
  Types.cpp)

target_link_libraries(pp_amp_link PUBLIC pp_amp_l3 pp_amp_l2 pp_amp_l1 pp_crypto sodium)

if(PP_AMP_BUILD_TESTS)
  add_subdirectory(tests)
endif()
