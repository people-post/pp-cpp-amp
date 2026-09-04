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

Error IoErr(const char* what) { return Error(std::string("adp udp: ") + what); }

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
  IpEndpoint e;
  e.family = IpEndpoint::Family::V6;
  const auto* a = reinterpret_cast<const sockaddr_in6*>(&ss);
  std::memcpy(e.addr.data(), &a->sin6_addr, 16);
  e.port = ntohs(a->sin6_port);
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
  ToSockAddr(peer, ss, len);
  const auto n = ::sendto(fd_, reinterpret_cast<const char*>(datagram.data()),
                          static_cast<int>(datagram.size()), 0, reinterpret_cast<sockaddr*>(&ss),
                          len);
  if (n < 0 || static_cast<size_t>(n) != datagram.size()) {
    return IoErr("sendto");
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
