#include "amp/L1/OsUdpDatagramIo.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using socklen_t = int;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/udp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#endif
#if defined(__linux__)
#include <linux/capability.h>
#include <sys/syscall.h>
// The kernel's UDP GSO ABI (4.18+); Android's libc headers may not name it.
#ifndef SOL_UDP
#define SOL_UDP 17
#endif
#ifndef UDP_SEGMENT
#define UDP_SEGMENT 103
#endif
#endif

namespace pp::adp {
namespace {

Error IoErr(const char* what) {
#if defined(_WIN32)
  const int err = WSAGetLastError();
#else
  const int err = errno;
#endif
  return Error(std::string("adp udp: ") + what + " errno=" + std::to_string(err));
}

std::string EndpointLabel(const IpEndpoint& ep) {
  char buf[INET6_ADDRSTRLEN] = {};
  const int af = ep.family == IpEndpoint::Family::V4 ? AF_INET : AF_INET6;
  if (::inet_ntop(af, ep.addr.data(), buf, sizeof(buf)) == nullptr) {
    return "?:" + std::to_string(ep.port);
  }
  return std::string(buf) + ":" + std::to_string(ep.port);
}

bool ToSockAddr(const IpEndpoint& ep, sockaddr_storage& ss, socklen_t& len) {
  std::memset(&ss, 0, sizeof(ss));
  if (ep.family == IpEndpoint::Family::V4) {
    auto* a = reinterpret_cast<sockaddr_in*>(&ss);
    a->sin_family = AF_INET;
    a->sin_port = htons(ep.port);
    std::memcpy(&a->sin_addr, ep.addr.data(), 4);
    len = sizeof(sockaddr_in);
    return true;
  }
  auto* a = reinterpret_cast<sockaddr_in6*>(&ss);
  a->sin6_family = AF_INET6;
  a->sin6_port = htons(ep.port);
  a->sin6_scope_id = ep.scope_id;
  std::memcpy(&a->sin6_addr, ep.addr.data(), 16);
  len = sizeof(sockaddr_in6);
  return true;
}

IpEndpoint FromSockAddr(const sockaddr_storage& ss) {
  if (ss.ss_family == AF_INET) {
    const auto* a = reinterpret_cast<const sockaddr_in*>(&ss);
    const auto* bytes = reinterpret_cast<const uint8_t*>(&a->sin_addr);
    return IpEndpoint::V4(bytes[0], bytes[1], bytes[2], bytes[3], ntohs(a->sin_port));
  }
  const auto* a = reinterpret_cast<const sockaddr_in6*>(&ss);
  // Dual-stack (::) sockets deliver IPv4 peers as v4-mapped; keep the dial book in V4 form.
  if (IN6_IS_ADDR_V4MAPPED(&a->sin6_addr)) {
    const auto* b = reinterpret_cast<const uint8_t*>(&a->sin6_addr) + 12;
    return IpEndpoint::V4(b[0], b[1], b[2], b[3], ntohs(a->sin6_port));
  }
  IpEndpoint e;
  e.family = IpEndpoint::Family::V6;
  std::memcpy(e.addr.data(), &a->sin6_addr, 16);
  e.port = ntohs(a->sin6_port);
  e.scope_id = a->sin6_scope_id; // A3: preserve iface scope for fe80 replies
  return e;
}

#if defined(_WIN32)
struct WinsockOnce {
  WinsockOnce() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
  }
};
void EnsureWinsock() {
  static WinsockOnce once;
  (void)once;
}
#else
void EnsureWinsock() {}
#endif

void CloseFd(int fd) {
#if defined(_WIN32)
  closesocket(fd);
#else
  ::close(fd);
#endif
}

/**
 * The receive error just returned (errno / WSAGetLastError) says nothing about the socket: a signal, or an ICMP
 * error for an earlier send (Windows reports one as WSAECONNRESET on the next receive unless told not to). The read
 * goes on; such an error is never "the socket failed".
 */
bool TransientReceiveError() {
#if defined(_WIN32)
  const int err = WSAGetLastError();
  return err == WSAECONNRESET || err == WSAENETRESET || err == WSAEINTR;
#else
  return errno == EINTR || errno == ECONNREFUSED;
#endif
}

/** Reads of one datagram that may hit a transient error before giving up for this call. */
constexpr int kTransientRetries = 8;
/** Error::code of a read that hit a transient error (RecvOneAttempt; never leaves this file). */
constexpr int32_t kTransientReceive = -2;

} // namespace

