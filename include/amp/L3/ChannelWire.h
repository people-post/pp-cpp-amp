#pragma once

#include "amp/L3/Types.h"


#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace pp::amp {

struct ChannelHeader {
  uint8_t frame_version = kChannelFrameVersion;
  ChannelFrameType frame_type = ChannelFrameType::Data;
  uint32_t channel_id = 0;
  uint32_t channel_seq = 0;
};

struct ChannelOpenBody {
  std::string protocol_id;
  ChannelClass channel_class = ChannelClass::Control;
  uint16_t flags = 0;
  /** Receiver reassembly budget; 0 means use ChannelPolicy default on the peer. */
  uint32_t max_message_bytes = 0;
};

struct ChannelFragBody {
  uint64_t msg_id = 0;
  uint16_t frag_index = 0;
  uint16_t frag_count = 0;
  uint32_t total_len = 0;
  std::vector<uint8_t> chunk;
};

struct ChannelFrame {
  ChannelHeader header;
  std::vector<uint8_t> payload;
  ChannelOpenBody open;
  uint8_t open_ack_result = 0;
  uint32_t reset_code = 0;
  ChannelFragBody frag;
};

class ChannelWire {
public:
  static Roe<std::vector<uint8_t>> Encode(const ChannelFrame& frame);
  /** Encode a FRAG frame from a payload span (avoids copying into ChannelFragBody::chunk). */
  static Roe<std::vector<uint8_t>> EncodeFrag(const ChannelHeader& header, uint64_t msg_id, uint16_t frag_index,
                                              uint16_t frag_count, uint32_t total_len,
                                              std::span<const uint8_t> chunk);
  static Roe<ChannelFrame> Decode(std::span<const uint8_t> wire);

  static Roe<std::vector<uint8_t>> EncodeLenUtf8Le(std::string_view text);
  static Roe<std::string> DecodeLenUtf8Le(std::span<const uint8_t> wire, size_t& consumed);
};

} // namespace pp::amp
