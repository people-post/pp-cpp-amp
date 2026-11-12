#pragma once

#include "amp/L3/ChannelWire.h"
#include "amp/L3/Types.h"

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace pp::amp {

/**
 * Reassemble FRAG frames into one L4 message.
 *
 * Reliable channels never lose a fragment: a partial over the cap is a misbehaving peer (an error, and the mux resets
 * the channel). On a lossy channel (BestEffort) a lost fragment leaves its message incomplete for good — that message
 * is gone, nothing else is wrong: past the cap the oldest partial is dropped to make room, and partials expire after
 * kLossyFragAssemblyTimeoutMs (a media frame that old is useless). Before, a lossy channel reset itself once 64 lost
 * messages had piled up: at 3 % loss a call's media lane died within seconds.
 */
class MessageReassembly {
public:
  /** max_partials bounds distinct msg_ids assembling concurrently (one per in-flight message). */
  explicit MessageReassembly(size_t max_message_bytes = 256 * 1024, size_t max_partials = 64, bool lossy = false);

  bool Lossy() const { return lossy_; }

  /** Returns complete message when assembly finishes. */
  Roe<std::optional<std::vector<uint8_t>>> Push(const ChannelFragBody& frag, int64_t now_ms);

  void SweepExpired(int64_t now_ms, int64_t timeout_ms = kDefaultFragAssemblyTimeoutMs);
  /** When the oldest partial began (SweepExpired drops it `timeout_ms` after); nullopt: none assembling. */
  std::optional<int64_t> OldestStartedMs() const;

private:
  struct Partial {
    uint16_t frag_count = 0;
    uint32_t total_len = 0;
    std::vector<std::vector<uint8_t>> chunks;
    uint32_t received_bytes = 0;
    int64_t started_ms = 0;
  };

  /** Drop the oldest partial (by start) — lossy channels only. */
  void DropOldestPartial();

  size_t max_message_bytes_;
  size_t max_partials_;
  bool lossy_ = false;
  std::map<uint64_t, Partial> partial_;
};

} // namespace pp::amp
