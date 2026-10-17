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

/** What an OS socket tells about itself (OsUdpDatagramIo::Stats). */
struct DatagramIoStats {
  /** Buffers the kernel granted, in bytes (what is usable: Linux's doubled figure halved). */
  size_t receive_buffer_bytes = 0;
  size_t send_buffer_bytes = 0;
  /** Datagrams the kernel dropped for want of receive buffer, since the socket opened (Linux). */
  std::optional<uint64_t> receive_drops;
};

class DatagramIo {
public:
  virtual ~DatagramIo() = default;
  /** The socket's buffers and drops, where the platform tells; none for in-memory IO. */
  virtual std::optional<DatagramIoStats> Stats() const { return std::nullopt; }

  virtual Roe<void> SendTo(const IpEndpoint& peer, std::span<const uint8_t> datagram) = 0;

  /** Non-blocking: returns nullopt if no datagram ready. */
  virtual Roe<std::optional<std::pair<IpEndpoint, std::vector<uint8_t>>>> RecvFrom() = 0;

  virtual IpEndpoint LocalEndpoint() const = 0;
};

} // namespace pp::adp
