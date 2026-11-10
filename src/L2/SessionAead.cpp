#include "amp/L2/SessionAead.h"

#include "amp/L2/Types.h"

#include "crypto/Aead.h"

namespace pp::amp {

static_assert(kSessionKeyBytes == XChaCha20Poly1305::kKeyBytes);
static_assert(kAeadNonceSize == XChaCha20Poly1305::kNonceBytes);

Roe<ByteVector> SessionAead::GenerateNonce() {
  return XChaCha20Poly1305::RandomNonce();
}

Roe<size_t> SessionAead::EncryptInto(const ByteVector& session_key, const std::span<const uint8_t> plaintext,
                                     const std::span<const uint8_t> aad, const std::span<const uint8_t> nonce,
                                     const std::span<uint8_t> ciphertext_out) {
  return XChaCha20Poly1305::SealInto(session_key, nonce, plaintext, aad, ciphertext_out);
}

Roe<ByteVector> SessionAead::Decrypt(const ByteVector& session_key, const std::span<const uint8_t> nonce,
                                     const std::span<const uint8_t> ciphertext, const std::span<const uint8_t> aad) {
  ByteVector plaintext(
      ciphertext.size() >= XChaCha20Poly1305::kTagBytes ? ciphertext.size() - XChaCha20Poly1305::kTagBytes : 0);
  auto length = XChaCha20Poly1305::OpenInto(session_key, nonce, ciphertext, aad, plaintext);
  if (!length) {
    return length.error();
  }
  plaintext.resize(*length);
  return plaintext;
}

Roe<AeadBlob> SessionAead::Encrypt(const ByteVector& session_key, const ByteVector& plaintext, const ByteVector& aad,
                                   const ByteVector& nonce) {
  AeadBlob blob;
  blob.nonce = nonce;
  blob.ciphertext.resize(plaintext.size() + XChaCha20Poly1305::kTagBytes);
  auto len = EncryptInto(session_key, plaintext, aad, nonce, blob.ciphertext);
  if (!len) {
    return len.error();
  }
  blob.ciphertext.resize(*len);
  return blob;
}

Roe<ByteVector> SessionAead::Decrypt(const ByteVector& session_key, const AeadBlob& blob, const ByteVector& aad) {
  return Decrypt(session_key, blob.nonce, blob.ciphertext, aad);
}

} // namespace pp::amp
