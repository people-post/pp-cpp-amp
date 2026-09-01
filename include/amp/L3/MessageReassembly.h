#pragma once

#include "amp/L3/ChannelWire.h"


#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace pp::amp {

/** Reassemble FRAG frames into one L4 message (Reliable channels). */
class MessageReassembly {
public:
  explicit MessageReassembly(size_t max_message_bytes = 256 * 1024);

  /** Returns complete message when assembly finishes. */
  Roe<std::optional<std::vector<uint8_t>>> Push(const ChannelFragBody& frag, int64_t now_ms);

  void SweepExpired(int64_t now_ms);

private:
  struct Partial {
    uint16_t frag_count = 0;
    uint32_t total_len = 0;
    std::vector<std::vector<uint8_t>> chunks;
    int64_t started_ms = 0;
  };

  size_t max_message_bytes_;
  std::map<uint64_t, Partial> partial_;
};

} // namespace pp::amp
