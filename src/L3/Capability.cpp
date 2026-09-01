#include "amp/L3/Capability.h"

namespace pp::amp {

namespace {

Roe<void> AppendStringList(std::vector<uint8_t>& out, const std::vector<std::string>& items) {
  if (items.size() > 0xFFFF) {
    return Error("amp cap: too many strings");
  }
  out.push_back(static_cast<uint8_t>(items.size()));
  out.push_back(static_cast<uint8_t>(items.size() >> 8));
  for (const auto& item : items) {
    auto encoded = ChannelWire::EncodeLenUtf8Le(item);
    if (!encoded) {
      return encoded.error();
    }
    out.insert(out.end(), encoded->begin(), encoded->end());
  }
  return Roe<void>();
}

Roe<std::vector<std::string>> ReadStringList(std::span<const uint8_t>& wire) {
  if (wire.size() < 2) {
    return Error("amp cap: truncated string list count");
  }
  const uint16_t count = static_cast<uint16_t>(wire[0]) | (static_cast<uint16_t>(wire[1]) << 8);
  wire = wire.subspan(2);
  std::vector<std::string> out;
  out.reserve(count);
  for (uint16_t i = 0; i < count; ++i) {
    size_t consumed = 0;
    auto s = ChannelWire::DecodeLenUtf8Le(wire, consumed);
    if (!s) {
      return s.error();
    }
    wire = wire.subspan(consumed);
    out.push_back(std::move(*s));
  }
  return out;
}

} // namespace

Roe<std::vector<uint8_t>> CapabilityCodec::Encode(const CapabilityPayload& payload) {
  std::vector<uint8_t> out;
  out.push_back(1); // cap version
  auto peer = ChannelWire::EncodeLenUtf8Le(payload.local_peer_id);
  if (!peer) {
    return peer.error();
  }
  out.insert(out.end(), peer->begin(), peer->end());
  auto addrs_ok = AppendStringList(out, payload.listen_multiaddrs);
  if (!addrs_ok) {
    return addrs_ok.error();
  }
  auto protos_ok = AppendStringList(out, payload.protocols);
  if (!protos_ok) {
    return protos_ok.error();
  }
  return out;
}

Roe<CapabilityPayload> CapabilityCodec::Decode(std::span<const uint8_t> wire) {
  if (wire.empty() || wire[0] != 1) {
    return Error("amp cap: bad version");
  }
  auto span = wire.subspan(1);
  size_t consumed = 0;
  auto peer = ChannelWire::DecodeLenUtf8Le(span, consumed);
  if (!peer) {
    return peer.error();
  }
  span = span.subspan(consumed);
  auto addrs = ReadStringList(span);
  if (!addrs) {
    return addrs.error();
  }
  auto protocols = ReadStringList(span);
  if (!protocols) {
    return protocols.error();
  }
  if (!span.empty()) {
    return Error("amp cap: trailing bytes");
  }
  CapabilityPayload out;
  out.local_peer_id = std::move(*peer);
  out.listen_multiaddrs = std::move(*addrs);
  out.protocols = std::move(*protocols);
  return out;
}

} // namespace pp::amp
