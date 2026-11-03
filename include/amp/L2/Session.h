#pragma once

#include "amp/L1/PacketProtection.h"
#include "amp/L1/Types.h"
#include "amp/L2/SessionCrypto.h"
#include "amp/L2/SessionKeys.h"
#include "amp/L2/Types.h"


#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace pp::amp {

/**
 * Established L2 session: its keys seal and open the link's packets (adp::IPacketProtection, A029 H1) and follow its
 * rekeys.
 */
class Session : public adp::IPacketProtection {
public:
  static Roe<Session> FromMaterial(SessionMaterial material, ByteVector master_ikm, ByteVector transcript_hash);

  const SessionMaterial& Material() const { return material_; }

  adp::PeerKey AssocKey() const;

  Roe<void> SealPacket(adp::PacketSpace space, uint32_t number, std::span<const uint8_t> header,
                       std::span<const uint8_t> plaintext, std::vector<uint8_t>& out) const override;

  /** Under the current key; in a rekey's grace window, under the previous one when the current one fails. */
  Roe<std::vector<uint8_t>> OpenPacket(adp::PacketSpace space, uint32_t number, std::span<const uint8_t> header,
                                       std::span<const uint8_t> sealed, int64_t now_ms) const override;

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
