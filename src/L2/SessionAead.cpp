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

Roe<AeadBlob> SessionAead::Encrypt(const ByteVector& session_key, const ByteVector& plaintext,
                                   const ByteVector& aad, const ByteVector& nonce) {
  if (session_key.size() != kSessionKeyBytes) {
    return Error("Invalid session key size");
  }
  if (nonce.size() != kAeadNonceSize) {
    return Error("Invalid nonce size");
  }
  pp::EnsureSodiumInit();

  AeadBlob blob;
  blob.nonce = nonce;
  blob.ciphertext.resize(plaintext.size() + crypto_aead_xchacha20poly1305_ietf_abytes());
  unsigned long long ciphertext_len = 0;
  if (crypto_aead_xchacha20poly1305_ietf_encrypt(blob.ciphertext.data(), &ciphertext_len, plaintext.data(),
                                                 plaintext.size(), aad.data(), aad.size(), nullptr, nonce.data(),
                                                 session_key.data()) != 0) {
    return Error("AEAD encrypt failed");
  }
  blob.ciphertext.resize(static_cast<size_t>(ciphertext_len));
  return blob;
}

Roe<ByteVector> SessionAead::Decrypt(const ByteVector& session_key, const AeadBlob& blob, const ByteVector& aad) {
  if (session_key.size() != kSessionKeyBytes) {
    return Error("Invalid session key size");
  }
  if (blob.nonce.size() != kAeadNonceSize) {
    return Error("Invalid nonce size");
  }
  pp::EnsureSodiumInit();

  ByteVector plaintext(blob.ciphertext.size());
  unsigned long long plaintext_len = 0;
  if (crypto_aead_xchacha20poly1305_ietf_decrypt(plaintext.data(), &plaintext_len, nullptr, blob.ciphertext.data(),
                                                 blob.ciphertext.size(), aad.data(), aad.size(), blob.nonce.data(),
                                                 session_key.data()) != 0) {
    return Error("AEAD decrypt failed");
  }
  plaintext.resize(static_cast<size_t>(plaintext_len));
  return plaintext;
}

} // namespace pp::amp
