#pragma once

#include "amp/L1/DatagramIo.h"

#include <atomic>
#include <memory>

namespace pp::adp {

/** Thin non-blocking UDP socket (POSIX / Winsock). Asio-free. */
/**
 * Socket buffers to ask the kernel for (0: the OS default). A node's one UDP socket carries every
 * peer: when its receive buffer overflows, the kernel drops datagrams of all peers alike —
 * handshakes, keepalives, other peers' requests. Linux grants at most net.core.rmem_max /
 * wmem_max (often 208 KiB) unless the process may force more (CAP_NET_ADMIN); Stats() tells what
 * was granted.
 */
struct UdpSocketOptions {
  size_t receive_buffer_bytes = 0;
  size_t send_buffer_bytes = 0;
};

class OsUdpDatagramIo final : public DatagramIo {
public:
  static Roe<std::unique_ptr<OsUdpDatagramIo>> Bind(const IpEndpoint& local, const UdpSocketOptions& options = {});

  ~OsUdpDatagramIo() override;

  Roe<void> SendTo(const IpEndpoint& peer, std::span<const uint8_t> datagram) override;
  Roe<std::optional<std::pair<IpEndpoint, std::vector<uint8_t>>>> RecvFrom() override;
  IpEndpoint LocalEndpoint() const override { return local_; }
  std::optional<DatagramIoStats> Stats() const override;

  OsUdpDatagramIo(const OsUdpDatagramIo&) = delete;
  OsUdpDatagramIo& operator=(const OsUdpDatagramIo&) = delete;

private:
  OsUdpDatagramIo(int fd, IpEndpoint local);

  int fd_ = -1;
  IpEndpoint local_;
  std::vector<uint8_t> recv_scratch_;
  /** Kernel's count of datagrams dropped for want of receive buffer (Linux SO_RXQ_OVFL); -1: not told. */
  std::atomic<int64_t> receive_drops_{-1};
};

} // namespace pp::adp
