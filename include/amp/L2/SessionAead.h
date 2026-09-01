#pragma once

#include "amp/L2/Types.h"

namespace pp::amp {

struct AeadBlob {
  ByteVector nonce;
  ByteVector ciphertext;
};

/** XChaCha20-Poly1305 session AEAD for AMP L2 seal/open. */
class SessionAead {
public:
  static Roe<AeadBlob> Encrypt(const ByteVector& session_key, const ByteVector& plaintext,
                               const ByteVector& aad, const ByteVector& nonce);
  static Roe<ByteVector> Decrypt(const ByteVector& session_key, const AeadBlob& blob, const ByteVector& aad);
  static Roe<ByteVector> GenerateNonce();
};

} // namespace pp::amp
