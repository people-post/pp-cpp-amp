#include "amp/L1/OsUdpDatagramIo.h"

#include <cerrno>
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

Roe<std::unique_ptr<OsUdpDatagramIo>> OsUdpDatagramIo::Bind(const IpEndpoint& local) {
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
    return Error(std::string("adp udp: sendto dst=") + EndpointLabel(peer) + " src=" +
                 EndpointLabel(local_) +
#if defined(_WIN32)
                 " errno=" + std::to_string(WSAGetLastError())
#else
                 " errno=" + std::to_string(errno)
#endif
    );
  }
  return {};
}

Roe<std::optional<std::pair<IpEndpoint, std::vector<uint8_t>>>> OsUdpDatagramIo::RecvFrom() {
  if (recv_scratch_.size() < 2048) {
    recv_scratch_.resize(2048);
  }
  sockaddr_storage ss{};
  socklen_t len = sizeof(ss);
  const auto n =
      ::recvfrom(fd_, reinterpret_cast<char*>(recv_scratch_.data()), static_cast<int>(recv_scratch_.size()), 0,
                 reinterpret_cast<sockaddr*>(&ss), &len);
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
