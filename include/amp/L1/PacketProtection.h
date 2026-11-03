#pragma once

#include "amp/L1/Types.h"

#include <cstdint>
#include <span>
#include <vector>

namespace pp::adp {

/**
 * The numbering a sealed packet's number belongs to. Each space counts on its own (L1 numbers best-effort and
 * reliable packets apart; a carrier-backed link numbers its inner packets), so two packets can carry the same number:
 * the space is part of the nonce.
 */
enum class PacketSpace : uint8_t {
  BestEffort = 0,
  Reliable = 1,
  CarrierBestEffort = 2,
  CarrierReliable = 3,
};

/** The AEAD tag a sealed packet ends with. */
inline constexpr size_t kPacketTagBytes = 16;

/**
 * Seals and opens one association's data packets: an AEAD whose nonce is derived from the packet's space and number,
 * never sent, with the packet's clear header as associated data. The session behind it holds the keys and follows
 * rekeys; L1 holds only this interface.
 *
 * The one rule a caller keeps: a (space, number) seals one plaintext under a key. A resend of the same packet seals
 * the same bytes again (the same ciphertext, which leaks nothing); a number is never reused for other bytes.
 */
class IPacketProtection {
public:
  virtual ~IPacketProtection() = default;

  /** Appends the ciphertext and tag of `plaintext` to `out`. */
  virtual Roe<void> SealPacket(PacketSpace space, uint32_t number, std::span<const uint8_t> header,
                               std::span<const uint8_t> plaintext, std::vector<uint8_t>& out) const = 0;

  /** The plaintext, or an error when the tag does not verify (forged, corrupted, or another key). */
  virtual Roe<std::vector<uint8_t>> OpenPacket(PacketSpace space, uint32_t number, std::span<const uint8_t> header,
                                               std::span<const uint8_t> sealed, int64_t now_ms) const = 0;
};

} // namespace pp::adp
