pp_amp_add_library(pp_amp_l1
  Clock.cpp
  Connection.cpp
  Endpoint.cpp
  HmacBinder.cpp
  MemoryDatagramIo.cpp
  OsUdpDatagramIo.cpp
  ReplayWindow.cpp
  WireCodec.cpp)

target_link_libraries(pp_amp_l1 PUBLIC pp_crypto sodium)

if(WIN32)
  target_link_libraries(pp_amp_l1 PUBLIC ws2_32)
endif()

if(PP_AMP_BUILD_TESTS)
  add_subdirectory(tests)
endif()
