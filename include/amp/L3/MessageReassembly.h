#pragma once

#include "amp/L3/ChannelWire.h"
#include "amp/L3/Types.h"


#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace pp::amp {

/** Reassemble FRAG frames into one L4 message (Reliable channels). */
class MessageReassembly {
public:
  /** max_partials bounds distinct msg_ids assembling concurrently (one per in-flight message). */
  explicit MessageReassembly(size_t max_message_bytes = 256 * 1024, size_t max_partials = 64);

  /** Returns complete message when assembly finishes. */
  Roe<std::optional<std::vector<uint8_t>>> Push(const ChannelFragBody& frag, int64_t now_ms);

  void SweepExpired(int64_t now_ms, int64_t timeout_ms = kDefaultFragAssemblyTimeoutMs);

private:
  struct Partial {
    uint16_t frag_count = 0;
    uint32_t total_len = 0;
    std::vector<std::vector<uint8_t>> chunks;
    uint32_t received_bytes = 0;
    int64_t started_ms = 0;
  };

  size_t max_message_bytes_;
  size_t max_partials_;
  std::map<uint64_t, Partial> partial_;
};

} // namespace pp::amp
