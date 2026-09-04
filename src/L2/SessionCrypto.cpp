#include "amp/L2/SessionCrypto.h"

#include "amp/L2/SessionAead.h"

#include "crypto/SodiumUtil.h"

#include <array>
#include <cstring>

#include <sodium.h>

namespace pp::amp {
namespace {

void FillAad(std::array<uint8_t, 13>& aad, const uint32_t session_epoch, const uint32_t channel_id,
             const uint32_t channel_seq, const Direction direction) {
  aad[0] = static_cast<uint8_t>(session_epoch);
  aad[1] = static_cast<uint8_t>(session_epoch >> 8);
  aad[2] = static_cast<uint8_t>(session_epoch >> 16);
  aad[3] = static_cast<uint8_t>(session_epoch >> 24);
  aad[4] = static_cast<uint8_t>(channel_id);
  aad[5] = static_cast<uint8_t>(channel_id >> 8);
  aad[6] = static_cast<uint8_t>(channel_id >> 16);
  aad[7] = static_cast<uint8_t>(channel_id >> 24);
  aad[8] = static_cast<uint8_t>(channel_seq);
  aad[9] = static_cast<uint8_t>(channel_seq >> 8);
  aad[10] = static_cast<uint8_t>(channel_seq >> 16);
  aad[11] = static_cast<uint8_t>(channel_seq >> 24);
  aad[12] = static_cast<uint8_t>(direction);
}

} // namespace

ByteVector SessionCrypto::BuildAad(const uint32_t session_epoch, const uint32_t channel_id, const uint32_t channel_seq,
                                   const Direction direction) {
  std::array<uint8_t, 13> aad{};
  FillAad(aad, session_epoch, channel_id, channel_seq, direction);
  return ByteVector(aad.begin(), aad.end());
}

Roe<std::vector<uint8_t>> SessionCrypto::Seal(const ByteVector& key, const uint32_t session_epoch,
                                              const uint32_t channel_id, const uint32_t channel_seq,
                                              const Direction direction, const std::span<const uint8_t> plaintext) {
  std::array<uint8_t, 13> aad{};
  FillAad(aad, session_epoch, channel_id, channel_seq, direction);

  pp::EnsureSodiumInit();
  std::array<uint8_t, kAeadNonceSize> nonce{};
  randombytes_buf(nonce.data(), nonce.size());

  std::vector<uint8_t> out(kAeadNonceSize + plaintext.size() + crypto_aead_xchacha20poly1305_ietf_abytes());
  std::memcpy(out.data(), nonce.data(), kAeadNonceSize);
  auto ct_len = SessionAead::EncryptInto(key, plaintext, aad, nonce,
                                         std::span<uint8_t>(out.data() + kAeadNonceSize, out.size() - kAeadNonceSize));
  if (!ct_len) {
    return ct_len.error();
  }
  out.resize(kAeadNonceSize + *ct_len);
  return out;
}

Roe<std::vector<uint8_t>> SessionCrypto::Open(const ByteVector& key, const uint32_t session_epoch,
                                              const uint32_t channel_id, const uint32_t channel_seq,
                                              const Direction direction, const std::span<const uint8_t> sealed) {
  if (sealed.size() < kAeadNonceSize) {
    return Error("amp: sealed payload too short");
  }
  std::array<uint8_t, 13> aad{};
  FillAad(aad, session_epoch, channel_id, channel_seq, direction);
  return SessionAead::Decrypt(key, sealed.first(kAeadNonceSize), sealed.subspan(kAeadNonceSize), aad);
}

} // namespace pp::amp
