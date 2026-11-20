#pragma once

#include "amp/L2/Types.h"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace pp::amp {

enum class MshMessageType : uint8_t {
  ClientHello = 1,
  ServerHello = 2,
  ClientPayload = 3,
  ServerPayload = 4,
  Finished = 5,
};

/** Whether this release knows `type` (ClientHello … Finished). */
inline constexpr bool IsKnownMshMessageType(const uint8_t type) {
  return type >= static_cast<uint8_t>(MshMessageType::ClientHello) &&
         type <= static_cast<uint8_t>(MshMessageType::Finished);
}

/** Error::code of a hello naming an MSH version other than kMshVersion (MshMessages::DecodeHello). */
inline constexpr int32_t kMshErrUnsupportedVersion = 1;

struct MshHello {
  uint8_t version = kMshVersion;
  ByteVector kem_public_key;
  ByteVector nonce;
};

struct MshPayload {
  ByteVector kem_ciphertext;
  ByteVector identity_public_key;
  ByteVector static_kem_public_key;
  ByteVector identity_signature;
};

struct MshFinished {
  ByteVector mac;
};

class MshMessages {
public:
  static Roe<std::vector<uint8_t>> EncodeHello(MshMessageType type, const MshHello& hello);
  /** A hello of another MSH version fails with Error::code kMshErrUnsupportedVersion (PeekHelloVersion names it). */
  static Roe<MshHello> DecodeHello(MshMessageType expected, std::span<const uint8_t> wire);
  /** A hello's version byte, read before anything else in it; nullopt when it is too short to hold one. */
  static std::optional<uint8_t> PeekHelloVersion(std::span<const uint8_t> wire);

  static Roe<std::vector<uint8_t>> EncodePayload(MshMessageType type, const MshPayload& payload);
  static Roe<MshPayload> DecodePayload(MshMessageType expected, std::span<const uint8_t> wire);

  static Roe<std::vector<uint8_t>> EncodeFinished(const MshFinished& finished);
  static Roe<MshFinished> DecodeFinished(std::span<const uint8_t> wire);

  static Roe<ByteVector> BuildIdentitySignMessage(const ByteVector& static_kem_public_key);
};

} // namespace pp::amp