#if defined(__linux__)
struct OsUdpDatagramIo::RecvBatchBuffers {
  std::vector<uint8_t> data = std::vector<uint8_t>(kRecvBatch * kRecvBufferBytes);
  std::array<mmsghdr, kRecvBatch> msgs{};
  std::array<iovec, kRecvBatch> iovs{};
  std::array<sockaddr_storage, kRecvBatch> names{};
  struct Control {
    alignas(cmsghdr) char bytes[CMSG_SPACE(sizeof(uint32_t))];
  };
  std::array<Control, kRecvBatch> controls{};
};
#else
struct OsUdpDatagramIo::RecvBatchBuffers {};
#endif

OsUdpDatagramIo::OsUdpDatagramIo(int fd, IpEndpoint local, UdpSocketOptions options)
    : fd_(fd), local_(local), options_(options) {
  std::lock_guard lock(wake_mu_);
  OpenWakeHandleLocked();
}

OsUdpDatagramIo::~OsUdpDatagramIo() {
  CloseSocket();
  std::lock_guard lock(wake_mu_);
  for (int& w : wake_fds_) {
    if (w >= 0) {
      CloseFd(w);
      w = -1;
    }
  }
}

void OsUdpDatagramIo::OpenWakeHandleLocked() {
  // Without one, a wake waits out the poll's timeout: slower, never wrong.
#if defined(_WIN32)
  // WSAPoll takes sockets only: the wake is a datagram to a loopback socket of our own.
  const SOCKET w = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (w == INVALID_SOCKET) {
    return;
  }
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  int len = sizeof(a);
  u_long nonblocking = 1;
  if (::bind(w, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 ||
      ::getsockname(w, reinterpret_cast<sockaddr*>(&a), &len) != 0 || ioctlsocket(w, FIONBIO, &nonblocking) != 0) {
    closesocket(w);
    return;
  }
  wake_fds_[0] = static_cast<int>(w);
  wake_addr_ = IpEndpoint::V4(127, 0, 0, 1, ntohs(a.sin_port));
#else
  int p[2] = {-1, -1};
  if (::pipe(p) != 0) {
    return;
  }
  for (const int end : p) {
    ::fcntl(end, F_SETFL, ::fcntl(end, F_GETFL, 0) | O_NONBLOCK);
    ::fcntl(end, F_SETFD, FD_CLOEXEC);
  }
  wake_fds_[0] = p[0];
  wake_fds_[1] = p[1];
#endif
}

void OsUdpDatagramIo::WakeWaiter() {
  DatagramIo::WakeWaiter(); // a wait that cannot poll (no handle left, a failed poll) waits on the base's
  const char byte = 1;
  std::lock_guard lock(wake_mu_);
#if defined(_WIN32)
  if (wake_fds_[0] >= 0) {
    sockaddr_storage ss{};
    socklen_t len = 0;
    ToSockAddr(wake_addr_, ss, len);
    (void)::sendto(static_cast<SOCKET>(wake_fds_[0]), &byte, 1, 0, reinterpret_cast<sockaddr*>(&ss), len);
  }
#else
  if (wake_fds_[1] >= 0) {
    (void)!::write(wake_fds_[1], &byte, 1); // full pipe: a wake is already pending
  }
#endif
}

bool OsUdpDatagramIo::DrainWakeHandle() {
  char buf[64];
  for (;;) {
#if defined(_WIN32)
    const int n = wake_fds_[0] >= 0 ? ::recv(static_cast<SOCKET>(wake_fds_[0]), buf, sizeof(buf), 0) : -1;
    if (n < 0) {
      return WSAGetLastError() == WSAEWOULDBLOCK || WSAGetLastError() == WSAECONNRESET;
    }
#else
    const auto n = wake_fds_[0] >= 0 ? ::read(wake_fds_[0], buf, sizeof(buf)) : -1;
    if (n == 0) {
      return false; // end of file: every write end is closed, the pipe can never wake us again
    }
    if (n < 0) {
      return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
    }
#endif
  }
}

void OsUdpDatagramIo::ReplaceWakeHandle(const bool read_end_invalid) {
  wake_handle_replacements_.fetch_add(1, std::memory_order_relaxed);
  std::lock_guard lock(wake_mu_);
  // The read end is ours to close unless the poll called it invalid (its number may be another file's then). A pipe
  // whose read end hangs up has no write end left: someone closed ours, and its number is not ours to close (or
  // write) any more. A failed Windows wake socket is ours.
  if (!read_end_invalid && wake_fds_[0] >= 0) {
    CloseFd(wake_fds_[0]);
  }
  wake_fds_[0] = -1;
  wake_fds_[1] = -1;
  OpenWakeHandleLocked();
}

bool OsUdpDatagramIo::SocketPollable(const std::chrono::steady_clock::time_point now) const {
  return fd_ >= 0 && socket_fault_.load(std::memory_order_relaxed) == SocketFault::None && now >= socket_paused_until_;
}

bool OsUdpDatagramIo::WaitReadable(const std::chrono::milliseconds timeout) {
  const int ms = static_cast<int>(std::clamp<int64_t>(timeout.count(), 0, 60000));
  const bool poll_socket = SocketPollable(std::chrono::steady_clock::now());
#if defined(_WIN32)
  WSAPOLLFD fds[2] = {};
  ULONG count = 0;
  int socket_at = -1;
  int wake_at = -1;
  if (poll_socket) {
    fds[count].fd = static_cast<SOCKET>(fd_);
    fds[count].events = POLLRDNORM;
    socket_at = static_cast<int>(count++);
  }
  if (wake_fds_[0] >= 0) {
    fds[count].fd = static_cast<SOCKET>(wake_fds_[0]);
    fds[count].events = POLLRDNORM;
    wake_at = static_cast<int>(count++);
  }
  if (count == 0) {
    return DatagramIo::WaitReadable(timeout);
  }
  const int n = ::WSAPoll(fds, count, ms);
  if (n < 0) {
    return DatagramIo::WaitReadable(timeout); // the poll itself failed: never return at once over and over
  }
  constexpr short kReadable = POLLRDNORM;
#else
  // A negative fd is ignored by poll(): no socket (a failed reopen, a fault) or no wake handle.
  pollfd fds[2] = {};
  int count = 0;
  int socket_at = -1;
  int wake_at = -1;
  if (poll_socket) {
    fds[count].fd = fd_;
    fds[count].events = POLLIN;
    socket_at = count++;
  }
  if (wake_fds_[0] >= 0) {
    fds[count].fd = wake_fds_[0];
    fds[count].events = POLLIN;
    wake_at = count++;
  }
  if (count == 0) {
    return DatagramIo::WaitReadable(timeout);
  }
  const int n = ::poll(fds, static_cast<nfds_t>(count), ms);
  if (n < 0) {
    if (errno == EINTR) {
      return false; // a signal: once, not a state
    }
    return DatagramIo::WaitReadable(timeout); // the poll itself failed: never return at once over and over
  }
  constexpr short kReadable = POLLIN;
#endif
  if (n == 0) {
    return false;
  }
  if (wake_at >= 0) {
    const short revents = fds[wake_at].revents;
    if ((revents & (POLLHUP | POLLERR | POLLNVAL)) != 0 || ((revents & kReadable) != 0 && !DrainWakeHandle())) {
      ReplaceWakeHandle((revents & POLLNVAL) != 0);
    }
  }
  if (socket_at < 0) {
    return false;
  }
  const short revents = fds[socket_at].revents;
  if ((revents & POLLNVAL) != 0) {
    socket_fault_.store(SocketFault::Closed, std::memory_order_relaxed); // closed under us: RecvBatch says so
    return true;
  }
  if ((revents & POLLHUP) != 0) {
    socket_fault_.store(SocketFault::HungUp, std::memory_order_relaxed); // shut down / defunct: it reads no more
    return true;
  }
  if ((revents & (kReadable | POLLERR)) != 0) {
    ready_reported_ = true;
    return true;
  }
  return false;
}

void OsUdpDatagramIo::CloseSocket() {
  if (fd_ >= 0) {
    CloseFd(fd_);
    fd_ = -1;
  }
}

#if defined(__linux__)
/**
 * CAP_NET_ADMIN made effective on this thread while in scope, when it is permitted but not
 * effective — a binary marked `setcap cap_net_admin+p`, e.g. for a container's non-root user that
 * cannot raise the host's sysctl cap. (`+ep` would make it effective throughout, and stop the
 * binary from starting at all where the capability is not granted.) The previous effective set
 * comes back when the scope ends.
 */
class ScopedNetAdmin {
public:
  ScopedNetAdmin() {
    header_.version = _LINUX_CAPABILITY_VERSION_3;
    header_.pid = 0;
    if (syscall(SYS_capget, &header_, saved_) != 0) {
      return;
    }
    const auto index = CAP_TO_INDEX(CAP_NET_ADMIN);
    const auto mask = CAP_TO_MASK(CAP_NET_ADMIN);
    if ((saved_[index].permitted & mask) == 0 || (saved_[index].effective & mask) != 0) {
      return;
    }
    __user_cap_data_struct raised[_LINUX_CAPABILITY_U32S_3];
    std::memcpy(raised, saved_, sizeof(raised));
    raised[index].effective |= mask;
    raised_ = syscall(SYS_capset, &header_, raised) == 0;
  }
  ~ScopedNetAdmin() {
    if (raised_) {
      syscall(SYS_capset, &header_, saved_);
    }
  }
  ScopedNetAdmin(const ScopedNetAdmin&) = delete;
  ScopedNetAdmin& operator=(const ScopedNetAdmin&) = delete;

private:
  __user_cap_header_struct header_{};
  __user_cap_data_struct saved_[_LINUX_CAPABILITY_U32S_3]{};
  bool raised_ = false;
};
#endif

/** Ask for `bytes` of `option` (SO_RCVBUF / SO_SNDBUF); on Linux, force it past the sysctl cap when allowed. */
static void SetBuffer(int fd, int option, [[maybe_unused]] int force_option, size_t bytes) {
  if (bytes == 0) {
    return;
  }
  const int value = static_cast<int>(std::min<size_t>(bytes, static_cast<size_t>(INT32_MAX / 2)));
#if defined(__linux__)
  {
    const ScopedNetAdmin net_admin;
    if (setsockopt(fd, SOL_SOCKET, force_option, &value, sizeof(value)) == 0) {
      return; // CAP_NET_ADMIN: past net.core.rmem_max / wmem_max
    }
  }
#endif
#if defined(_WIN32)
  setsockopt(fd, SOL_SOCKET, option, reinterpret_cast<const char*>(&value), sizeof(value));
#else
  setsockopt(fd, SOL_SOCKET, option, &value, sizeof(value));
#endif
}

static size_t GetBuffer(int fd, int option) {
  int value = 0;
  socklen_t len = sizeof(value);
#if defined(_WIN32)
  if (getsockopt(fd, SOL_SOCKET, option, reinterpret_cast<char*>(&value), &len) != 0) {
    return 0;
  }
#else
  if (getsockopt(fd, SOL_SOCKET, option, &value, &len) != 0) {
    return 0;
  }
#endif
#if defined(__linux__)
  value /= 2; // Linux reports twice what it grants (bookkeeping overhead)
#endif
  return value > 0 ? static_cast<size_t>(value) : 0;
}

static Roe<std::pair<int, IpEndpoint>> OpenSocket(const IpEndpoint& local, const UdpSocketOptions& options) {
  EnsureWinsock();
  const int family = local.family == IpEndpoint::Family::V4 ? AF_INET : AF_INET6;
  const int fd = static_cast<int>(::socket(family, SOCK_DGRAM, IPPROTO_UDP));
  if (fd < 0) {
    return IoErr("socket");
  }
#if defined(_WIN32)
  u_long mode = 1;
  ioctlsocket(fd, FIONBIO, &mode);
  // An ICMP port unreachable for one send must not fail the next receive (WSAECONNRESET): UDP has no connection.
  BOOL report_reset = FALSE;
  DWORD returned = 0;
  (void)WSAIoctl(static_cast<SOCKET>(fd), _WSAIOW(IOC_VENDOR, 12) /* SIO_UDP_CONNRESET */, &report_reset,
                 sizeof(report_reset), nullptr, 0, &returned, nullptr, nullptr);
#else
  const int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
#if defined(__linux__)
  SetBuffer(fd, SO_RCVBUF, SO_RCVBUFFORCE, options.receive_buffer_bytes);
  SetBuffer(fd, SO_SNDBUF, SO_SNDBUFFORCE, options.send_buffer_bytes);
  const int overflow = 1; // the kernel's drop count rides each received datagram (SO_RXQ_OVFL)
  setsockopt(fd, SOL_SOCKET, SO_RXQ_OVFL, &overflow, sizeof(overflow));
#else
  SetBuffer(fd, SO_RCVBUF, 0, options.receive_buffer_bytes);
  SetBuffer(fd, SO_SNDBUF, 0, options.send_buffer_bytes);
#endif
  // Dual-stack: AF_INET6 wildcard (::) should also accept IPv4-mapped when OS allows.
  if (local.family == IpEndpoint::Family::V6) {
    int v6only = 0;
#if defined(_WIN32)
    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&v6only), sizeof(v6only));
#else
    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
#endif
  }
  sockaddr_storage ss{};
  socklen_t len = 0;
  ToSockAddr(local, ss, len);
  if (::bind(fd, reinterpret_cast<sockaddr*>(&ss), len) != 0) {
    CloseFd(fd);
    return IoErr("bind");
  }
  sockaddr_storage bound{};
  socklen_t blen = sizeof(bound);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &blen) != 0) {
    CloseFd(fd);
    return IoErr("getsockname");
  }
  return std::pair<int, IpEndpoint>{fd, FromSockAddr(bound)};
}

