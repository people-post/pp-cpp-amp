#pragma once

#include "amp/L1/PacketProtection.h"
#include "amp/L2/Types.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace pp::amp {

class SessionCrypto {
public:
  /**
   * A sealed packet's nonce (A029 H1): direction, packet space, key epoch and packet number, then zeros. Never sent:
   * both ends derive it, and it is unique per key as long as a (space, number) seals one plaintext.
   */
  static std::array<uint8_t, kAeadNonceSize> PacketNonce(Direction direction, adp::PacketSpace space,
                                                         uint32_t session_epoch, uint32_t number);

  static Roe<void> SealPacket(const ByteVector& key, Direction direction, uint32_t session_epoch,
                              adp::PacketSpace space, uint32_t number, std::span<const uint8_t> header,
                              std::span<const uint8_t> plaintext, std::vector<uint8_t>& out);

  static Roe<std::vector<uint8_t>> OpenPacket(const ByteVector& key, Direction direction, uint32_t session_epoch,
                                              adp::PacketSpace space, uint32_t number, std::span<const uint8_t> header,
                                              std::span<const uint8_t> sealed);
};

} // namespace pp::amp
