#include "amp/link/AmpAdpCarrier.h"

#include "amp/L1/PacketProtection.h"

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

std::array<uint8_t, kCarrierSealedHeaderBytes> AmpAdpCarrier::EncodeSealedHeader(const bool reliable,
                                                                                 const uint32_t number) {
  std::array<uint8_t, kCarrierSealedHeaderBytes> out{};
  out[0] = static_cast<uint8_t>(reliable ? AmpAdpPayloadKind::SealedReliable : AmpAdpPayloadKind::Sealed);
  for (int i = 0; i < 4; ++i) {
    out[1 + i] = static_cast<uint8_t>(number >> (8 * i));
  }
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

Roe<CarrierSealedHeader> AmpAdpCarrier::DecodeSealedHeader(const std::span<const uint8_t> payload) {
  if (payload.size() < kCarrierSealedHeaderBytes + adp::kPacketTagBytes) {
    return Error("amp carrier: truncated sealed packet");
  }
  const auto kind = static_cast<AmpAdpPayloadKind>(payload[0]);
  if (kind != AmpAdpPayloadKind::Sealed && kind != AmpAdpPayloadKind::SealedReliable) {
    return Error("amp carrier: not a sealed packet");
  }
  size_t offset = 1;
  auto number = ReadU32(payload, offset);
  if (!number) {
    return number.error();
  }
  CarrierSealedHeader header;
  header.reliable = kind == AmpAdpPayloadKind::SealedReliable;
  header.number = *number;
  return header;
}

std::vector<uint8_t> AmpAdpCarrier::EncodeLaneData(const uint32_t seq, const std::span<const uint8_t> inner) {
  std::vector<uint8_t> out;
  out.reserve(1 + 4 + inner.size());
  out.push_back(static_cast<uint8_t>(AmpAdpPayloadKind::LaneData));
  AppendU32(out, seq);
  out.insert(out.end(), inner.begin(), inner.end());
  return out;
}

Roe<std::pair<uint32_t, std::span<const uint8_t>>> AmpAdpCarrier::DecodeLaneData(
    const std::span<const uint8_t> payload) {
  if (payload.empty() || static_cast<AmpAdpPayloadKind>(payload[0]) != AmpAdpPayloadKind::LaneData) {
    return Error("amp carrier: not a lane data payload");
  }
  size_t offset = 1;
  auto seq = ReadU32(payload, offset);
  if (!seq) {
    return seq.error();
  }
  if (*seq == 0 || offset >= payload.size()) {
    return Error("amp carrier: bad lane data");
  }
  return std::pair<uint32_t, std::span<const uint8_t>>{*seq, payload.subspan(offset)};
}

std::vector<uint8_t> AmpAdpCarrier::EncodeLaneAck(const LaneAckFields& ack) {
  std::vector<uint8_t> out;
  out.reserve(1 + 4 + 8);
  out.push_back(static_cast<uint8_t>(AmpAdpPayloadKind::LaneAck));
  AppendU32(out, ack.cumulative);
  AppendU32(out, static_cast<uint32_t>(ack.selective));
  AppendU32(out, static_cast<uint32_t>(ack.selective >> 32));
  return out;
}

Roe<LaneAckFields> AmpAdpCarrier::DecodeLaneAck(const std::span<const uint8_t> payload) {
  if (payload.empty() || static_cast<AmpAdpPayloadKind>(payload[0]) != AmpAdpPayloadKind::LaneAck) {
    return Error("amp carrier: not a lane ack payload");
  }
  size_t offset = 1;
  auto cumulative = ReadU32(payload, offset);
  auto lo = ReadU32(payload, offset);
  auto hi = ReadU32(payload, offset);
  if (!cumulative || !lo || !hi) {
    return Error("amp carrier: truncated lane ack");
  }
  return LaneAckFields{*cumulative, static_cast<uint64_t>(*lo) | (static_cast<uint64_t>(*hi) << 32)};
}

} // namespace pp::amp
