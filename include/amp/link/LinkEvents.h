#pragma once

#include "amp/L1/Types.h"
#include "amp/link/LinkIdentity.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace pp::amp {

/** Why PeerLinkManager removed a PeerLink (DropLink). */
enum class LinkDropReason : uint8_t {
  Unknown = 0,
  /** ADP Connection closed (Close received or local close). */
  ConnectionClosed,
  /** Cold ADP link with no authenticated RX within kAliveTimeoutMs. */
  ConnectionDead,
  /** Nested link's carrier channel closed / orphaned. */
  CarrierClosed,
  DualDialLost,
  HandshakeFailed,
  HandshakeTimeout,
  TransportFailed,
  DialAborted,
  /** Non-Connected occupant displaced by a fresh dial or alias adoption. */
  Displaced,
  /** Product asked to drop it (RequestDropLink — e.g. stale link after the peer changed network). */
  Requested,
  /** Local network changed and the link did not answer a probe within the grace (OnNetworkChanged). */
  NetworkChanged,
  /** This side is shutting down (CloseAllLinks / MeshRuntime::Stop); each peer was told. */
  LocalShutdown,
  /** The link table was full: this idle inbound link made room for a new peer (its peer was told). */
  IdleEvicted,
  /** Asked to prove it is alive (VerifyLink), the link did not answer a probe within the grace. */
  Unresponsive,
  /** The dialed peer speaks another wire version (its version negotiation reply, N032). */
  VersionMismatch,
};

inline const char* LinkDropReasonName(const LinkDropReason reason) {
  switch (reason) {
    case LinkDropReason::IdleEvicted:
      return "idle-evicted";
    case LinkDropReason::Unresponsive:
      return "unresponsive";
    case LinkDropReason::VersionMismatch:
      return "version-mismatch";
    case LinkDropReason::Unknown:
      return "unknown";
    case LinkDropReason::ConnectionClosed:
      return "connection-closed";
    case LinkDropReason::ConnectionDead:
      return "connection-dead";
    case LinkDropReason::CarrierClosed:
      return "carrier-closed";
    case LinkDropReason::DualDialLost:
      return "dual-dial-lost";
    case LinkDropReason::HandshakeFailed:
      return "handshake-failed";
    case LinkDropReason::HandshakeTimeout:
      return "handshake-timeout";
    case LinkDropReason::TransportFailed:
      return "transport-failed";
    case LinkDropReason::DialAborted:
      return "dial-aborted";
    case LinkDropReason::Displaced:
      return "displaced";
    case LinkDropReason::Requested:
      return "requested";
    case LinkDropReason::NetworkChanged:
      return "network-changed";
    case LinkDropReason::LocalShutdown:
      return "local-shutdown";
  }
  return "unknown";
}

/**
 * Which side of a version mismatch is older, so the product can say who must update (release compatibility U5).
 */
enum class VersionSkew : uint8_t {
  /** Not known: the peer named no versions, or some above ours and some below. */
  Unknown = 0,
  /** Every version the peer speaks is below ours: the peer must update. */
  PeerOlder = 1,
  /** Every version the peer speaks is above ours: this side must update. */
  PeerNewer = 2,
};

inline const char* VersionSkewName(const VersionSkew skew) {
  switch (skew) {
    case VersionSkew::PeerOlder:
      return "peer-older";
    case VersionSkew::PeerNewer:
      return "peer-newer";
    case VersionSkew::Unknown:
      return "unknown";
  }
  return "unknown";
}

/** PeerNewer when every one of `theirs` is above `ours`, PeerOlder when every one is below, else Unknown. */
inline VersionSkew CompareVersions(const uint8_t ours, const std::vector<uint8_t>& theirs) {
  if (theirs.empty()) {
    return VersionSkew::Unknown;
  }
  bool all_above = true;
  bool all_below = true;
  for (const uint8_t v : theirs) {
    all_above = all_above && v > ours;
    all_below = all_below && v < ours;
  }
  return all_above ? VersionSkew::PeerNewer : all_below ? VersionSkew::PeerOlder : VersionSkew::Unknown;
}

