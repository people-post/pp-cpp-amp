#include "amp/L2/SessionCrypto.h"

#include "amp/L2/SessionAead.h"

namespace pp::amp {

ByteVector SessionCrypto::BuildAad(const uint32_t session_epoch, const uint32_t channel_id, const uint32_t channel_seq,
                                 const Direction direction) {
  ByteVector aad(13);
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
  return aad;
}

Roe<std::vector<uint8_t>> SessionCrypto::Seal(const ByteVector& key, const uint32_t session_epoch,
                                              const uint32_t channel_id, const uint32_t channel_seq,
                                              const Direction direction, std::span<const uint8_t> plaintext) {
  auto aad = BuildAad(session_epoch, channel_id, channel_seq, direction);
  auto nonce = SessionAead::GenerateNonce();
  if (!nonce) {
    return nonce.error();
  }
  ByteVector plain(plaintext.begin(), plaintext.end());
  auto blob = SessionAead::Encrypt(key, plain, aad, *nonce);
  if (!blob) {
    return blob.error();
  }
  std::vector<uint8_t> out;
  out.reserve(blob->nonce.size() + blob->ciphertext.size());
  out.insert(out.end(), blob->nonce.begin(), blob->nonce.end());
  out.insert(out.end(), blob->ciphertext.begin(), blob->ciphertext.end());
  return out;
}

Roe<std::vector<uint8_t>> SessionCrypto::Open(const ByteVector& key, const uint32_t session_epoch,
                                              const uint32_t channel_id, const uint32_t channel_seq,
                                              const Direction direction, std::span<const uint8_t> sealed) {
  if (sealed.size() < kAeadNonceSize) {
    return Error("amp: sealed payload too short");
  }
  AeadBlob blob;
  blob.nonce.assign(sealed.begin(), sealed.begin() + kAeadNonceSize);
  blob.ciphertext.assign(sealed.begin() + kAeadNonceSize, sealed.end());
  auto aad = BuildAad(session_epoch, channel_id, channel_seq, direction);
  auto plain = SessionAead::Decrypt(key, blob, aad);
  if (!plain) {
    return plain.error();
  }
  return std::vector<uint8_t>(plain->begin(), plain->end());
}

} // namespace pp::amp
