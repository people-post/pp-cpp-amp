#include "amp/L1/WireCodec.h"

#include "amp/L1/PacketProtection.h"

#include <cstring>

namespace pp::adp {
namespace {

void WriteU32Le(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(static_cast<uint8_t>(v & 0xff));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
  out.push_back(static_cast<uint8_t>((v >> 16) & 0xff));
  out.push_back(static_cast<uint8_t>((v >> 24) & 0xff));
}

uint32_t ReadU32Le(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint8_t VersionAndType(const PacketType type) {
  return static_cast<uint8_t>((kWireVersion << 4) | static_cast<uint8_t>(type));
}

} // namespace

Roe<std::vector<uint8_t>> WireCodec::Encode(const WirePacket& pkt) {
  if (pkt.version != kWireVersion) {
    return Error("adp: bad version");
  }
  if (IsSealed(pkt.type)) {
    return Error("adp: a sealed packet has no plain encoding");
  }
  if (pkt.payload.size() > kMaxPayload) {
    return Error("adp: payload too large");
  }
  std::vector<uint8_t> out;
  out.reserve(kHeaderBytes + pkt.payload.size() + kHmacBytes);
  out.push_back(VersionAndType(pkt.type));
  out.insert(out.end(), pkt.assoc.bytes.begin(), pkt.assoc.bytes.end());
  WriteU32Le(out, pkt.seq);
  WriteU32Le(out, pkt.timestamp_ms);
  out.insert(out.end(), pkt.payload.begin(), pkt.payload.end());
  return out;
}

Roe<PacketType> WireCodec::PeekType(std::span<const uint8_t> datagram) {
  if (datagram.empty()) {
    return Error("adp: empty datagram");
  }
  if ((datagram[0] >> 4) != kWireVersion) {
    return Error("adp: unknown version");
  }
  const uint8_t type = datagram[0] & 0x0f;
  if (type > kMaxPacketType) {
    return Error("adp: bad packet type");
  }
  return static_cast<PacketType>(type);
}

AssocId WireCodec::PeekAssoc(std::span<const uint8_t> datagram) {
  AssocId id{};
  if (datagram.size() >= 1 + kAssocIdBytes) {
    std::memcpy(id.bytes.data(), datagram.data() + 1, kAssocIdBytes);
  }
  return id;
}

Roe<WirePacket> WireCodec::Decode(std::span<const uint8_t> datagram) {
  if (datagram.size() < kHeaderBytes + kHmacBytes) {
    return Error("adp: truncated datagram");
  }
  auto type = PeekType(datagram);
  if (!type) {
    return type.error();
  }
  if (IsSealed(*type)) {
    return Error("adp: a sealed packet has no plain decoding");
  }
  if (datagram.size() > kMaxDatagramBytes) {
    return Error("adp: datagram too large");
  }
  WirePacket pkt;
  pkt.version = kWireVersion;
  pkt.type = *type;
  pkt.assoc = PeekAssoc(datagram);
  pkt.seq = ReadU32Le(datagram.data() + 1 + kAssocIdBytes);
  pkt.timestamp_ms = ReadU32Le(datagram.data() + 1 + kAssocIdBytes + 4);
  pkt.payload.assign(datagram.begin() + kHeaderBytes, datagram.end() - kHmacBytes);
  return pkt;
}

std::array<uint8_t, kSealedHeaderBytes> WireCodec::EncodeSealedHeader(const SealedHeader& header) {
  std::array<uint8_t, kSealedHeaderBytes> out{};
  out[0] = VersionAndType(header.type);
  std::memcpy(out.data() + 1, header.assoc.bytes.data(), kAssocIdBytes);
  for (int i = 0; i < 4; ++i) {
    out[1 + kAssocIdBytes + i] = static_cast<uint8_t>(header.number >> (8 * i));
  }
  return out;
}

Roe<SealedHeader> WireCodec::DecodeSealedHeader(std::span<const uint8_t> datagram) {
  if (datagram.size() < kSealedHeaderBytes + kPacketTagBytes) {
    return Error("adp: truncated sealed datagram");
  }
  if (datagram.size() > kMaxDatagramBytes) {
    return Error("adp: datagram too large");
  }
  auto type = PeekType(datagram);
  if (!type) {
    return type.error();
  }
  if (!IsSealed(*type)) {
    return Error("adp: not a sealed packet");
  }
  SealedHeader header;
  header.type = *type;
  header.assoc = PeekAssoc(datagram);
  header.number = ReadU32Le(datagram.data() + 1 + kAssocIdBytes);
  return header;
}

std::span<const uint8_t> WireCodec::MacInput(std::span<const uint8_t> datagram) {
  if (datagram.size() < kHmacBytes) {
    return {};
  }
  return datagram.first(datagram.size() - kHmacBytes);
}

} // namespace pp::adp
