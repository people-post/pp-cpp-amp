#pragma once

#include "amp/L1/Types.h"


#include <span>
#include <vector>

namespace pp::adp {

class WireCodec {
public:
  static Roe<std::vector<uint8_t>> Encode(const WirePacket& pkt);
  static Roe<WirePacket> Decode(std::span<const uint8_t> datagram);

  /** Bytes covered by HMAC (everything except the trailing tag). */
  static std::span<const uint8_t> MacInput(std::span<const uint8_t> datagram);
};

} // namespace pp::adp
