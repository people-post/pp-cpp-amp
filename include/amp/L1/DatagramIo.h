#pragma once

#include "amp/L1/Types.h"


#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace pp::adp {

/**
 * `Error::code` of a SendTo failure that means "no route to this peer right now" (host down / host
 * unreachable / network unreachable / network down) — the association to it cannot work until the
 * path changes, so the link above drops it at once instead of waiting out its liveness window.
 */
inline constexpr int32_t kDatagramSendUnreachable = 1;

inline bool IsUnreachableSendError(const Error& error) { return error.code == kDatagramSendUnreachable; }

class DatagramIo {
public:
  virtual ~DatagramIo() = default;

  virtual Roe<void> SendTo(const IpEndpoint& peer, std::span<const uint8_t> datagram) = 0;

  /** Non-blocking: returns nullopt if no datagram ready. */
  virtual Roe<std::optional<std::pair<IpEndpoint, std::vector<uint8_t>>>> RecvFrom() = 0;

  virtual IpEndpoint LocalEndpoint() const = 0;
};

} // namespace pp::adp
