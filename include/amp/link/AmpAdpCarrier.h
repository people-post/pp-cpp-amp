#pragma once

#include "amp/L2/MshMessages.h"


#include <cstdint>
#include <span>
#include <tuple>
#include <vector>

namespace pp::amp {

enum class AmpAdpPayloadKind : uint8_t {
  Msh = 0,
  Sealed = 1,
  MshChunk = 2,
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
};

} // namespace pp::amp