Roe<std::unique_ptr<OsUdpDatagramIo>> OsUdpDatagramIo::Bind(const IpEndpoint& local, const UdpSocketOptions& options) {
  auto opened = OpenSocket(local, options);
  if (!opened) {
    return opened.error();
  }
  return std::unique_ptr<OsUdpDatagramIo>(new OsUdpDatagramIo(opened->first, opened->second, options));
}

Roe<void> OsUdpDatagramIo::Reopen() {
  // A socket the poll called closed is not ours to close any more (its number may be another file's).
  if (socket_fault_.load(std::memory_order_relaxed) == SocketFault::Closed) {
    fd_ = -1;
  }
  CloseSocket(); // first: the new socket takes the same port
  socket_fault_.store(SocketFault::None, std::memory_order_relaxed);
  ready_reported_ = false;
  ready_misses_ = 0;
  poll_pause_ = std::chrono::milliseconds(0);
  socket_paused_until_ = {};
  auto opened = OpenSocket(local_, options_);
  if (!opened) {
    return opened.error();
  }
  fd_ = opened->first;
  receive_drops_.store(-1, std::memory_order_relaxed);
  return {};
}

namespace {

/** `peer` as the sockaddr a socket bound on `local` sends to (a dual-stack socket: ::ffff:a.b.c.d). */
void PeerSockAddr(const IpEndpoint& local, const IpEndpoint& peer, sockaddr_storage& ss, socklen_t& len) {
  if (local.family == IpEndpoint::Family::V6 && peer.family == IpEndpoint::Family::V4) {
    // Dual-stack socket: an AF_INET sockaddr is rejected (EINVAL on Darwin); send to ::ffff:a.b.c.d.
    std::memset(&ss, 0, sizeof(ss));
    auto* a6 = reinterpret_cast<sockaddr_in6*>(&ss);
    a6->sin6_family = AF_INET6;
    a6->sin6_port = htons(peer.port);
    a6->sin6_addr.s6_addr[10] = 0xff;
    a6->sin6_addr.s6_addr[11] = 0xff;
    std::memcpy(&a6->sin6_addr.s6_addr[12], peer.addr.data(), 4);
    len = sizeof(sockaddr_in6);
  } else {
    ToSockAddr(peer, ss, len);
  }
}

/** The error a failed send returns (errno / WSAGetLastError now): unreachable ones say so. */
Error SendError(const IpEndpoint& local, const IpEndpoint& peer, const char* what) {
#if defined(_WIN32)
  const int err = WSAGetLastError();
  const bool unreachable = err == WSAEHOSTUNREACH || err == WSAENETUNREACH || err == WSAEHOSTDOWN || err == WSAENETDOWN;
#else
  const int err = errno;
  const bool unreachable = err == EHOSTUNREACH || err == ENETUNREACH || err == EHOSTDOWN || err == ENETDOWN;
#endif
  return Error(unreachable ? kDatagramSendUnreachable : 0,
               std::string("adp udp: ") + what + " dst=" + EndpointLabel(peer) + " src=" + EndpointLabel(local) +
                   " errno=" + std::to_string(err));
}

} // namespace

