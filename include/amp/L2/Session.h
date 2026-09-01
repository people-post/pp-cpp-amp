#pragma once

#include "amp/L1/Types.h"
#include "amp/L2/SessionCrypto.h"
#include "amp/L2/SessionKeys.h"
#include "amp/L2/Types.h"


#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace pp::amp {

/** Established L2 session — seal/open L3 payloads. */
class Session {
public:
  static Roe<Session> FromMaterial(SessionMaterial material, ByteVector master_ikm, ByteVector transcript_hash);

  const SessionMaterial& Material() const { return material_; }

  adp::PeerKey AssocKey() const;

  Roe<std::vector<uint8_t>> Seal(uint32_t channel_id, uint32_t channel_seq,
                                 std::span<const uint8_t> plaintext) const;

  Roe<std::vector<uint8_t>> Open(uint32_t channel_id, uint32_t channel_seq, std::span<const uint8_t> sealed,
                                 int64_t now_ms = 0) const;

  /** Bump epoch and re-derive directional keys (assoc key unchanged). */
  Roe<void> Rekey();

  /** Coordinated rekey — retain previous recv key for grace window. */
  Roe<void> ApplyRekey(uint32_t target_epoch, int64_t now_ms);

  bool HasGraceRecvKey(int64_t now_ms) const;

private:
  explicit Session(SessionMaterial material, ByteVector master_ikm, ByteVector transcript_hash);

  Direction OutDirection() const;
  Direction InDirection() const;

  SessionMaterial material_;
  ByteVector master_ikm_;
  ByteVector transcript_hash_;
  std::optional<ByteVector> previous_recv_key_;
  int64_t grace_until_ms_ = 0;
};

} // namespace pp::amp
