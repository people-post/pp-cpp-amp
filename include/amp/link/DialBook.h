#pragma once

#include "amp/L1/Endpoint.h"
#include "amp/link/AdpMultiaddr.h"
#include "amp/link/CodedFailure.h"
#include "amp/link/LinkIdentity.h"
#include "amp/link/Types.h"

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pp::amp {

/** Endpoint directory + dial backoff / concurrent-dial caps. Strand-affine. */
class DialBook {
public:
  static constexpr size_t kMaxCandidatesPerKey = 16;

  struct EndpointRecord {
    std::string multiaddr; // active dial target (= candidates[dial_index] when non-empty)
    adp::IpEndpoint endpoint;
    std::string peer_id;
    /** Best-first ordered candidates (B15/B28). Empty until first Register. */
    std::vector<std::string> candidates;
    /** Index into `candidates` for the active / in-flight dial attempt. */
    size_t dial_index = 0;
    /** The active candidate is `/dns*`: `endpoint` holds its address once resolved (`resolved`). */
    std::string hostname;
    HostFamily host_family = HostFamily::Any;
    bool resolved = true;
  };

  explicit DialBook(PeerLinkConfig config) : config_(std::move(config)) {}

  const PeerLinkConfig& Config() const { return config_; }
  PeerLinkConfig& Config() { return config_; }

  Roe<void> RegisterEndpoint(const DialKey& peer_key, const std::string& multiaddr);
  /** Replace candidate list (best-first). Preferred = first parseable entry. */
  Roe<void> RegisterEndpoints(const DialKey& peer_key, const std::vector<std::string>& multiaddrs);
  /** Forget `peer_key`'s endpoints and backoff (see PeerLinkManager::UnregisterEndpoint). */
  void UnregisterEndpoint(const DialKey& peer_key);
  std::optional<std::string> PreferredMultiaddr(const std::string& peer_id) const;
  const EndpointRecord* Find(const DialKey& peer_key) const;
  EndpointRecord* Find(const DialKey& peer_key);
  bool Contains(const DialKey& peer_key) const { return endpoints_.contains(peer_key); }
  std::unordered_map<DialKey, EndpointRecord>& Endpoints() { return endpoints_; }
  const std::unordered_map<DialKey, EndpointRecord>& Endpoints() const { return endpoints_; }

  /** Advance to the next candidate after a failed dial. Returns false if exhausted. */
  bool AdvanceDialCandidate(const DialKey& peer_key);
  /** Reset dial_index to 0 and sync multiaddr/endpoint (after success or full miss). */
  void ResetDialIndex(const DialKey& peer_key);
  /** Promote the winning multiaddr to front after a successful dial. */
  void PromoteDialWinner(const DialKey& peer_key, const std::string& multiaddr);

  void ClearBackoff(const DialKey& peer_key) { dial_failed_until_.erase(peer_key); }
  void ClearAllBackoff() { dial_failed_until_.clear(); }
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

  /** The active candidate of `peer_key` is a hostname with no address resolved within the TTL. */
  bool NeedsResolution(const DialKey& peer_key) const;
  /** `multiaddr` (a hostname candidate) resolved to `endpoint`: every record dialing it takes it. */
  void SetResolution(const std::string& multiaddr, const adp::IpEndpoint& endpoint);
  /** A dial of `peer_key`'s active hostname candidate failed: its address may be stale (the host
   *  moved), so the next dial resolves it again. */
  void ForgetResolution(const DialKey& peer_key);

private:
  bool ApplyParsed(EndpointRecord& rec, const std::string& multiaddr, const DialKey& peer_key);
  void SyncActiveFromIndex(EndpointRecord& rec);
  /** Point `rec` at `parsed` (its active candidate): the address, or a hostname's cached one. */
  void ApplyAddress(EndpointRecord& rec, const std::string& multiaddr, const ParsedAdpMultiaddr& parsed);

  struct Resolution {
    adp::IpEndpoint endpoint;
    std::chrono::steady_clock::time_point at;
  };
  /** Hostname candidates resolved, by multiaddr: dialed until the TTL passes. */
  std::unordered_map<std::string, Resolution> resolutions_;

  PeerLinkConfig config_;
  std::unordered_map<DialKey, EndpointRecord> endpoints_;
  std::unordered_map<DialKey, std::chrono::steady_clock::time_point> dial_failed_until_;
  size_t concurrent_dials_ = 0;
};

} // namespace pp::amp