Roe<void> OsUdpDatagramIo::SendTo(const IpEndpoint& peer, std::span<const uint8_t> datagram) {
  if (fd_ < 0) {
    return Error("adp udp: no socket (a reopen failed)");
  }
  sockaddr_storage ss{};
  socklen_t len = 0;
  PeerSockAddr(local_, peer, ss, len);
  const auto n = ::sendto(fd_, reinterpret_cast<const char*>(datagram.data()), static_cast<int>(datagram.size()), 0,
                          reinterpret_cast<sockaddr*>(&ss), len);
  if (n < 0 || static_cast<size_t>(n) != datagram.size()) {
    return SendError(local_, peer, "sendto");
  }
  return {};
}

Roe<void> OsUdpDatagramIo::SendSegments(const IpEndpoint& peer, const std::span<const std::span<const uint8_t>> parts,
                                        const size_t segment) {
#if defined(__linux__) && defined(UDP_SEGMENT)
  if (fd_ >= 0 && gso_ok_ && parts.size() > 1 && parts.size() <= kMaxSegments && segment > 0 && segment <= 0xffff) {
    sockaddr_storage ss{};
    socklen_t len = 0;
    PeerSockAddr(local_, peer, ss, len);
    std::array<iovec, kMaxSegments> iov{};
    size_t total = 0;
    for (size_t i = 0; i < parts.size(); ++i) {
      iov[i] = iovec{const_cast<uint8_t*>(parts[i].data()), parts[i].size()};
      total += parts[i].size();
    }
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(uint16_t))] = {};
    msghdr msg{};
    msg.msg_name = &ss;
    msg.msg_namelen = len;
    msg.msg_iov = iov.data();
    msg.msg_iovlen = parts.size();
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);
    cmsghdr* c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_UDP;
    c->cmsg_type = UDP_SEGMENT;
    c->cmsg_len = CMSG_LEN(sizeof(uint16_t));
    const auto gso_size = static_cast<uint16_t>(segment);
    std::memcpy(CMSG_DATA(c), &gso_size, sizeof(gso_size));
    const auto n = ::sendmsg(fd_, &msg, 0);
    if (n >= 0 && static_cast<size_t>(n) == total) {
      return {};
    }
    if (n < 0 && (errno == EIO || errno == EINVAL || errno == ENOPROTOOPT || errno == EOPNOTSUPP)) {
      gso_ok_ = false; // this kernel or device will not segment: one by one from now on
    } else {
      return SendError(local_, peer, "sendmsg(gso)");
    }
  }
