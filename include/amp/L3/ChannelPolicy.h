#pragma once

#include "amp/L3/AmpChannelLimits.h"
#include "amp/L3/Types.h"

#include <chrono>
#include <cstddef>
#include <functional>

namespace pp::amp {

enum class ChannelDropPolicy {
  Never,
  Oldest,
};

struct ChannelPolicy {
  ChannelClass cls = ChannelClass::Control;
  ChannelDropPolicy drop = ChannelDropPolicy::Never;
  size_t max_outbound_frames = 0;
  bool write_preferred = false;
  bool read_once = false;
  size_t max_message_bytes = AmpChannelLimits::kMaxChatStreamJsonBytes;
  std::chrono::milliseconds read_timeout{0};
  std::function<void()> on_outbound_drop;
};

/** Generic reliable JSON / control RPC profile (chat, history, dial-back, …). */
inline ChannelPolicy ControlJsonChannelPolicy(
    std::chrono::milliseconds read_timeout = std::chrono::milliseconds{8000}) {
  ChannelPolicy policy;
  policy.cls = ChannelClass::Control;
  policy.drop = ChannelDropPolicy::Never;
  policy.max_outbound_frames = AmpChannelLimits::kMaxControlOutboundFrames;
  policy.read_once = true;
  policy.max_message_bytes = AmpChannelLimits::kMaxChatStreamJsonBytes;
  policy.read_timeout = read_timeout;
  return policy;
}

inline ChannelPolicy CapabilityChannelPolicy() {
  ChannelPolicy policy;
  policy.cls = ChannelClass::Control;
  policy.drop = ChannelDropPolicy::Never;
  policy.max_outbound_frames = 1;
  policy.max_message_bytes = AmpChannelLimits::kMaxControlJsonFrameBytes;
  return policy;
}

/** Chat attachment / large binary (FRAG up to AmpChannelLimits::kMaxChatBlobFrameBytes). */
inline ChannelPolicy ChatBlobChannelPolicy() {
  ChannelPolicy policy;
  policy.cls = ChannelClass::Bulk;
  policy.drop = ChannelDropPolicy::Never;
  policy.max_outbound_frames = AmpChannelLimits::kMaxControlOutboundFrames;
  policy.max_message_bytes = AmpChannelLimits::kMaxChatBlobFrameBytes;
  return policy;
}

/**
 * Outer splice for nested Session carrier ([A024]).
 * BestEffort (Realtime) so inner media FRAG bursts are not capped by ADP reliable_window.
 * Nested MSH is a few frames; MemoryDatagramIo tests are lossless. Dual QoS lanes later.
 */
inline ChannelPolicy CircuitCarrierChannelPolicy(
    std::chrono::milliseconds read_timeout = std::chrono::milliseconds{8000}) {
  ChannelPolicy policy;
  policy.cls = ChannelClass::Realtime;
  policy.drop = ChannelDropPolicy::Oldest;
  policy.max_outbound_frames = AmpChannelLimits::kMaxCallMediaOutboundFrames;
  policy.write_preferred = true;
  policy.read_once = false;
  policy.max_message_bytes = AmpChannelLimits::kMaxCallMediaFrameBytes;
  policy.read_timeout = read_timeout;
  return policy;
}

} // namespace pp::amp
