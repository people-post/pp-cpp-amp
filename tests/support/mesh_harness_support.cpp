#include "support/mesh_harness_support.h"

#include "crypto/MlDsa.h"

#include <sodium.h>

#include <string>

namespace pbr::test {

namespace {

pp::Roe<std::string> DeriveTestPeerIdStub(const pp::amp::ByteVector& identity_public_key) {
  if (identity_public_key.size() != pp::kMlDsa65PublicKeyBytes) {
    return pp::Error("invalid ML-DSA-65 public key size for test peer id");
  }
  unsigned char hash[crypto_generichash_BYTES_MIN];
  if (crypto_generichash(hash, sizeof(hash), identity_public_key.data(), identity_public_key.size(), nullptr, 0) != 0) {
    return pp::Error("test peer id hash failed");
  }
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out = "test1";
  out.reserve(5 + 32);
  for (size_t i = 0; i < 16; ++i) {
    out.push_back(kHex[hash[i] >> 4]);
    out.push_back(kHex[hash[i] & 0x0f]);
  }
  return out;
}

} // namespace

pp::Roe<std::string> DeriveTestPeerId(const pp::amp::ByteVector& identity_public_key) {
  return DeriveTestPeerIdStub(identity_public_key);
}

pp::amp::PeerLinkConfig AmpMeshTestLinkConfig() {
  pp::amp::PeerLinkConfig config;
  config.peer_id_from_identity = [](const pp::amp::ByteVector& identity_public_key) -> std::string {
    auto peer_id = DeriveTestPeerId(identity_public_key);
    if (!peer_id) {
      return {};
    }
    return *peer_id;
  };
  return config;
}

} // namespace pbr::test
