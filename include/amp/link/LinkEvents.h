#pragma once

#include "amp/L1/Types.h"
#include "amp/link/LinkIdentity.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

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
};

inline const char* LinkDropReasonName(const LinkDropReason reason) {
  switch (reason) {
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
  }
  return "unknown";
}

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
  };

  Kind kind = Kind::Connected;
  LinkHandle handle;
  DialKey dial_key;
  /** Authenticated remote PeerId; empty if the link never finished MSH. */
  std::string peer_id;
  TransportClass transport = TransportClass::Adp;
  bool outbound = false;
  /** ADP remote endpoint (new endpoint for PathChanged). Empty for carrier links. */
  std::optional<adp::IpEndpoint> remote;
  /** PathChanged only: endpoint before the migration. */
  std::optional<adp::IpEndpoint> previous_remote;
  /** Dropped only. */
  LinkDropReason reason = LinkDropReason::Unknown;
  /** Dropped only: link had reached Connected before (false = failed / aborted attempt). */
  bool was_connected = false;
  /** Dropped only, ADP: ms since last authenticated RX; -1 if none / carrier. */
  int64_t last_rx_age_ms = -1;
};

inline const char* LinkEventKindName(const LinkEvent::Kind kind) {
  switch (kind) {
  case LinkEvent::Kind::Connected:
    return "connected";
  case LinkEvent::Kind::Dropped:
    return "dropped";
  case LinkEvent::Kind::PathChanged:
    return "path-changed";
  }
  return "unknown";
}

using LinkEventListener = std::function<void(const LinkEvent& event)>;
using LinkEventListenerId = uint64_t;

} // namespace pp::amp
