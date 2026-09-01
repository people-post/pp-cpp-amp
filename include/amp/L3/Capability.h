#pragma once

#include "amp/L3/ChannelWire.h"


#include <string>
#include <vector>

namespace pp::amp {

/** Channel 0 capability payload (binary v1). */
struct CapabilityPayload {
  std::string local_peer_id;
  std::vector<std::string> listen_multiaddrs;
  std::vector<std::string> protocols;
};

class CapabilityCodec {
public:
  static Roe<std::vector<uint8_t>> Encode(const CapabilityPayload& payload);
  static Roe<CapabilityPayload> Decode(std::span<const uint8_t> wire);
};

} // namespace pp::amp
