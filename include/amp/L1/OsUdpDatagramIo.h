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
 * wmem_max (often 208 KiB) unless the process may force more (CAP_NET_ADMIN, effective or only
 * permitted — `setcap cap_net_admin+p`: raised for the call alone); Stats() tells what was granted.
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
  Roe<void> SendSegments(const IpEndpoint& peer, std::span<const std::span<const uint8_t>> parts,
                         size_t segment) override;
  Roe<std::optional<std::pair<IpEndpoint, std::vector<uint8_t>>>> RecvFrom() override;
  Roe<size_t> RecvBatch(size_t max, const DatagramVisitor& visit) override;
  bool WaitReadable(std::chrono::milliseconds timeout) override;
  void WakeWaiter() override;
  IpEndpoint LocalEndpoint() const override { return local_; }
  std::optional<DatagramIoStats> Stats() const override;
  /** Binds `LocalEndpoint()` again (its actual port, not the one asked for) with the same options. */
  Roe<void> Reopen() override;

  OsUdpDatagramIo(const OsUdpDatagramIo&) = delete;
  OsUdpDatagramIo& operator=(const OsUdpDatagramIo&) = delete;

private:
  OsUdpDatagramIo(int fd, IpEndpoint local, UdpSocketOptions options);
  void CloseSocket();
  /** The wake handle WaitReadable polls beside the socket (a pipe; Windows: a loopback UDP socket). */
  void OpenWakeHandle();
  void DrainWakeHandle();
  /** One datagram into `buf`: its sender and length, or nullopt when none is ready. */
  Roe<std::optional<std::pair<IpEndpoint, size_t>>> RecvOneInto(std::span<uint8_t> buf);
  /** Linux: up to kRecvBatch datagrams per recvmmsg into buffers kept across calls. */
  Roe<size_t> RecvMany(size_t max, const DatagramVisitor& visit);
  void NoteReceiveDrops(uint32_t drops) { receive_drops_.store(drops, std::memory_order_relaxed); }

  /** Datagrams read per system call (recvmmsg). */
  static constexpr size_t kRecvBatch = 32;
  /** Room for one datagram (kMaxDatagramBytes fits). */
  static constexpr size_t kRecvBufferBytes = 2048;
  struct RecvBatchBuffers;
  std::unique_ptr<RecvBatchBuffers> batch_;

  int fd_ = -1;
  IpEndpoint local_;
  UdpSocketOptions options_;
  std::vector<uint8_t> recv_scratch_;
  /** UDP GSO worked or was not tried yet; false once the kernel refused it (SendSegments then loops SendTo). */
  bool gso_ok_ = true;
  /** POSIX: the pipe's read and write ends. Windows: [0] a loopback UDP socket, [1] unused. -1: none. */
  int wake_fds_[2] = {-1, -1};
  IpEndpoint wake_addr_;
  /** Kernel's count of datagrams dropped for want of receive buffer (Linux SO_RXQ_OVFL); -1: not told. */
  std::atomic<int64_t> receive_drops_{-1};
};

} // namespace pp::adp
