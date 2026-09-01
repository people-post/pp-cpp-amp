#pragma once

#include "amp/L1/Types.h"

#include <cstdint>
#include <string>

namespace pp::amp {

inline constexpr uint8_t kChannelFrameVersion = 1;
inline constexpr uint32_t kCapabilityChannelId = 0;
inline constexpr uint32_t kIllegalChannelId = 0xFFFF'FFFFu;
inline constexpr int64_t kDefaultFragAssemblyTimeoutMs = 30'000;

enum class ChannelFrameType : uint8_t {
  Open = 0,
  OpenAck = 1,
  Data = 2,
  Close = 3,
  Reset = 4,
  Frag = 5,
};

enum class ChannelClass : uint8_t {
  Transactional = 0,
  Control = 1,
  Bulk = 2,
  Realtime = 3,
  RealtimeControl = 4,
};

enum class ChannelState : uint8_t {
  Closed = 0,
  Opening = 1,
  Open = 2,
  Closing = 3,
};

inline adp::QosClass QosForClass(ChannelClass cls) {
  switch (cls) {
  case ChannelClass::Realtime:
    return adp::QosClass::BestEffort;
  default:
    return adp::QosClass::Reliable;
  }
}

} // namespace pp::amp