#endif
  return DatagramIo::SendSegments(peer, parts, segment);
}

std::optional<DatagramIoStats> OsUdpDatagramIo::Stats() const {
  DatagramIoStats stats;
  stats.receive_buffer_bytes = GetBuffer(fd_, SO_RCVBUF);
  stats.send_buffer_bytes = GetBuffer(fd_, SO_SNDBUF);
  if (const int64_t drops = receive_drops_.load(std::memory_order_relaxed); drops >= 0) {
    stats.receive_drops = static_cast<uint64_t>(drops);
  }
  return stats;
}

Roe<std::optional<std::pair<IpEndpoint, size_t>>> OsUdpDatagramIo::RecvOneInto(const std::span<uint8_t> buf) {
  if (fd_ < 0) {
    return Error("adp udp: no socket (a reopen failed)");
  }
  for (int attempt = 0; attempt < kTransientRetries; ++attempt) {
    auto got = RecvOneAttempt(buf);
    if (got || got.error().code != kTransientReceive) {
      return got;
    }
  }
  return std::optional<std::pair<IpEndpoint, size_t>>{}; // nothing but transient errors: nothing read now
}

Roe<std::optional<std::pair<IpEndpoint, size_t>>> OsUdpDatagramIo::RecvOneAttempt(const std::span<uint8_t> buf) {
  sockaddr_storage ss{};
  socklen_t len = sizeof(ss);
#if defined(__linux__)
  iovec iov{buf.data(), buf.size()};
  alignas(cmsghdr) char control[CMSG_SPACE(sizeof(uint32_t))];
  msghdr msg{};
  msg.msg_name = &ss;
  msg.msg_namelen = sizeof(ss);
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = control;
  msg.msg_controllen = sizeof(control);
  const auto n = ::recvmsg(fd_, &msg, 0);
  if (n >= 0) {
    for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c != nullptr; c = CMSG_NXTHDR(&msg, c)) {
      if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SO_RXQ_OVFL) {
        uint32_t drops = 0;
        std::memcpy(&drops, CMSG_DATA(c), sizeof(drops));
        NoteReceiveDrops(drops);
      }
    }
    if (receive_drops_.load(std::memory_order_relaxed) < 0) {
      NoteReceiveDrops(0); // told nothing: none dropped yet
    }
  }
