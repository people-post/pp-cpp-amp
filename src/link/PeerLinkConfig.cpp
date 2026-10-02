#include "amp/link/Types.h"

#include <string>

namespace pp::amp {

Roe<void> ValidatePeerLinkConfig(const PeerLinkConfig& config) {
  const auto& adp = config.adp;
  if (adp.reliable_window == 0) {
    return Error("amp config: adp.reliable_window must be > 0");
  }
  // A packet the peer may legitimately have in flight must never be "too far".
  if (adp.replay_window < adp.reliable_window) {
    return Error("amp config: adp.replay_window (" + std::to_string(adp.replay_window) +
                 ") must be >= adp.reliable_window (" + std::to_string(adp.reliable_window) + ")");
  }
  if (adp.rtx_interval_ms <= 0 || adp.max_rtx <= 0) {
    return Error("amp config: adp.rtx_interval_ms and adp.max_rtx must be > 0");
  }
  if (adp.skew_ms <= 0) {
    return Error("amp config: adp.skew_ms must be > 0");
  }
  // A live link must survive a couple of retransmit rounds of silence.
  if (adp.alive_timeout_ms < 2 * adp.rtx_interval_ms) {
    return Error("amp config: adp.alive_timeout_ms must be >= 2 x adp.rtx_interval_ms");
  }
  const auto& mux = config.mux;
  if (mux.max_concurrent_channels == 0) {
    return Error("amp config: mux.max_concurrent_channels must be > 0");
  }
  if (mux.max_queued_bytes < AmpChannelLimits::kMaxControlJsonFrameBytes) {
    return Error("amp config: mux.max_queued_bytes must hold at least one control message (" +
                 std::to_string(AmpChannelLimits::kMaxControlJsonFrameBytes) + " bytes)");
  }
  // A fragment may need the whole retransmit budget before it arrives.
  if (mux.frag_assembly_timeout_ms < adp.rtx_interval_ms * adp.max_rtx) {
    return Error("amp config: mux.frag_assembly_timeout_ms must be >= adp.rtx_interval_ms x adp.max_rtx");
  }
  return {};
}

} // namespace pp::amp
