#include "amp/L1/OsUdpDatagramIo.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

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
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
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

} // namespace

OsUdpDatagramIo::OsUdpDatagramIo(int fd, IpEndpoint local) : fd_(fd), local_(local) {}

OsUdpDatagramIo::~OsUdpDatagramIo() {
  if (fd_ >= 0) {
#if defined(_WIN32)
    closesocket(fd_);
#else
    ::close(fd_);
#endif
    fd_ = -1;
  }
}

/** Ask for `bytes` of `option` (SO_RCVBUF / SO_SNDBUF); on Linux, force it past the sysctl cap when allowed. */
static void SetBuffer(int fd, int option, [[maybe_unused]] int force_option, size_t bytes) {
  if (bytes == 0) {
    return;
  }
  const int value = static_cast<int>(std::min<size_t>(bytes, static_cast<size_t>(INT32_MAX / 2)));
#if defined(__linux__)
  if (setsockopt(fd, SOL_SOCKET, force_option, &value, sizeof(value)) == 0) {
    return; // CAP_NET_ADMIN: past net.core.rmem_max / wmem_max
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

Roe<std::unique_ptr<OsUdpDatagramIo>> OsUdpDatagramIo::Bind(const IpEndpoint& local, const UdpSocketOptions& options) {
  EnsureWinsock();
  const int family = local.family == IpEndpoint::Family::V4 ? AF_INET : AF_INET6;
  const int fd = static_cast<int>(::socket(family, SOCK_DGRAM, IPPROTO_UDP));
  if (fd < 0) {
    return IoErr("socket");
  }
#if defined(_WIN32)
  u_long mode = 1;
  ioctlsocket(fd, FIONBIO, &mode);
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
#if defined(_WIN32)
    closesocket(fd);
#else
    ::close(fd);
#endif
    return IoErr("bind");
  }
  sockaddr_storage bound{};
  socklen_t blen = sizeof(bound);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &blen) != 0) {
#if defined(_WIN32)
    closesocket(fd);
#else
    ::close(fd);
#endif
    return IoErr("getsockname");
  }
  IpEndpoint actual = FromSockAddr(bound);
  return std::unique_ptr<OsUdpDatagramIo>(new OsUdpDatagramIo(fd, actual));
}

Roe<void> OsUdpDatagramIo::SendTo(const IpEndpoint& peer, std::span<const uint8_t> datagram) {
  sockaddr_storage ss{};
  socklen_t len = 0;
  if (local_.family == IpEndpoint::Family::V6 && peer.family == IpEndpoint::Family::V4) {
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
  const auto n = ::sendto(fd_, reinterpret_cast<const char*>(datagram.data()),
                          static_cast<int>(datagram.size()), 0, reinterpret_cast<sockaddr*>(&ss),
                          len);
  if (n < 0 || static_cast<size_t>(n) != datagram.size()) {
#if defined(_WIN32)
    const int err = WSAGetLastError();
    const bool unreachable = err == WSAEHOSTUNREACH || err == WSAENETUNREACH || err == WSAEHOSTDOWN ||
                             err == WSAENETDOWN;
#else
    const int err = errno;
    const bool unreachable = err == EHOSTUNREACH || err == ENETUNREACH || err == EHOSTDOWN || err == ENETDOWN;
#endif
    return Error(unreachable ? kDatagramSendUnreachable : 0,
                 std::string("adp udp: sendto dst=") + EndpointLabel(peer) + " src=" + EndpointLabel(local_) +
                     " errno=" + std::to_string(err));
  }
  return {};
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

Roe<std::optional<std::pair<IpEndpoint, std::vector<uint8_t>>>> OsUdpDatagramIo::RecvFrom() {
  if (recv_scratch_.size() < 2048) {
    recv_scratch_.resize(2048);
  }
  sockaddr_storage ss{};
  socklen_t len = sizeof(ss);
#if defined(__linux__)
  iovec iov{recv_scratch_.data(), recv_scratch_.size()};
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
        receive_drops_.store(drops, std::memory_order_relaxed);
      }
    }
    if (receive_drops_.load(std::memory_order_relaxed) < 0) {
      receive_drops_.store(0, std::memory_order_relaxed); // told nothing: none dropped yet
    }
  }
#else
  const auto n =
      ::recvfrom(fd_, reinterpret_cast<char*>(recv_scratch_.data()), static_cast<int>(recv_scratch_.size()), 0,
                 reinterpret_cast<sockaddr*>(&ss), &len);
#endif
  if (n < 0) {
#if defined(_WIN32)
    const int err = WSAGetLastError();
    if (err == WSAEWOULDBLOCK) {
      return std::optional<std::pair<IpEndpoint, std::vector<uint8_t>>>{};
    }
#else
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return std::optional<std::pair<IpEndpoint, std::vector<uint8_t>>>{};
    }
#endif
    return IoErr("recvfrom");
  }
  std::vector<uint8_t> buf(recv_scratch_.begin(), recv_scratch_.begin() + n);
  return std::optional<std::pair<IpEndpoint, std::vector<uint8_t>>>{
      {FromSockAddr(ss), std::move(buf)}};
}

} // namespace pp::adp