#else
  const auto n = ::recvfrom(fd_, reinterpret_cast<char*>(buf.data()), static_cast<int>(buf.size()), 0,
                            reinterpret_cast<sockaddr*>(&ss), &len);
#endif
  if (n < 0) {
#if defined(_WIN32)
    const int err = WSAGetLastError();
    if (err == WSAEWOULDBLOCK) {
      return std::optional<std::pair<IpEndpoint, size_t>>{};
    }
#else
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return std::optional<std::pair<IpEndpoint, size_t>>{};
    }
#endif
    if (TransientReceiveError()) {
      return Error(kTransientReceive, "adp udp: transient receive error");
    }
    return IoErr("recvfrom");
  }
  return std::optional<std::pair<IpEndpoint, size_t>>{{FromSockAddr(ss), static_cast<size_t>(n)}};
}

Roe<std::optional<std::pair<IpEndpoint, std::vector<uint8_t>>>> OsUdpDatagramIo::RecvFrom() {
  if (recv_scratch_.size() < kRecvBufferBytes) {
    recv_scratch_.resize(kRecvBufferBytes);
  }
  auto got = RecvOneInto(recv_scratch_);
  if (!got) {
    return got.error();
  }
  if (!*got) {
    return std::optional<std::pair<IpEndpoint, std::vector<uint8_t>>>{};
  }
  std::vector<uint8_t> buf(recv_scratch_.begin(), recv_scratch_.begin() + static_cast<std::ptrdiff_t>((*got)->second));
  return std::optional<std::pair<IpEndpoint, std::vector<uint8_t>>>{{(*got)->first, std::move(buf)}};
}

