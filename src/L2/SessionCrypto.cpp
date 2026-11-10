#include "amp/L2/SessionCrypto.h"

#include "amp/L2/SessionAead.h"

#include <array>
#include <cstring>

namespace pp::amp {

std::array<uint8_t, kAeadNonceSize> SessionCrypto::PacketNonce(const Direction direction, const adp::PacketSpace space,
                                                               const uint32_t session_epoch, const uint32_t number) {
  std::array<uint8_t, kAeadNonceSize> nonce{};
  nonce[0] = static_cast<uint8_t>(direction);
  nonce[1] = static_cast<uint8_t>(space);
  for (int i = 0; i < 4; ++i) {
    nonce[2 + i] = static_cast<uint8_t>(session_epoch >> (8 * i));
    nonce[6 + i] = static_cast<uint8_t>(number >> (8 * i));
  }
  return nonce;
}

Roe<void> SessionCrypto::SealPacket(const ByteVector& key, const Direction direction, const uint32_t session_epoch,
                                    const adp::PacketSpace space, const uint32_t number,
                                    const std::span<const uint8_t> header, const std::span<const uint8_t> plaintext,
                                    std::vector<uint8_t>& out) {
  const auto nonce = PacketNonce(direction, space, session_epoch, number);
  const size_t at = out.size();
  out.resize(at + plaintext.size() + adp::kPacketTagBytes);
  auto len =
      SessionAead::EncryptInto(key, plaintext, header, nonce, std::span<uint8_t>(out.data() + at, out.size() - at));
  if (!len) {
    out.resize(at);
    return len.error();
  }
  out.resize(at + *len);
  return Roe<void>();
}

Roe<std::vector<uint8_t>> SessionCrypto::OpenPacket(const ByteVector& key, const Direction direction,
                                                    const uint32_t session_epoch, const adp::PacketSpace space,
                                                    const uint32_t number, const std::span<const uint8_t> header,
                                                    const std::span<const uint8_t> sealed) {
  if (sealed.size() < adp::kPacketTagBytes) {
    return Error("amp: sealed packet too short");
  }
  const auto nonce = PacketNonce(direction, space, session_epoch, number);
  return SessionAead::Decrypt(key, nonce, sealed, header);
}

} // namespace pp::amp
