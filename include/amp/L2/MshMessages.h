#pragma once

#include "amp/L2/Types.h"


#include <cstdint>
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
  static Roe<MshHello> DecodeHello(MshMessageType expected, std::span<const uint8_t> wire);

  static Roe<std::vector<uint8_t>> EncodePayload(MshMessageType type, const MshPayload& payload);
  static Roe<MshPayload> DecodePayload(MshMessageType expected, std::span<const uint8_t> wire);

  static Roe<std::vector<uint8_t>> EncodeFinished(const MshFinished& finished);
  static Roe<MshFinished> DecodeFinished(std::span<const uint8_t> wire);

  static Roe<ByteVector> BuildIdentitySignMessage(const ByteVector& static_kem_public_key);
};

} // namespace pp::amp
