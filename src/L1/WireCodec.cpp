#include "amp/L1/WireCodec.h"

#include <cstring>

namespace pp::adp {
namespace {

void WriteU16Le(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back(static_cast<uint8_t>(v & 0xff));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
}

void WriteU32Le(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(static_cast<uint8_t>(v & 0xff));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
  out.push_back(static_cast<uint8_t>((v >> 16) & 0xff));
  out.push_back(static_cast<uint8_t>((v >> 24) & 0xff));
}

uint16_t ReadU16Le(const uint8_t* p) {
  return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t ReadU32Le(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

} // namespace

Roe<std::vector<uint8_t>> WireCodec::Encode(const WirePacket& pkt) {
  if (pkt.version != kWireVersion) {
    return Error("adp: bad version");
  }
  if (pkt.payload.size() > kMaxPayload) {
    return Error("adp: payload too large");
  }
  std::vector<uint8_t> out;
  out.reserve(kHeaderBytes + pkt.payload.size());
  out.push_back(pkt.version);
  out.push_back(static_cast<uint8_t>(pkt.type));
  out.insert(out.end(), pkt.assoc.bytes.begin(), pkt.assoc.bytes.end());
  WriteU32Le(out, pkt.seq);
  WriteU32Le(out, pkt.timestamp_ms);
  WriteU16Le(out, static_cast<uint16_t>(pkt.payload.size()));
  out.insert(out.end(), pkt.payload.begin(), pkt.payload.end());
  return out;
}

Roe<WirePacket> WireCodec::Decode(std::span<const uint8_t> datagram) {
  if (datagram.size() < kHeaderBytes + kHmacBytes) {
    return Error("adp: truncated datagram");
  }
  const size_t body = datagram.size() - kHmacBytes;
  if (body < kHeaderBytes) {
    return Error("adp: truncated header");
  }
  WirePacket pkt;
  pkt.version = datagram[0];
  if (pkt.version != kWireVersion) {
    return Error("adp: unknown version");
  }
  const uint8_t type = datagram[1];
  if (type > static_cast<uint8_t>(PacketType::Keepalive)) {
    return Error("adp: bad packet type");
  }
  pkt.type = static_cast<PacketType>(type);
  std::memcpy(pkt.assoc.bytes.data(), datagram.data() + 2, kAssocIdBytes);
  pkt.seq = ReadU32Le(datagram.data() + 18);
  pkt.timestamp_ms = ReadU32Le(datagram.data() + 22);
  const uint16_t plen = ReadU16Le(datagram.data() + 26);
  if (plen > kMaxPayload) {
    return Error("adp: payload_len too large");
  }
  if (body != kHeaderBytes + plen) {
    return Error("adp: length mismatch");
  }
  pkt.payload.assign(datagram.begin() + kHeaderBytes, datagram.begin() + body);
  return pkt;
}

std::span<const uint8_t> WireCodec::MacInput(std::span<const uint8_t> datagram) {
  if (datagram.size() < kHmacBytes) {
    return {};
  }
  return datagram.first(datagram.size() - kHmacBytes);
}

} // namespace pp::adp
