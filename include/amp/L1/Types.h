#pragma once

#include "common/Error.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace pp::adp {

using pp::Error;
using pp::Roe;

inline constexpr uint8_t kWireVersion = 1;
inline constexpr size_t kAssocIdBytes = 16;
inline constexpr size_t kHmacBytes = 16;
inline constexpr size_t kKeyBytes = 32;
inline constexpr size_t kHeaderBytes = 28; // before payload + hmac
inline constexpr uint16_t kMaxPayload = 1200;
inline constexpr int64_t kDefaultSkewMs = 60'000;
inline constexpr size_t kDefaultReplayWindow = 64;
inline constexpr size_t kDefaultReliableWindow = 16;
inline constexpr int64_t kDefaultRtxIntervalMs = 50;
inline constexpr int kDefaultMaxRtx = 20;
inline constexpr int64_t kAliveTimeoutMs = 5'000;

enum class QosClass : uint8_t {
  BestEffort = 0,
  Reliable = 1,
};

enum class PacketType : uint8_t {
  DataBestEffort = 0,
  DataReliable = 1,
  Ack = 2,
  Close = 3,
};

struct AssocId {
  std::array<uint8_t, kAssocIdBytes> bytes{};

  bool operator==(const AssocId& o) const { return bytes == o.bytes; }
  bool operator!=(const AssocId& o) const { return !(*this == o); }
};

struct AssocIdHash {
  size_t operator()(const AssocId& id) const noexcept {
    // FNV-1a in uint64_t so 32-bit Android size_t does not truncate the constants.
    uint64_t h = 14695981039346656037ull;
    for (uint8_t b : id.bytes) {
      h ^= b;
      h *= 1099511628211ull;
    }
    return static_cast<size_t>(h);
  }
};

struct PeerKey {
  std::array<uint8_t, kKeyBytes> bytes{};
};

/** Portable UDP endpoint (no Asio). IPv4 mapped into first 4 bytes when V4. */
struct IpEndpoint {
  enum class Family : uint8_t { V4 = 4, V6 = 6 };

  Family family = Family::V4;
  std::array<uint8_t, 16> addr{};
  uint16_t port = 0;

  static IpEndpoint V4(uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint16_t port) {
    IpEndpoint e;
    e.family = Family::V4;
    e.addr[0] = a;
    e.addr[1] = b;
    e.addr[2] = c;
    e.addr[3] = d;
    e.port = port;
    return e;
  }

  bool operator==(const IpEndpoint& o) const {
    if (family != o.family || port != o.port) {
      return false;
    }
    const size_t n = family == Family::V4 ? 4 : 16;
    return std::memcmp(addr.data(), o.addr.data(), n) == 0;
  }
  bool operator!=(const IpEndpoint& o) const { return !(*this == o); }
};

struct IpEndpointHash {
  size_t operator()(const IpEndpoint& e) const noexcept {
    size_t h = static_cast<size_t>(e.port) * 1315423911u;
    const size_t n = e.family == IpEndpoint::Family::V4 ? 4 : 16;
    for (size_t i = 0; i < n; ++i) {
      h ^= e.addr[i];
      h *= 16777619u;
    }
    return h;
  }
};

struct WirePacket {
  uint8_t version = kWireVersion;
  PacketType type = PacketType::DataBestEffort;
  AssocId assoc{};
  uint32_t seq = 0;
  uint32_t timestamp_ms = 0;
  std::vector<uint8_t> payload;
};

struct Message {
  AssocId assoc{};
  uint32_t seq = 0;
  QosClass qos = QosClass::BestEffort;
  std::vector<uint8_t> payload;
};

using MessageHandler = std::function<void(const Message&)>;
using PathChangeHandler = std::function<void(const IpEndpoint& from, const IpEndpoint& to)>;

} // namespace pp::adp
