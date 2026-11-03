#pragma once

#include "amp/L2/MshMessages.h"

#include <array>
#include <cstdint>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

namespace pp::amp {

enum class AmpAdpPayloadKind : uint8_t {
  Msh = 0,
  /** A carrier-backed link's sealed packet, best effort: [kind][number u32][sealed frame + tag] (A029 H1). */
  Sealed = 1,
  MshChunk = 2,
  /** Carrier reliable lane (CarrierLane): [seq u32][inner Sealed wire]. */
  LaneData = 3,
  /** Carrier reliable lane ack: [cumulative u32][selective u64]; cumulative 0 + no bits = probe. */
  LaneAck = 4,
  /** Like Sealed, reliable (its own packet space): what the carrier lane carries. */
  SealedReliable = 5,
};

/** A sealed carrier packet's clear header: kind, packet number. Its associated data. */
inline constexpr size_t kCarrierSealedHeaderBytes = 1 + 4;

/** A sealed carrier packet's clear header, decoded. */
struct CarrierSealedHeader {
  bool reliable = false;
  uint32_t number = 0;
};

struct LaneAckFields {
  uint32_t cumulative = 0;
  uint64_t selective = 0;
};

inline constexpr size_t kMaxMshBodyPerDatagram = 900;
/**
 * Largest real MSH v1 message (ClientPayload/ServerPayload: ML-KEM ciphertext + ML-DSA identity
 * key + signature + framing) is ~7.5 KiB; this leaves headroom without letting a peer-declared
 * chunk count reserve an unbounded number of assembly slots.
 */
inline constexpr size_t kMaxMshMessageBytes = 12'288;
inline constexpr uint16_t kMaxMshChunkCount =
    static_cast<uint16_t>((kMaxMshMessageBytes + kMaxMshBodyPerDatagram - 1) / kMaxMshBodyPerDatagram);

class AmpAdpCarrier {
public:
  static Roe<std::vector<uint8_t>> EncodeMsh(MshMessageType type, std::span<const uint8_t> body);
  static Roe<std::vector<std::vector<uint8_t>>> EncodeMshChunked(MshMessageType type, std::span<const uint8_t> body);
  /** The clear header of a sealed carrier packet; the session seals the frame after it. */
  static std::array<uint8_t, kCarrierSealedHeaderBytes> EncodeSealedHeader(bool reliable, uint32_t number);

  static Roe<AmpAdpPayloadKind> DecodeKind(std::span<const uint8_t> payload);
  static Roe<MshMessageType> DecodeMshType(std::span<const uint8_t> payload);
  static Roe<std::vector<uint8_t>> DecodeMshBody(std::span<const uint8_t> payload);
  static Roe<std::tuple<MshMessageType, uint16_t, uint16_t, std::span<const uint8_t>>> DecodeMshChunk(
      std::span<const uint8_t> payload);
  /** A sealed carrier packet's clear header; its body (frame ciphertext + tag) follows kCarrierSealedHeaderBytes in. */
  static Roe<CarrierSealedHeader> DecodeSealedHeader(std::span<const uint8_t> payload);

  static std::vector<uint8_t> EncodeLaneData(uint32_t seq, std::span<const uint8_t> inner);
  /** (seq, inner wire) — the inner wire is an ordinary payload (Sealed). */
  static Roe<std::pair<uint32_t, std::span<const uint8_t>>> DecodeLaneData(std::span<const uint8_t> payload);
  static std::vector<uint8_t> EncodeLaneAck(const LaneAckFields& ack);
  static Roe<LaneAckFields> DecodeLaneAck(std::span<const uint8_t> payload);
};

} // namespace pp::amp
