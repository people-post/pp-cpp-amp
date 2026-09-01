#include "amp/L2/SessionKeys.h"

#include "crypto/SodiumUtil.h"

#include <sodium.h>

#include <cstring>

namespace pp::amp {

namespace {

Roe<ByteVector> HkdfExpand(const ByteVector& prk, const char* info, size_t out_len) {
  ByteVector out(out_len);
  if (crypto_kdf_hkdf_sha256_expand(out.data(), out.size(), info, std::strlen(info), prk.data()) != 0) {
    return Error("amp: HKDF expand failed");
  }
  return out;
}

Roe<ByteVector> HkdfExtract(std::span<const uint8_t> ikm) {
  ByteVector prk(crypto_kdf_hkdf_sha256_KEYBYTES);
  if (crypto_kdf_hkdf_sha256_extract(prk.data(), reinterpret_cast<const unsigned char*>(kAmpHkdfSalt),
                                     std::strlen(kAmpHkdfSalt), ikm.data(), ikm.size()) != 0) {
    return Error("amp: HKDF extract failed");
  }
  return prk;
}

Roe<ByteVector> DeriveLabel(const ByteVector& prk, const char* label, uint32_t session_epoch) {
  const std::string info = std::string(label) + "|epoch:" + std::to_string(session_epoch);
  return HkdfExpand(prk, info.c_str(), kSessionKeyBytes);
}

} // namespace

Roe<ByteVector> SessionKeys::TranscriptHash(const std::vector<ByteVector>& transcript_parts) {
  pp::EnsureSodiumInit();
  crypto_hash_sha256_state state;
  crypto_hash_sha256_init(&state);
  for (const auto& part : transcript_parts) {
    crypto_hash_sha256_update(&state, part.data(), part.size());
  }
  ByteVector hash(crypto_hash_sha256_BYTES);
  crypto_hash_sha256_final(&state, hash.data());
  return hash;
}

Roe<SessionMaterial> SessionKeys::Derive(std::span<const uint8_t> master_ikm,
                                           std::span<const uint8_t> transcript_hash, bool initiator,
                                           uint32_t session_epoch) {
  if (master_ikm.empty()) {
    return Error("amp: empty master ikm");
  }
  if (transcript_hash.size() != crypto_hash_sha256_BYTES) {
    return Error("amp: bad transcript hash size");
  }
  pp::EnsureSodiumInit();

  ByteVector ikm;
  ikm.reserve(master_ikm.size() + transcript_hash.size());
  ikm.insert(ikm.end(), master_ikm.begin(), master_ikm.end());
  ikm.insert(ikm.end(), transcript_hash.begin(), transcript_hash.end());

  auto prk = HkdfExtract(ikm);
  if (!prk) {
    return prk.error();
  }

  auto k_assoc = HkdfExpand(*prk, kAmpKAssocInfo, kSessionKeyBytes);
  if (!k_assoc) {
    return k_assoc.error();
  }
  auto k_client = DeriveLabel(*prk, kAmpKClientInfo, session_epoch);
  if (!k_client) {
    return k_client.error();
  }
  auto k_server = DeriveLabel(*prk, kAmpKServerInfo, session_epoch);
  if (!k_server) {
    return k_server.error();
  }

  SessionMaterial material;
  material.k_assoc = std::move(*k_assoc);
  material.session_epoch = session_epoch;
  material.initiator = initiator;
  material.k_send = initiator ? std::move(*k_client) : std::move(*k_server);
  material.k_recv = initiator ? std::move(*k_server) : std::move(*k_client);
  return material;
}

} // namespace pp::amp
