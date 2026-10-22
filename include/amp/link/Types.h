#pragma once

#include "amp/L3/ChannelMux.h"

#include "amp/L1/Types.h"
#include "amp/L2/Types.h"


#include <sodium.h>

#include <chrono>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

namespace pp::amp {

/** DialKey prefix of MeshRuntime::BurstDial's per-candidate ephemeral dials. */
inline constexpr std::string_view kBurstDialKeyPrefix = "amp:burst:";

/** Ephemeral dial keys never become a link's durable alias (inbound adopt, alias moves). */
inline bool IsEphemeralDialKey(std::string_view key) {
  return key.substr(0, kBurstDialKeyPrefix.size()) == kBurstDialKeyPrefix;
}

inline constexpr const char* kAdpMultiaddrProtocol = "adp";
inline constexpr const char* kAdpMultiaddrVersion = "1.0.0";

/**
 * Amp-owned outer circuit carrier for nested A↔B Session ([A024]).
 * Products should use this default via EnableNestedCarrierAccept(true) — do not mint
 * product-namespace aliases.
 */
inline constexpr const char* kAmpCircuitCarrierProtocolId = "/pp-amp/circuit-carrier/1.0.0";

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
  /** True when the connected PeerLink is nested over a circuit/media carrier ([A024]). */
  bool carrier_backed = false;
  std::string multiaddr;
};

struct PeerLinkConfig {
  size_t max_links = 48;
  /**
   * Cap on inbound links still Handshaking, separate from and smaller than max_links: a flood of
   * cheap forged-source inbound attempts must not fill the whole table and starve outbound dials
   * or already-Connected links of a slot.
   */
  size_t max_pending_inbound = 16;
  size_t max_concurrent_dials = 6;
  std::chrono::milliseconds dial_timeout{8000};
  /** Per-candidate handshake budget when DialBook has further fallbacks (B15/B28). */
  std::chrono::milliseconds dial_attempt_timeout{800};
  /**
   * A key's dial failed: no new dial before the backoff. It starts at dial_failure_backoff_initial
   * and doubles with each failure in a row up to dial_failure_backoff, with ±20 % jitter so a herd
   * refused together does not come back together; cleared by a fresh address or a success.
   */
  std::chrono::milliseconds dial_failure_backoff{30000};
  std::chrono::milliseconds dial_failure_backoff_initial{1000};
  /** How long a hostname candidate's resolved address is dialed before it is resolved again. */
  std::chrono::milliseconds host_resolution_ttl{60000};
  /** Outbound keepalive interval for MarkHot links (NAT refresh). */
  std::chrono::milliseconds keepalive_hot_interval{20000};
  /** Outbound keepalive interval for MarkWarm links. */
  std::chrono::milliseconds keepalive_warm_interval{60000};
  /**
   * After OnNetworkChanged: a Connected ADP link with no authenticated RX within this long is
   * evicted (reason network-changed); it is re-probed every quarter of it meanwhile.
   */
  std::chrono::milliseconds network_change_grace{2000};
  /** When set, derives remote PeerId string from authenticated MSH identity key. */
  std::function<std::string(const ByteVector& identity_public_key)> peer_id_from_identity;

  /** ADP transport tuning for every link's connection (docs/TUNING.md). */
  adp::AdpTuning adp;
  /** Channel-mux tuning for every link (docs/TUNING.md). */
  MuxTuning mux;
};

/**
 * Check a config's tunables for values that cannot work together (e.g. a replay
 * window smaller than the send window stalls delivery after a loss). AmpStack::Create
 * refuses an invalid config; call it directly when building PeerLinkManager by hand.
 */
Roe<void> ValidatePeerLinkConfig(const PeerLinkConfig& config);

/** Scheduled keepalive tier on a connected link (see docs/KEEPALIVE.md). */
enum class KeepaliveTier {
  None = 0,
  Warm = 1,
  Hot = 2,
};

} // namespace pp::amp
