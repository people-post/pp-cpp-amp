#pragma once

#include <cstddef>

namespace pp::amp {

/**
 * Shared AMP channel I/O budgets (frame sizes + outbound queue caps).
 * Formerly `Libp2pExecutorLimits` under `base/p2p` — kept in mesh so L3 does not
 * depend on product p2p (pp-cpp-amp extract readiness).
 */
struct AmpChannelLimits {
  /** Control-plane JSON (dial-back, circuit-relay RPC). */
  static constexpr size_t kMaxControlJsonFrameBytes = 64 * 1024;
  /** Chat / relay envelope streams (direct chat, chat-history, dial-back). */
  static constexpr size_t kMaxChatStreamJsonBytes = 256 * 1024;
  /** Chat attachment ciphertext (≤ 4 MiB plaintext + AEAD overhead). */
  static constexpr size_t kMaxChatBlobFrameBytes = 4ULL * 1024ULL * 1024ULL + 64 * 1024;
  /** Media-relay binary data frames. */
  static constexpr size_t kMaxMediaDataFrameBytes = 256 * 1024;
  /** Call-media encrypted Opus / H264 video_lo frames (V034; was 16 KiB audio-only). */
  static constexpr size_t kMaxCallMediaFrameBytes = 128 * 1024;
  /** Call-media outbound queue cap. */
  static constexpr size_t kMaxCallMediaOutboundFrames = 64;
  /** Media-relay hop fanout: audio + latest video. */
  static constexpr size_t kMaxMediaRelayOutboundFrames = 2;
  /** Media-relay client (phone→hop): subscribe JSON + audio + IDR headroom. */
  static constexpr size_t kMaxMediaRelayClientOutboundFrames = 6;
  /** Control JSON (chat / history): one request or ack queued. */
  static constexpr size_t kMaxControlOutboundFrames = 1;
};

} // namespace pp::amp
