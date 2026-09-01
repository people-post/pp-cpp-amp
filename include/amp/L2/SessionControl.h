#pragma once

#include "amp/L2/Types.h"

#include <cstdint>
#include <span>
#include <vector>

namespace pp::amp {

inline constexpr uint8_t kSessionControlWireVersion = 2;

enum class SessionControlKind : uint8_t {
  RekeyRequest = 1,
  RekeyAck = 2,
};

struct SessionRekeyMessage {
  SessionControlKind kind = SessionControlKind::RekeyRequest;
  uint32_t target_epoch = 0;
};

class SessionControlCodec {
public:
  static bool LooksLike(std::span<const uint8_t> wire);

  static Roe<std::vector<uint8_t>> Encode(const SessionRekeyMessage& message);
  static Roe<SessionRekeyMessage> Decode(std::span<const uint8_t> wire);
};

inline constexpr int64_t kSessionRekeyGraceMs = 1000;

} // namespace pp::amp