/** The version a mismatch was found in. */
enum class VersionLayer : uint8_t {
  /** The L1 wire version (the packet's first nibble; the peer's version negotiation reply). */
  Wire = 0,
  /** The MSH handshake version (the hello's version byte; the responder's Close names its versions). */
  Msh = 1,
};

inline const char* VersionLayerName(const VersionLayer layer) {
  return layer == VersionLayer::Msh ? "msh" : "wire";
}

/** A version mismatch, as a link found it (LinkEvent::version_mismatch, PeerLink::LastVersionMismatch). */
struct VersionMismatchInfo {
  VersionLayer layer = VersionLayer::Wire;
  /** The version this side speaks. */
  uint8_t ours = 0;
  /** The versions the peer said it speaks (from its version negotiation, hello or Close). */
  std::vector<uint8_t> theirs;
  /** CompareVersions(ours, theirs): who must update. */
  VersionSkew skew = VersionSkew::Unknown;
};

/**
 * Link lifecycle event (ADR_LINK_PLANE: no PeerLink&). Delivered via the completion poster
 * (MeshRuntime::PostToIo) — never on the link's own callback stack.
 */
struct LinkEvent {
  enum class Kind : uint8_t {
    /** Link reached Connected and won dual-dial adoption. */
    Connected,
    /** Link removed from the table; `reason` set. */
    Dropped,
    /** ADP remote endpoint migrated (authenticated packet from a new address). */
    PathChanged,
    /**
     * A dial ended without a link after every dial-book candidate was tried (not a dial this side
     * aborted). `dial_key`, `peer_id` (the dial book's, when known) and `reason` are set — what the
     * peer's reachability can be judged by; a per-candidate miss is only a `Dropped` attempt.
     */
    DialFailed,
  };

  Kind kind = Kind::Connected;
  LinkHandle handle;
  DialKey dial_key;
  /** Authenticated remote PeerId; empty if the link never finished MSH. */
  std::string peer_id;
  TransportClass transport = TransportClass::Adp;
  LinkPathKind path_kind = LinkPathKind::Direct;
  bool outbound = false;
  /** ADP remote endpoint (new endpoint for PathChanged). Empty for carrier links. */
  std::optional<adp::IpEndpoint> remote;
  /** PathChanged only: endpoint before the migration. */
  std::optional<adp::IpEndpoint> previous_remote;
  /** Dropped and DialFailed. */
  LinkDropReason reason = LinkDropReason::Unknown;
  /** Dropped only: link had reached Connected before (false = failed / aborted attempt). */
  bool was_connected = false;
  /** Dropped only, ADP: ms since last authenticated RX; -1 if none / carrier. */
  int64_t last_rx_age_ms = -1;
  /**
   * Dropped only, ADP: the keepalive cadence the link ran at (the larger of ours and the peer's),
   * 0 if neither side kept it alive. A `ConnectionDead` drop at 0 is an idle link timing out — not
   * evidence the peer is gone; above 0 its echo requests went unanswered.
   */
  uint32_t keepalive_interval_ms = 0;
  /**
   * Dropped and DialFailed with reason VersionMismatch: which version, ours and the peer's, and who must update
   * (U5). Unset otherwise.
   */
  std::optional<VersionMismatchInfo> version_mismatch;
  /**
   * Dropped and DialFailed, ADP: why the peer closed the association, from its Close (U4). Unspecified when it sent
   * none, gave no reason (a v6.0.0 peer), or this side closed it.
   */
  adp::CloseReason peer_close_reason = adp::CloseReason::Unspecified;
};

inline const char* LinkEventKindName(const LinkEvent::Kind kind) {
  switch (kind) {
    case LinkEvent::Kind::Connected:
      return "connected";
    case LinkEvent::Kind::Dropped:
      return "dropped";
    case LinkEvent::Kind::PathChanged:
      return "path-changed";
    case LinkEvent::Kind::DialFailed:
      return "dial-failed";
  }
  return "unknown";
}

using LinkEventListener = std::function<void(const LinkEvent& event)>;
using LinkEventListenerId = uint64_t;

} // namespace pp::amp
