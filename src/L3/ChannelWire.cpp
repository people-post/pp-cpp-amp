#include "amp/L3/ChannelWire.h"

namespace pp::amp {

namespace {

void AppendU32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(static_cast<uint8_t>(v));
  out.push_back(static_cast<uint8_t>(v >> 8));
  out.push_back(static_cast<uint8_t>(v >> 16));
  out.push_back(static_cast<uint8_t>(v >> 24));
}

void AppendU16(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back(static_cast<uint8_t>(v));
  out.push_back(static_cast<uint8_t>(v >> 8));
}

void AppendU64(std::vector<uint8_t>& out, uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<uint8_t>(v >> (8 * i)));
  }
}

Roe<uint32_t> ReadU32(std::span<const uint8_t>& wire) {
  if (wire.size() < 4) {
    return Error("amp ch: truncated u32");
  }
  uint32_t v = static_cast<uint32_t>(wire[0]) | (static_cast<uint32_t>(wire[1]) << 8)
               | (static_cast<uint32_t>(wire[2]) << 16) | (static_cast<uint32_t>(wire[3]) << 24);
  wire = wire.subspan(4);
  return v;
}

Roe<uint16_t> ReadU16(std::span<const uint8_t>& wire) {
  if (wire.size() < 2) {
    return Error("amp ch: truncated u16");
  }
  uint16_t v = static_cast<uint16_t>(wire[0]) | (static_cast<uint16_t>(wire[1]) << 8);
  wire = wire.subspan(2);
  return v;
}

Roe<uint64_t> ReadU64(std::span<const uint8_t>& wire) {
  if (wire.size() < 8) {
    return Error("amp ch: truncated u64");
  }
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v |= static_cast<uint64_t>(wire[static_cast<size_t>(i)]) << (8 * i);
  }
  wire = wire.subspan(8);
  return v;
}

Roe<void> ExpectEmpty(std::span<const uint8_t> wire) {
  if (!wire.empty()) {
    return Error("amp ch: trailing bytes");
  }
  return Roe<void>();
}

} // namespace

Roe<std::vector<uint8_t>> ChannelWire::EncodeLenUtf8Le(std::string_view text) {
  if (text.size() > 0xFFFF'FFFFu) {
    return Error("amp ch: string too long");
  }
  std::vector<uint8_t> out;
  AppendU32(out, static_cast<uint32_t>(text.size()));
  out.insert(out.end(), text.begin(), text.end());
  return out;
}

Roe<std::string> ChannelWire::DecodeLenUtf8Le(std::span<const uint8_t> wire, size_t& consumed) {
  consumed = 0;
  auto span = wire;
  auto len = ReadU32(span);
  if (!len) {
    return len.error();
  }
  if (span.size() < *len) {
    return Error("amp ch: truncated utf8");
  }
  std::string out(reinterpret_cast<const char*>(span.data()), *len);
  consumed = wire.size() - span.size() + *len;
  return out;
}

Roe<std::vector<uint8_t>> ChannelWire::Encode(const ChannelFrame& frame) {
  if (frame.header.frame_type == ChannelFrameType::Frag) {
    return EncodeFrag(frame.header, frame.frag.msg_id, frame.frag.frag_index, frame.frag.frag_count,
                      frame.frag.total_len, frame.frag.chunk);
  }
  if (frame.header.frame_version != kChannelFrameVersion) {
    return Error("amp ch: bad frame version");
  }
  std::vector<uint8_t> out;
  out.reserve(10 + frame.payload.size() + 64);
  out.push_back(frame.header.frame_version);
  out.push_back(static_cast<uint8_t>(frame.header.frame_type));
  AppendU32(out, frame.header.channel_id);
  AppendU32(out, frame.header.channel_seq);

  switch (frame.header.frame_type) {
  case ChannelFrameType::Open: {
    auto proto = EncodeLenUtf8Le(frame.open.protocol_id);
    if (!proto) {
      return proto.error();
    }
    out.insert(out.end(), proto->begin(), proto->end());
    out.push_back(static_cast<uint8_t>(frame.open.channel_class));
    AppendU16(out, frame.open.flags);
    AppendU32(out, frame.open.max_message_bytes);
    break;
  }
  case ChannelFrameType::OpenAck:
    out.push_back(frame.open_ack_result);
    break;
  case ChannelFrameType::Data:
    out.insert(out.end(), frame.payload.begin(), frame.payload.end());
    break;
  case ChannelFrameType::Close:
    if (!frame.payload.empty()) {
      auto reason = EncodeLenUtf8Le(std::string(frame.payload.begin(), frame.payload.end()));
      if (!reason) {
        return reason.error();
      }
      out.insert(out.end(), reason->begin(), reason->end());
    }
    break;
  case ChannelFrameType::Reset:
    AppendU32(out, frame.reset_code);
    break;
  case ChannelFrameType::Frag:
    // Handled above.
    break;
  default:
    return Error("amp ch: unknown frame type");
  }
  return out;
}

