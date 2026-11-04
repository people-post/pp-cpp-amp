#pragma once

#include "amp/L1/Types.h"

#include <array>
#include <span>
#include <vector>

namespace pp::adp {

/** A sealed packet's clear header, read before its body is opened. */
struct SealedHeader {
  PacketType type = PacketType::DataBestEffort;
  AssocId assoc{};
  uint32_t number = 0;
};

class WireCodec {
public:
  /** An HMAC packet (handshake / control) without its HMAC; a sealed type is refused. */
  static Roe<std::vector<uint8_t>> Encode(const WirePacket& pkt);
  /** An HMAC packet, HMAC included (not checked here); a sealed type is refused. */
  static Roe<WirePacket> Decode(std::span<const uint8_t> datagram);

  /** Bytes covered by HMAC (everything except the trailing tag). */
  static std::span<const uint8_t> MacInput(std::span<const uint8_t> datagram);

  /** The datagram's type, if its version is ours. */
  static Roe<PacketType> PeekType(std::span<const uint8_t> datagram);
  /** The association id (the same place in both layouts); all zero when the datagram is too short to hold one. */
  static AssocId PeekAssoc(std::span<const uint8_t> datagram);

  /** The version negotiation reply to `received` (a datagram of another version), listing `versions`. */
  static std::vector<uint8_t> EncodeVersionNegotiation(std::span<const uint8_t> received,
                                                       std::span<const uint8_t> versions);
  /** A version negotiation datagram: the echo of the datagram it answers and the versions its sender speaks. */
  struct VersionNegotiation {
    std::array<uint8_t, kVersionNegotiationEchoBytes> echo{};
    std::vector<uint8_t> versions;
  };
  static Roe<VersionNegotiation> DecodeVersionNegotiation(std::span<const uint8_t> datagram);

  /** A sealed packet's clear header (kSealedHeaderBytes): the AEAD's associated data. */
  static std::array<uint8_t, kSealedHeaderBytes> EncodeSealedHeader(const SealedHeader& header);
  /** The clear header of a sealed datagram; its body (ciphertext + tag) follows kSealedHeaderBytes in. */
  static Roe<SealedHeader> DecodeSealedHeader(std::span<const uint8_t> datagram);
};

} // namespace pp::adp
