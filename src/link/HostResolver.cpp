#include "amp/link/HostResolver.h"

#include <cstring>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace pp::amp {

namespace {

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

Roe<adp::IpEndpoint> ResolveHostNow(const std::string_view host, const HostFamily family, const uint16_t port) {
  EnsureWinsock();
  addrinfo hints{};
  hints.ai_family = family == HostFamily::V4 ? AF_INET : family == HostFamily::V6 ? AF_INET6 : AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  addrinfo *results = nullptr;
  const std::string name(host);
  if (const int rc = ::getaddrinfo(name.c_str(), nullptr, &hints, &results); rc != 0 || results == nullptr) {
    return Error("amp addr: cannot resolve " + name);
  }
  Roe<adp::IpEndpoint> out = Error("amp addr: " + name + " has no usable address");
  for (const addrinfo *ai = results; ai != nullptr; ai = ai->ai_next) {
    adp::IpEndpoint ep;
    ep.port = port;
    if (ai->ai_family == AF_INET && ai->ai_addrlen >= sizeof(sockaddr_in)) {
      sockaddr_in in4{};
      std::memcpy(&in4, ai->ai_addr, sizeof(in4));
      ep.family = adp::IpEndpoint::Family::V4;
      std::memcpy(ep.addr.data(), &in4.sin_addr, 4);
    } else if (ai->ai_family == AF_INET6 && ai->ai_addrlen >= sizeof(sockaddr_in6)) {
      sockaddr_in6 in6{};
      std::memcpy(&in6, ai->ai_addr, sizeof(in6));
      ep.family = adp::IpEndpoint::Family::V6;
      std::memcpy(ep.addr.data(), &in6.sin6_addr, 16);
      ep.scope_id = static_cast<uint32_t>(in6.sin6_scope_id);
    } else {
      continue;
    }
    out = ep;
    break;
  }
  ::freeaddrinfo(results);
  return out;
}

HostResolver::HostResolver(Post post, Lookup lookup)
    : sink_(std::make_shared<Sink>()), lookup_(lookup ? std::move(lookup) : Lookup(&ResolveHostNow)) {
  sink_->post = std::move(post);
}

HostResolver::~HostResolver() {
  std::lock_guard lock(sink_->mu);
  sink_->post = nullptr;
}

void HostResolver::Resolve(std::string host, const HostFamily family, const uint16_t port, Done done) {
  std::thread([sink = sink_, lookup = lookup_, host = std::move(host), family, port, done = std::move(done)]() mutable {
    auto result = lookup(host, family, port);
    std::lock_guard lock(sink->mu);
    if (sink->post) {
      sink->post([done = std::move(done), result = std::move(result)]() mutable { done(std::move(result)); });
    }
  }).detach();
}

} // namespace pp::amp
