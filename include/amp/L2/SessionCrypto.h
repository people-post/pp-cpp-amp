#pragma once

#include "amp/L2/Types.h"


#include <cstdint>
#include <span>
#include <vector>

namespace pp::amp {

class SessionCrypto {
public:
  static ByteVector BuildAad(uint32_t session_epoch, uint32_t channel_id, uint32_t channel_seq, Direction direction);

  static Roe<std::vector<uint8_t>> Seal(const ByteVector& key, uint32_t session_epoch, uint32_t channel_id,
                                        uint32_t channel_seq, Direction direction,
                                        std::span<const uint8_t> plaintext);

  static Roe<std::vector<uint8_t>> Open(const ByteVector& key, uint32_t session_epoch, uint32_t channel_id,
                                       uint32_t channel_seq, Direction direction, std::span<const uint8_t> sealed);
};

} // namespace pp::amp
