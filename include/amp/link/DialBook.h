#pragma once

#include "amp/L1/Endpoint.h"
#include "amp/link/AdpMultiaddr.h"
#include "amp/link/CodedFailure.h"
#include "amp/link/LinkIdentity.h"
#include "amp/link/Types.h"

#include <chrono>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pp::amp {

/** Endpoint directory + dial backoff / concurrent-dial caps. Strand-affine. */
class DialBook {
public:
  struct EndpointRecord {
    std::string multiaddr;
    adp::IpEndpoint endpoint;
    std::string peer_id;
  };

  explicit DialBook(PeerLinkConfig config) : config_(std::move(config)) {}

  const PeerLinkConfig& Config() const { return config_; }
  PeerLinkConfig& Config() { return config_; }

  Roe<void> RegisterEndpoint(const DialKey& peer_key, const std::string& multiaddr);
  std::optional<std::string> PreferredMultiaddr(const std::string& peer_id) const;
  const EndpointRecord* Find(const DialKey& peer_key) const;
  EndpointRecord* Find(const DialKey& peer_key);
  bool Contains(const DialKey& peer_key) const { return endpoints_.contains(peer_key); }
  std::unordered_map<DialKey, EndpointRecord>& Endpoints() { return endpoints_; }
  const std::unordered_map<DialKey, EndpointRecord>& Endpoints() const { return endpoints_; }

  void ClearBackoff(const DialKey& peer_key) { dial_failed_until_.erase(peer_key); }
  void ArmBackoff(const DialKey& peer_key);
  bool InBackoff(const DialKey& peer_key, std::chrono::steady_clock::time_point now) const;
  std::optional<std::chrono::milliseconds> BackoffRemaining(const DialKey& peer_key,
                                                            std::chrono::steady_clock::time_point now) const;
  void EraseExpiredBackoff(const DialKey& peer_key, std::chrono::steady_clock::time_point now);

  size_t ConcurrentDials() const { return concurrent_dials_; }
  void IncConcurrentDials() { ++concurrent_dials_; }
  void DecConcurrentDials() {
    if (concurrent_dials_ > 0) {
      --concurrent_dials_;
    }
  }

  void IngestRemoteAddrs(const std::string& peer_id, const std::vector<std::string>& addrs);

private:
  PeerLinkConfig config_;
  std::unordered_map<DialKey, EndpointRecord> endpoints_;
  std::unordered_map<DialKey, std::chrono::steady_clock::time_point> dial_failed_until_;
  size_t concurrent_dials_ = 0;
};

} // namespace pp::amp