#if defined(__linux__)
Roe<size_t> OsUdpDatagramIo::RecvMany(const size_t max, const DatagramVisitor& visit) {
  if (!batch_) {
    batch_ = std::make_unique<RecvBatchBuffers>();
  }
  auto& b = *batch_;
  size_t total = 0;
  int transient = 0;
  while (total < max) {
    const auto want = static_cast<unsigned>(std::min(kRecvBatch, max - total));
    for (unsigned i = 0; i < want; ++i) {
      // The kernel writes the lengths back: set them again before every call.
      b.iovs[i] = iovec{b.data.data() + i * kRecvBufferBytes, kRecvBufferBytes};
      msghdr& m = b.msgs[i].msg_hdr;
      m = msghdr{};
      m.msg_name = &b.names[i];
      m.msg_namelen = sizeof(sockaddr_storage);
      m.msg_iov = &b.iovs[i];
      m.msg_iovlen = 1;
      m.msg_control = b.controls[i].bytes;
      m.msg_controllen = sizeof(b.controls[i].bytes);
      b.msgs[i].msg_len = 0;
    }
    const int n = ::recvmmsg(fd_, b.msgs.data(), want, 0, nullptr);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;
      }
      if (TransientReceiveError() && ++transient < kTransientRetries) {
        continue;
      }
      if (TransientReceiveError() || total > 0) {
        break; // transient: nothing more now; else reported by the next call
      }
      return IoErr("recvmmsg");
    }
    for (int i = 0; i < n; ++i) {
      msghdr& m = b.msgs[static_cast<size_t>(i)].msg_hdr;
      for (cmsghdr* c = CMSG_FIRSTHDR(&m); c != nullptr; c = CMSG_NXTHDR(&m, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SO_RXQ_OVFL) {
          uint32_t drops = 0;
          std::memcpy(&drops, CMSG_DATA(c), sizeof(drops));
          NoteReceiveDrops(drops);
        }
      }
    }
    if (n > 0 && receive_drops_.load(std::memory_order_relaxed) < 0) {
      NoteReceiveDrops(0); // told nothing: none dropped yet
    }
    for (int i = 0; i < n; ++i) {
      const auto at = static_cast<size_t>(i);
      visit(FromSockAddr(b.names[at]),
            std::span<const uint8_t>(b.data.data() + at * kRecvBufferBytes, b.msgs[at].msg_len));
    }
    total += static_cast<size_t>(n);
    if (static_cast<unsigned>(n) < want || fd_ < 0) {
      break; // drained (or a visitor reopened the socket)
    }
  }
  return total;
}
#endif

