#include "amp/L2/SessionControl.h"

namespace pp::amp {

namespace {

void AppendU32Le(std::vector<uint8_t>& out, const uint32_t value) {
  out.push_back(static_cast<uint8_t>(value));
  out.push_back(static_cast<uint8_t>(value >> 8));
  out.push_back(static_cast<uint8_t>(value >> 16));
  out.push_back(static_cast<uint8_t>(value >> 24));
}

Roe<uint32_t> ReadU32Le(std::span<const uint8_t> wire, size_t& consumed) {
  if (wire.size() < 4) {
    return Error("amp session control: truncated u32");
  }
  consumed = 4;
  return static_cast<uint32_t>(wire[0]) | (static_cast<uint32_t>(wire[1]) << 8)
         | (static_cast<uint32_t>(wire[2]) << 16) | (static_cast<uint32_t>(wire[3]) << 24);
}

} // namespace

bool SessionControlCodec::LooksLike(const std::span<const uint8_t> wire) {
  return wire.size() >= 6 && wire[0] == kSessionControlWireVersion
         && (wire[1] == static_cast<uint8_t>(SessionControlKind::RekeyRequest)
             || wire[1] == static_cast<uint8_t>(SessionControlKind::RekeyAck));
}

Roe<std::vector<uint8_t>> SessionControlCodec::Encode(const SessionRekeyMessage& message) {
  std::vector<uint8_t> out;
  out.push_back(kSessionControlWireVersion);
  out.push_back(static_cast<uint8_t>(message.kind));
  AppendU32Le(out, message.target_epoch);
  return out;
}

Roe<SessionRekeyMessage> SessionControlCodec::Decode(const std::span<const uint8_t> wire) {
  if (wire.size() != 6) {
    return Error("amp session control: bad length");
  }
  if (wire[0] != kSessionControlWireVersion) {
    return Error("amp session control: bad version");
  }
  SessionRekeyMessage out;
  if (wire[1] == static_cast<uint8_t>(SessionControlKind::RekeyRequest)) {
    out.kind = SessionControlKind::RekeyRequest;
  } else if (wire[1] == static_cast<uint8_t>(SessionControlKind::RekeyAck)) {
    out.kind = SessionControlKind::RekeyAck;
  } else {
    return Error("amp session control: bad kind");
  }
  size_t consumed = 0;
  auto epoch = ReadU32Le(wire.subspan(2), consumed);
  if (!epoch) {
    return epoch.error();
  }
  out.target_epoch = *epoch;
  return out;
}

} // namespace pp::amp
