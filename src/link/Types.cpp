#include "amp/link/Types.h"

#include <sodium.h>

namespace pp::amp {

adp::PeerKey PreSessionPeerKey() {
  adp::PeerKey key{};
  static const char kLabel[] = "pp-amp-msh-presession-v1";
  crypto_generichash(key.bytes.data(), key.bytes.size(), reinterpret_cast<const unsigned char*>(kLabel),
                     sizeof(kLabel) - 1, nullptr, 0);
  return key;
}

adp::AssocId PreSessionAssocId() {
  adp::AssocId id{};
  id.bytes[0] = 0x01;
  return id;
}

std::string IdentityPublicKeyFingerprint(const ByteVector& identity_public_key) {
  if (identity_public_key.empty()) {
    return {};
  }
  unsigned char digest[crypto_generichash_BYTES_MIN];
  crypto_generichash(digest, sizeof(digest), identity_public_key.data(), identity_public_key.size(), nullptr, 0);
  static const char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(2 + sizeof(digest) * 2);
  out.append("id:");
  for (const unsigned char byte : digest) {
    out.push_back(kHex[byte >> 4]);
    out.push_back(kHex[byte & 0x0f]);
  }
  return out;
}

} // namespace pp::amp
