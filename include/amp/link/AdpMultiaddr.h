#pragma once

#include "amp/L1/Types.h"


#include <string>

namespace pp::amp {

/** Which addresses a hostname may resolve to: `/dns4` (IPv4), `/dns6` (IPv6), `/dns` (either). */
enum class HostFamily : uint8_t { Any, V4, V6 };

struct ParsedAdpMultiaddr {
  /** The address; for a hostname, only `port` is set until it is resolved (HostResolver). */
  adp::IpEndpoint endpoint;
  std::string peer_id;
  /** `/dns|dns4|dns6/<hostname>`: resolved when dialed, not when parsed. Empty for an IP. */
  std::string hostname;
  HostFamily host_family = HostFamily::Any;

  bool IsHostname() const { return !hostname.empty(); }
};

/**
 * Parse `/ip4|ip6|dns|dns4|dns6/<host>/udp/<port>/adp/1.0.0/p2p/<PeerId>`. A hostname is checked as
 * a DNS name (labels of letters, digits and hyphens, at most 253 characters), not resolved.
 */
Roe<ParsedAdpMultiaddr> ParseAdpMultiaddr(std::string_view multiaddr);

/** Format an ADP listen/dial multiaddr (IPv4 or IPv6). */
Roe<std::string> FormatAdpMultiaddr(const adp::IpEndpoint& endpoint, std::string_view peer_id);

} // namespace pp::amp
