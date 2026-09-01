pp_amp_add_library(pp_amp_l3
  Capability.cpp
  ChannelBridge.cpp
  ChannelMux.cpp
  ChannelSession.cpp
  ChannelWire.cpp
  MessageReassembly.cpp)

target_link_libraries(pp_amp_l3 PUBLIC pp_amp_l2 pp_amp_l1)

if(PP_AMP_BUILD_TESTS)
  add_subdirectory(tests)
endif()
