#pragma once

#include "amp/L1/DatagramIo.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>

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
  /**
   * poll() on the socket and the wake handle. It never says "ready" for ever: a socket the poll reports hung up or
   * closed (POLLHUP / POLLNVAL: iOS defuncts a suspended app's socket, a shutdown) is no longer polled — RecvBatch
   * reports it as an error until Reopen; one reported readable that then reads nothing twice running (an error
   * pending that reading does not clear) is left out of the poll for a while (50 ms, doubling to 1 s) and RecvBatch
   * says so once. A wake handle that hangs up or fails (its write end closed) is replaced. A poll that fails waits
   * out the timeout instead. Before, each of these returned at once, the read found nothing, and a driver waiting
   * here spun at 100 % of a core (pp-browser #596).
   */
  bool WaitReadable(std::chrono::milliseconds timeout) override;
  void WakeWaiter() override;
  IpEndpoint LocalEndpoint() const override { return local_; }
  std::optional<DatagramIoStats> Stats() const override;
  /** Binds `LocalEndpoint()` again (its actual port, not the one asked for) with the same options. */
  Roe<void> Reopen() override;

  OsUdpDatagramIo(const OsUdpDatagramIo&) = delete;
  OsUdpDatagramIo& operator=(const OsUdpDatagramIo&) = delete;

private:
  friend struct OsUdpDatagramIoTestPeer;

  OsUdpDatagramIo(int fd, IpEndpoint local, UdpSocketOptions options);
  void CloseSocket();
  /** The wake handle WaitReadable polls beside the socket (a pipe; Windows: a loopback UDP socket). wake_mu_ held. */
  void OpenWakeHandleLocked();
  /** False when the handle is spent (its write end closed: a read finds end of file). */
  bool DrainWakeHandle();
  /** A new wake handle for one the poll found broken (`read_end_invalid`: POLLNVAL, not ours to close any more). */
  void ReplaceWakeHandle(bool read_end_invalid);
  /** RecvBatch's read, then what a "ready" poll followed by nothing tells. */
  Roe<size_t> ReceiveReady(size_t max, const DatagramVisitor& visit);
  /** The socket may go in the poll: no fault seen, not paused. */
  bool SocketPollable(std::chrono::steady_clock::time_point now) const;

  /** What the poll last saw wrong with the socket (Reopen clears it). */
  enum class SocketFault : uint8_t { None, HungUp, Closed };
  /** A wait saw the socket readable; the next RecvBatch tells whether it was. */
  bool ready_reported_ = false;
  /** "Readable" polls in a row that read nothing. */
  int ready_misses_ = 0;
  static constexpr int kReadyMissLimit = 2;
  static constexpr std::chrono::milliseconds kFirstPollPause{50};
  static constexpr std::chrono::milliseconds kMaxPollPause{1000};
  std::chrono::milliseconds poll_pause_{0};
  std::chrono::steady_clock::time_point socket_paused_until_{};
  std::atomic<SocketFault> socket_fault_{SocketFault::None};
  std::atomic<uint64_t> wake_handle_replacements_{0};
  std::atomic<uint64_t> socket_poll_pauses_{0};
  /** One datagram into `buf`: its sender and length, or nullopt when none is ready. */
  Roe<std::optional<std::pair<IpEndpoint, size_t>>> RecvOneInto(std::span<uint8_t> buf);
  /** One recvfrom / recvmsg: a transient error (a signal, an earlier send's ICMP) as an error of its own code. */
  Roe<std::optional<std::pair<IpEndpoint, size_t>>> RecvOneAttempt(std::span<uint8_t> buf);
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
  /**
   * POSIX: the pipe's read and write ends. Windows: [0] a loopback UDP socket, [1] unused. -1: none. Written under
   * wake_mu_ (WakeWaiter, any thread, writes through it); [0] read by the waiting thread, the only one replacing it.
   */
  int wake_fds_[2] = {-1, -1};
  IpEndpoint wake_addr_;
  std::mutex wake_mu_;
  /** Kernel's count of datagrams dropped for want of receive buffer (Linux SO_RXQ_OVFL); -1: not told. */
  std::atomic<int64_t> receive_drops_{-1};
};

} // namespace pp::adp
