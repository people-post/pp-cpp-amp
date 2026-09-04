#pragma once

#include "amp/L2/Types.h"

#include <span>

namespace pp::amp {

struct AeadBlob {
  ByteVector nonce;
  ByteVector ciphertext;
};

/** XChaCha20-Poly1305 session AEAD for AMP L2 seal/open. */
class SessionAead {
public:
  /** Encrypt `plaintext` into `ciphertext_out` (must be ≥ plaintext.size() + tag). */
  static Roe<size_t> EncryptInto(const ByteVector& session_key, std::span<const uint8_t> plaintext,
                                 std::span<const uint8_t> aad, std::span<const uint8_t> nonce,
                                 std::span<uint8_t> ciphertext_out);

  static Roe<ByteVector> Decrypt(const ByteVector& session_key, std::span<const uint8_t> nonce,
                                 std::span<const uint8_t> ciphertext, std::span<const uint8_t> aad);

  static Roe<AeadBlob> Encrypt(const ByteVector& session_key, const ByteVector& plaintext,
                               const ByteVector& aad, const ByteVector& nonce);
  static Roe<ByteVector> Decrypt(const ByteVector& session_key, const AeadBlob& blob, const ByteVector& aad);
  static Roe<ByteVector> GenerateNonce();
};

} // namespace pp::amp
