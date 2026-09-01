#pragma once

#include "amp/L1/Types.h"


#include <string>

namespace pp::amp {

struct ParsedAdpMultiaddr {
  adp::IpEndpoint endpoint;
  std::string peer_id;
};

/** Parse `/ip4/<host>/udp/<port>/adp/1.0.0/p2p/<PeerId>`. */
Roe<ParsedAdpMultiaddr> ParseAdpMultiaddr(std::string_view multiaddr);

/** Format an ADP listen/dial multiaddr. */
Roe<std::string> FormatAdpMultiaddr(const adp::IpEndpoint& endpoint, std::string_view peer_id);

} // namespace pp::amp
