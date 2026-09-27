#pragma once

#include "amp/L2/MshMessages.h"


#include <cstdint>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

namespace pp::amp {

enum class AmpAdpPayloadKind : uint8_t {
  Msh = 0,
  Sealed = 1,
  MshChunk = 2,
  /** Carrier reliable lane (CarrierLane): [seq u32][inner Sealed wire]. */
  LaneData = 3,
  /** Carrier reliable lane ack: [cumulative u32][selective u64]; cumulative 0 + no bits = probe. */
  LaneAck = 4,
};

struct LaneAckFields {
  uint32_t cumulative = 0;
  uint64_t selective = 0;
};

inline constexpr size_t kMaxMshBodyPerDatagram = 900;

class AmpAdpCarrier {
public:
  static Roe<std::vector<uint8_t>> EncodeMsh(MshMessageType type, std::span<const uint8_t> body);
  static Roe<std::vector<std::vector<uint8_t>>> EncodeMshChunked(MshMessageType type, std::span<const uint8_t> body);
  static Roe<std::vector<uint8_t>> EncodeSealed(uint32_t channel_id, uint32_t channel_seq,
                                                std::span<const uint8_t> sealed);

  static Roe<AmpAdpPayloadKind> DecodeKind(std::span<const uint8_t> payload);
  static Roe<MshMessageType> DecodeMshType(std::span<const uint8_t> payload);
  static Roe<std::vector<uint8_t>> DecodeMshBody(std::span<const uint8_t> payload);
  static Roe<std::tuple<MshMessageType, uint16_t, uint16_t, std::span<const uint8_t>>> DecodeMshChunk(
      std::span<const uint8_t> payload);
  static Roe<std::pair<uint32_t, uint32_t>> DecodeSealedHeader(std::span<const uint8_t> payload);
  static Roe<std::vector<uint8_t>> DecodeSealedBody(std::span<const uint8_t> payload);

  static std::vector<uint8_t> EncodeLaneData(uint32_t seq, std::span<const uint8_t> inner);
  /** (seq, inner wire) — the inner wire is an ordinary payload (Sealed). */
  static Roe<std::pair<uint32_t, std::span<const uint8_t>>> DecodeLaneData(std::span<const uint8_t> payload);
  static std::vector<uint8_t> EncodeLaneAck(const LaneAckFields& ack);
  static Roe<LaneAckFields> DecodeLaneAck(std::span<const uint8_t> payload);
};

} // namespace pp::amp
