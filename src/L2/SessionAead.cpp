#include "amp/L2/SessionAead.h"

#include "amp/L2/Types.h"

#include "crypto/SodiumUtil.h"

#include <sodium.h>

namespace pp::amp {

Roe<ByteVector> SessionAead::GenerateNonce() {
  pp::EnsureSodiumInit();
  ByteVector nonce(kAeadNonceSize);
  randombytes_buf(nonce.data(), nonce.size());
  return nonce;
}

Roe<size_t> SessionAead::EncryptInto(const ByteVector& session_key, const std::span<const uint8_t> plaintext,
                                     const std::span<const uint8_t> aad, const std::span<const uint8_t> nonce,
                                     const std::span<uint8_t> ciphertext_out) {
  if (session_key.size() != kSessionKeyBytes) {
    return Error("Invalid session key size");
  }
  if (nonce.size() != kAeadNonceSize) {
    return Error("Invalid nonce size");
  }
  const size_t need = plaintext.size() + crypto_aead_xchacha20poly1305_ietf_abytes();
  if (ciphertext_out.size() < need) {
    return Error("AEAD ciphertext buffer too small");
  }
  pp::EnsureSodiumInit();

  unsigned long long ciphertext_len = 0;
  if (crypto_aead_xchacha20poly1305_ietf_encrypt(ciphertext_out.data(), &ciphertext_len, plaintext.data(),
                                                 plaintext.size(), aad.data(), aad.size(), nullptr, nonce.data(),
                                                 session_key.data()) != 0) {
    return Error("AEAD encrypt failed");
  }
  return static_cast<size_t>(ciphertext_len);
}

Roe<ByteVector> SessionAead::Decrypt(const ByteVector& session_key, const std::span<const uint8_t> nonce,
                                     const std::span<const uint8_t> ciphertext, const std::span<const uint8_t> aad) {
  if (session_key.size() != kSessionKeyBytes) {
    return Error("Invalid session key size");
  }
  if (nonce.size() != kAeadNonceSize) {
    return Error("Invalid nonce size");
  }
  pp::EnsureSodiumInit();

  ByteVector plaintext(ciphertext.size());
  unsigned long long plaintext_len = 0;
  if (crypto_aead_xchacha20poly1305_ietf_decrypt(plaintext.data(), &plaintext_len, nullptr, ciphertext.data(),
                                                 ciphertext.size(), aad.data(), aad.size(), nonce.data(),
                                                 session_key.data()) != 0) {
    return Error("AEAD decrypt failed");
  }
  plaintext.resize(static_cast<size_t>(plaintext_len));
  return plaintext;
}

Roe<AeadBlob> SessionAead::Encrypt(const ByteVector& session_key, const ByteVector& plaintext,
                                   const ByteVector& aad, const ByteVector& nonce) {
  AeadBlob blob;
  blob.nonce = nonce;
  blob.ciphertext.resize(plaintext.size() + crypto_aead_xchacha20poly1305_ietf_abytes());
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
