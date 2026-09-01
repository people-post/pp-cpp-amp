#pragma once

#include "amp/L1/Types.h"
#include "amp/L2/Types.h"


#include <sodium.h>

#include <chrono>
#include <cstddef>
#include <functional>
#include <string>

namespace pp::amp {

inline constexpr const char* kAdpMultiaddrProtocol = "adp";
inline constexpr const char* kAdpMultiaddrVersion = "1.0.0";

/** Outer circuit target for nested A↔B Session ([A024]); not an L4 product protocol. */
inline constexpr const char* kAmpCircuitCarrierProtocolId = "/pp-browser/amp-circuit-carrier/1.0.0";

/** Pre-MSH ADP HMAC key (documented constant; upgraded to K_assoc after handshake). */
adp::PeerKey PreSessionPeerKey();

/** Reserved assoc id for MSH handshake before K_assoc is derived. */
adp::AssocId PreSessionAssocId();

/** Stable hex fingerprint for an ML-DSA identity public key (link indexing). */
std::string IdentityPublicKeyFingerprint(const ByteVector& identity_public_key);

enum class PeerLinkPhase {
  Unavailable,
  Idle,
  Dialing,
  Handshaking,
  Connected,
  Backoff,
};

struct PeerLinkSnapshot {
  PeerLinkPhase phase = PeerLinkPhase::Unavailable;
  std::chrono::milliseconds backoff_remaining{0};
  std::string detail;
  bool has_endpoint = false;
  std::string multiaddr;
};

struct PeerLinkConfig {
  size_t max_links = 48;
  size_t max_concurrent_dials = 6;
  std::chrono::milliseconds dial_timeout{8000};
  std::chrono::milliseconds idle_ttl{180000};
  std::chrono::milliseconds dial_failure_backoff{30000};
  /** When set, derives remote PeerId string from authenticated MSH identity key. */
  std::function<std::string(const ByteVector& identity_public_key)> peer_id_from_identity;
};

} // namespace pp::amp
