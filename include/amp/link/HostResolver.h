#pragma once

#include "amp/L1/Types.h"
#include "amp/link/AdpMultiaddr.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace pp::amp {

/**
 * The first address `host` resolves to for `family` (getaddrinfo, blocking), with `port`. For a
 * `/dns|dns4|dns6` multiaddr candidate; never call it on the io strand.
 */
Roe<adp::IpEndpoint> ResolveHostNow(std::string_view host, HostFamily family, uint16_t port);

/**
 * Resolves hostnames off the io strand: each lookup runs on a short-lived thread of its own and
 * hands its result to `post` (MeshRuntime::PostToIo), so `done` runs on the io lane. Once the
 * resolver is destroyed, lookups still running deliver nothing (no thread is joined: a lookup can
 * take seconds, and a stopping node must not wait for it).
 */
class HostResolver {
public:
  using Done = std::function<void(Roe<adp::IpEndpoint>)>;
  using Post = std::function<void(std::function<void()>)>;
  /** How a lookup is done; ResolveHostNow by default (tests replace it). */
  using Lookup = std::function<Roe<adp::IpEndpoint>(const std::string &host, HostFamily family, uint16_t port)>;

  explicit HostResolver(Post post, Lookup lookup = {});
  ~HostResolver();
  HostResolver(const HostResolver &) = delete;
  HostResolver &operator=(const HostResolver &) = delete;

  void Resolve(std::string host, HostFamily family, uint16_t port, Done done);

private:
  struct Sink {
    std::mutex mu;
    Post post; ///< cleared when the resolver goes
  };
  std::shared_ptr<Sink> sink_;
  Lookup lookup_;
};

} // namespace pp::amp