Roe<std::vector<uint8_t>> ChannelWire::EncodeFrag(const ChannelHeader& header, const uint64_t msg_id,
                                                  const uint16_t frag_index, const uint16_t frag_count,
                                                  const uint32_t total_len, const std::span<const uint8_t> chunk) {
  if (header.frame_version != kChannelFrameVersion) {
    return Error("amp ch: bad frame version");
  }
  std::vector<uint8_t> out;
  out.reserve(10 + 16 + chunk.size());
  out.push_back(header.frame_version);
  out.push_back(static_cast<uint8_t>(ChannelFrameType::Frag));
  AppendU32(out, header.channel_id);
  AppendU32(out, header.channel_seq);
  AppendU64(out, msg_id);
  AppendU16(out, frag_index);
  AppendU16(out, frag_count);
  AppendU32(out, total_len);
  out.insert(out.end(), chunk.begin(), chunk.end());
  return out;
}

Roe<ChannelFrame> ChannelWire::Decode(std::span<const uint8_t> wire) {
  if (wire.size() < 10) {
    return Error("amp ch: truncated header");
  }
  ChannelFrame frame;
  frame.header.frame_version = wire[0];
  frame.header.frame_type = static_cast<ChannelFrameType>(wire[1]);
  frame.header.channel_id = static_cast<uint32_t>(wire[2]) | (static_cast<uint32_t>(wire[3]) << 8)
                            | (static_cast<uint32_t>(wire[4]) << 16) | (static_cast<uint32_t>(wire[5]) << 24);
  frame.header.channel_seq = static_cast<uint32_t>(wire[6]) | (static_cast<uint32_t>(wire[7]) << 8)
                             | (static_cast<uint32_t>(wire[8]) << 16) | (static_cast<uint32_t>(wire[9]) << 24);
  auto body = wire.subspan(10);

  if (frame.header.frame_version != kChannelFrameVersion) {
    return Error("amp ch: unsupported frame version");
  }

  switch (frame.header.frame_type) {
  case ChannelFrameType::Open: {
    size_t consumed = 0;
    auto proto = DecodeLenUtf8Le(body, consumed);
    if (!proto) {
      return proto.error();
    }
    body = body.subspan(consumed);
    if (body.empty()) {
      return Error("amp ch: missing channel class");
    }
    frame.open.protocol_id = std::move(*proto);
    frame.open.channel_class = static_cast<ChannelClass>(body[0]);
    body = body.subspan(1);
    auto flags = ReadU16(body);
    if (!flags) {
      return flags.error();
    }
    frame.open.flags = *flags;
    // Optional trailing u32 for forward/backward decode: absent → 0 (peer default).
    if (!body.empty()) {
      auto max_bytes = ReadU32(body);
      if (!max_bytes) {
        return max_bytes.error();
      }
      frame.open.max_message_bytes = *max_bytes;
    }
    if (auto trailing = ExpectEmpty(body); !trailing) {
      return trailing.error();
    }
    break;
  }
  case ChannelFrameType::OpenAck:
    if (body.empty()) {
      return Error("amp ch: missing open ack result");
    }
    frame.open_ack_result = body[0];
    if (auto trailing = ExpectEmpty(body.subspan(1)); !trailing) {
      return trailing.error();
    }
    break;
  case ChannelFrameType::Data:
    frame.payload.assign(body.begin(), body.end());
    break;
  case ChannelFrameType::Close:
    if (!body.empty()) {
      size_t consumed = 0;
      auto reason = DecodeLenUtf8Le(body, consumed);
      if (!reason) {
        return reason.error();
      }
      frame.payload.assign(reason->begin(), reason->end());
      if (auto trailing = ExpectEmpty(body.subspan(consumed)); !trailing) {
        return trailing.error();
      }
    }
    break;
  case ChannelFrameType::Reset: {
    auto code = ReadU32(body);
    if (!code) {
      return code.error();
    }
    frame.reset_code = *code;
    if (auto trailing = ExpectEmpty(body); !trailing) {
      return trailing.error();
    }
    break;
  }
  case ChannelFrameType::Frag: {
    auto msg_id = ReadU64(body);
    if (!msg_id) {
      return msg_id.error();
    }
    auto frag_index = ReadU16(body);
    if (!frag_index) {
      return frag_index.error();
    }
    auto frag_count = ReadU16(body);
    if (!frag_count) {
      return frag_count.error();
    }
    auto total_len = ReadU32(body);
    if (!total_len) {
      return total_len.error();
    }
    frame.frag.msg_id = *msg_id;
    frame.frag.frag_index = *frag_index;
    frame.frag.frag_count = *frag_count;
    frame.frag.total_len = *total_len;
    frame.frag.chunk.assign(body.begin(), body.end());
    break;
  }
  default:
    return Error("amp ch: unknown frame type");
  }
  return frame;
}

} // namespace pp::amp