Roe<size_t> OsUdpDatagramIo::RecvBatch(const size_t max, const DatagramVisitor& visit) {
  if (fd_ < 0) {
    return Error("adp udp: no socket (a reopen failed)");
  }
  if (const SocketFault fault = socket_fault_.load(std::memory_order_relaxed); fault != SocketFault::None) {
    ready_reported_ = false;
    return Error(fault == SocketFault::Closed ? "adp udp: socket closed under us (poll: invalid)"
                                              : "adp udp: socket hung up (poll: shut down or defunct)");
  }
  return ReceiveReady(max, visit);
}

Roe<size_t> OsUdpDatagramIo::ReceiveReady(const size_t max, const DatagramVisitor& visit) {
  const bool was_ready = std::exchange(ready_reported_, false);
#if defined(__linux__)
  auto got = RecvMany(max, visit);
#else
  auto got = [&]() -> Roe<size_t> {
    if (recv_scratch_.size() < kRecvBufferBytes) {
      recv_scratch_.resize(kRecvBufferBytes);
    }
    size_t total = 0;
    while (total < max) {
      auto one = RecvOneInto(recv_scratch_);
      if (!one) {
        if (total > 0) {
          break; // reported by the next call
        }
        return one.error();
      }
      if (!*one) {
        break;
      }
      visit((*one)->first, std::span<const uint8_t>(recv_scratch_.data(), (*one)->second));
      ++total;
    }
    return total;
  }();
#endif
  if (got && *got > 0) {
    ready_misses_ = 0;
    poll_pause_ = std::chrono::milliseconds(0);
    return got;
  }
  if (!was_ready || ++ready_misses_ < kReadyMissLimit) {
    return got;
  }
  // "Readable" again and again with nothing to read: an error the read does not clear, a socket that will not read.
  // Polled, it ends every wait at once. Left out of the poll for a while (a reader still reads it on its own
  // schedule), and said once, so the owner can check the socket.
  ready_misses_ = 0;
  poll_pause_ = poll_pause_.count() == 0 ? kFirstPollPause : std::min(poll_pause_ * 2, kMaxPollPause);
  socket_paused_until_ = std::chrono::steady_clock::now() + poll_pause_;
  socket_poll_pauses_.fetch_add(1, std::memory_order_relaxed);
  if (!got) {
    return got;
  }
  return Error("adp udp: socket polls readable but reads nothing; out of the poll for " +
               std::to_string(poll_pause_.count()) + " ms");
}

} // namespace pp::adp
