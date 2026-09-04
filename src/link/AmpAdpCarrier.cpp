#include "amp/link/AmpAdpCarrier.h"

namespace pp::amp {

namespace {

void AppendU32(std::vector<uint8_t>& out, const uint32_t v) {
  out.push_back(static_cast<uint8_t>(v));
  out.push_back(static_cast<uint8_t>(v >> 8));
  out.push_back(static_cast<uint8_t>(v >> 16));
  out.push_back(static_cast<uint8_t>(v >> 24));
}

Roe<uint32_t> ReadU32(std::span<const uint8_t> wire, size_t& offset) {
  if (offset + 4 > wire.size()) {
    return Error("amp carrier: truncated u32");
  }
  const uint32_t v = static_cast<uint32_t>(wire[offset]) | (static_cast<uint32_t>(wire[offset + 1]) << 8)
                     | (static_cast<uint32_t>(wire[offset + 2]) << 16)
                     | (static_cast<uint32_t>(wire[offset + 3]) << 24);
  offset += 4;
  return v;
}

} // namespace

Roe<std::vector<uint8_t>> AmpAdpCarrier::EncodeMsh(const MshMessageType type, const std::span<const uint8_t> body) {
  std::vector<uint8_t> out;
  out.push_back(static_cast<uint8_t>(AmpAdpPayloadKind::Msh));
  out.push_back(static_cast<uint8_t>(type));
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

Roe<std::vector<std::vector<uint8_t>>> AmpAdpCarrier::EncodeMshChunked(const MshMessageType type,
                                                                         const std::span<const uint8_t> body) {
  if (body.size() <= kMaxMshBodyPerDatagram) {
    auto single = EncodeMsh(type, body);
    if (!single) {
      return single.error();
    }
    return std::vector<std::vector<uint8_t>>{std::move(*single)};
  }
  const uint16_t frag_count =
      static_cast<uint16_t>((body.size() + kMaxMshBodyPerDatagram - 1) / kMaxMshBodyPerDatagram);
  std::vector<std::vector<uint8_t>> out;
  out.reserve(frag_count);
  for (uint16_t i = 0; i < frag_count; ++i) {
    const size_t offset = static_cast<size_t>(i) * kMaxMshBodyPerDatagram;
    const size_t chunk_len = std::min(kMaxMshBodyPerDatagram, body.size() - offset);
    std::vector<uint8_t> wire;
    wire.push_back(static_cast<uint8_t>(AmpAdpPayloadKind::MshChunk));
    wire.push_back(static_cast<uint8_t>(type));
    wire.push_back(static_cast<uint8_t>(i));
    wire.push_back(static_cast<uint8_t>(i >> 8));
    wire.push_back(static_cast<uint8_t>(frag_count));
    wire.push_back(static_cast<uint8_t>(frag_count >> 8));
    wire.insert(wire.end(), body.begin() + static_cast<std::ptrdiff_t>(offset),
                body.begin() + static_cast<std::ptrdiff_t>(offset + chunk_len));
    out.push_back(std::move(wire));
  }
  return out;
}

Roe<std::vector<uint8_t>> AmpAdpCarrier::EncodeSealed(const uint32_t channel_id, const uint32_t channel_seq,
                                                      const std::span<const uint8_t> sealed) {
  std::vector<uint8_t> out;
  out.reserve(1 + 8 + sealed.size());
  out.push_back(static_cast<uint8_t>(AmpAdpPayloadKind::Sealed));
  AppendU32(out, channel_id);
  AppendU32(out, channel_seq);
  out.insert(out.end(), sealed.begin(), sealed.end());
  return out;
}

Roe<AmpAdpPayloadKind> AmpAdpCarrier::DecodeKind(const std::span<const uint8_t> payload) {
  if (payload.empty()) {
    return Error("amp carrier: empty payload");
  }
  return static_cast<AmpAdpPayloadKind>(payload[0]);
}

Roe<MshMessageType> AmpAdpCarrier::DecodeMshType(const std::span<const uint8_t> payload) {
  if (payload.size() < 2) {
    return Error("amp carrier: truncated msh header");
  }
  if (static_cast<AmpAdpPayloadKind>(payload[0]) != AmpAdpPayloadKind::Msh) {
    return Error("amp carrier: not an msh payload");
  }
  return static_cast<MshMessageType>(payload[1]);
}

Roe<std::vector<uint8_t>> AmpAdpCarrier::DecodeMshBody(const std::span<const uint8_t> payload) {
  if (payload.size() < 2) {
    return Error("amp carrier: truncated msh payload");
  }
  return std::vector<uint8_t>(payload.begin() + 2, payload.end());
}

Roe<std::tuple<MshMessageType, uint16_t, uint16_t, std::span<const uint8_t>>> AmpAdpCarrier::DecodeMshChunk(
    const std::span<const uint8_t> payload) {
  if (payload.size() < 6 || static_cast<AmpAdpPayloadKind>(payload[0]) != AmpAdpPayloadKind::MshChunk) {
    return Error("amp carrier: bad msh chunk");
  }
  const auto type = static_cast<MshMessageType>(payload[1]);
  const uint16_t index = static_cast<uint16_t>(payload[2]) | (static_cast<uint16_t>(payload[3]) << 8);
  const uint16_t count = static_cast<uint16_t>(payload[4]) | (static_cast<uint16_t>(payload[5]) << 8);
  return std::make_tuple(type, index, count, payload.subspan(6));
}

Roe<std::pair<uint32_t, uint32_t>> AmpAdpCarrier::DecodeSealedHeader(const std::span<const uint8_t> payload) {
  if (payload.size() < 9 || static_cast<AmpAdpPayloadKind>(payload[0]) != AmpAdpPayloadKind::Sealed) {
    return Error("amp carrier: bad sealed header");
  }
  size_t offset = 1;
  auto channel_id = ReadU32(payload, offset);
  if (!channel_id) {
    return channel_id.error();
  }
  auto channel_seq = ReadU32(payload, offset);
  if (!channel_seq) {
    return channel_seq.error();
  }
  return std::make_pair(*channel_id, *channel_seq);
}

Roe<std::vector<uint8_t>> AmpAdpCarrier::DecodeSealedBody(const std::span<const uint8_t> payload) {
  if (payload.size() < 9) {
    return Error("amp carrier: truncated sealed payload");
  }
  return std::vector<uint8_t>(payload.begin() + 9, payload.end());
}

} // namespace pp::amp
