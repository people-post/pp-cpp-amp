#pragma once

#include "amp/L1/Types.h"

#include <cstdint>
#include <string>

namespace pp::amp {

/**
 * A frame's header: type, channel id, channel seq (no version byte: the packet's first byte carries the wire
 * version). It travels sealed, inside the packet (A029 H1).
 */
inline constexpr size_t kChannelHeaderBytes = 1 + 4 + 4;
/** A fragment's fields after the header: message id, index, count, total length. */
inline constexpr size_t kChannelFragFieldsBytes = 8 + 2 + 2 + 4;
inline constexpr uint32_t kCapabilityChannelId = 0;
/** Protocol of the capability channel each link keeps open (link plumbing, not a request). */
inline constexpr const char* kCapabilityProtocolId = "/pp-amp/capability/1.0.0";
inline constexpr uint32_t kIllegalChannelId = 0xFFFF'FFFFu;
inline constexpr int64_t kDefaultFragAssemblyTimeoutMs = 30'000;
/** A lossy (BestEffort) channel's partial message expires sooner: its sender never resends a fragment. */
inline constexpr int64_t kLossyFragAssemblyTimeoutMs = 1'000;

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
