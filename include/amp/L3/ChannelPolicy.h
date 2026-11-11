#pragma once

#include "amp/L3/AmpChannelLimits.h"
#include "amp/L3/Types.h"

#include <chrono>
#include <cstddef>
#include <functional>

namespace pp::amp {

/**
 * Scheduled keepalive tier on a connected link (see docs/KEEPALIVE.md). A link keeps the highest tier
 * that its product mark (PeerLinkManager::MarkWarm / MarkHot) or any channel open on it (ChannelPolicy::keepalive)
 * asks for.
 */
enum class KeepaliveTier {
  None = 0,
  Warm = 1,
  Hot = 2,
};

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
  /**
   * Reset the channel when nothing arrives on it for this long (0 = never). The
   * clock runs from open / Bind and restarts on every inbound DATA or FRAG, so on
   * a request/response channel it also covers the wait for the reply. The reader
   * gets a terminal notice with ChannelMux::kReadTimeoutReason; the peer a RESET.
   * Checked on ChannelMux::Tick, so it fires up to one tick late.
   */
  std::chrono::milliseconds read_timeout{0};
  std::function<void()> on_outbound_drop;
  /**
   * While this channel is open (bound), its link keeps at least this keepalive tier: a link that must stay
   * alive because of what rides it (a call's standby path) says so by the channel, and the tier goes when
   * the channel closes — whatever else shares the link keeps its own.
   */
  KeepaliveTier keepalive = KeepaliveTier::None;
};

/** Generic reliable JSON / control RPC profile (chat, history, dial-back, …). */
inline ChannelPolicy ControlJsonChannelPolicy(std::chrono::milliseconds read_timeout = std::chrono::milliseconds{
                                                  8000}) {
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

/**
 * Core Bulk profile (FRAG up to AmpChannelLimits::kMaxBulkFrameBytes).
 * Product wrappers may copy this and set read_once / read_timeout.
 */
inline ChannelPolicy MakeBulkChannelPolicy() {
  ChannelPolicy policy;
  policy.cls = ChannelClass::Bulk;
  policy.drop = ChannelDropPolicy::Never;
  policy.max_outbound_frames = AmpChannelLimits::kMaxControlOutboundFrames;
  policy.max_message_bytes = AmpChannelLimits::kMaxBulkFrameBytes;
  return policy;
}

/** Large Reliable binary / content-addressed object transfer (product-neutral). */
inline ChannelPolicy BulkChannelPolicy() {
  return MakeBulkChannelPolicy();
}

/**
 * Call-media / realtime frames: BestEffort + drop Oldest under outbound burst.
   * No read timeout by default: a muted or paused stream is legitimately silent. No keepalive tier: carriers
 * and relay data lanes build on it, and their links' tiers are their owners' (a call's own control channel
 * keeps its path hot — pp-cpp-mesh CallMediaControlChannelPolicy; a carrier's link keeps the tier of the
 * nested link it carries — docs/KEEPALIVE.md).
 */
inline ChannelPolicy CallMediaChannelPolicy(std::chrono::milliseconds read_timeout = std::chrono::milliseconds{0}) {
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

/**
 * Outer splice for nested Session carrier ([A024]).
 * BestEffort (Realtime) so inner media FRAG bursts are not capped by ADP reliable_window.
 * Nested MSH is a few frames; MemoryDatagramIo tests are lossless. Dual QoS lanes later.
 */
inline ChannelPolicy CircuitCarrierChannelPolicy(std::chrono::milliseconds read_timeout = std::chrono::milliseconds{
                                                     0}) {
  return CallMediaChannelPolicy(read_timeout);
}

} // namespace pp::amp
