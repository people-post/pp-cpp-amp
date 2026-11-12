#pragma once

#include "amp/L1/Connection.h"
#include "amp/L1/ReplayWindow.h"
#include "amp/L3/Capability.h"
#include "amp/L3/ChannelMux.h"
#include "amp/L3/ChannelSession.h"
#include "amp/link/CarrierLane.h"
#include "amp/link/CodedFailure.h"
#include "amp/link/LinkIdentity.h"
#include "amp/link/MshAdpHandshake.h"
#include "amp/link/Types.h"
#include "amp/L2/Session.h"
#include "amp/L2/Types.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace pp::amp {

class PeerLink;

/**
 * Host ports PeerLink needs — owned by the consumer (PeerLink), installed by the
 * composition root (PeerLinkManager). Speaks link needs, not manager types.
 * Same idea as pp-browser COMPOSITION_VOCABULARY: lower peer must not `#include` higher.
 */
struct PeerLinkHostPorts {
  /** Wall/clock for handshake start and mux timers. */
  std::function<int64_t()> now_ms;
  /** Derive authenticated PeerId from MSH identity public key (empty → fingerprint fallback). */
  std::function<std::string(const ByteVector& identity_public_key)> derive_peer_id;
  /**
   * After mux attach: return false if this link lost dual-dial election and must demote.
   * Host schedules drop; PeerLink does not call DropLink itself.
   */
  std::function<bool(PeerLink& link)> on_established;
  /** Request parent erase of this link (by handle — its key may be reused) after the stack unwinds (A027). */
  std::function<void(LinkHandle link)> schedule_drop;
  /** Request parent erase of a Connected link whose connection closed (deferred like schedule_drop). */
  std::function<void(LinkHandle link)> schedule_drop_closed;
  /** Dual-dial: adopt dial alias onto surviving Connected link. */
  std::function<void(std::string remote_peer_id, std::string dial_alias)> schedule_adopt_alias;
  /** True if another Connected Session already exists for remote PeerId. */
  std::function<bool(const std::string& remote_peer_id)> has_other_connected;
};

/** One ADP association + AMP session + channel mux to a remote peer. Io-thread affine.
 *  Carrier-backed links ([A024]) use a bridged ChannelSession instead of ADP Connection. */
class PeerLink {
public:
  enum class Err : int32_t {
    Ok = 0,
    NoConnection,
    TransportFailed,
    TransportUnavailable,
    HandshakeFailed,
    DialTimeout,
    CarrierClosed,
    CarrierEnqueueFailed,
    DualDialLost,
    NotConnected,
    RekeyInFlight,
    Ch0NotOpen,
    /** The peer answered with version negotiation: it speaks another wire version (N032). */
    VersionMismatch,
    Generic,
  };

  using Failure = CodedFailure<Err>;
  using LinkRoe = CodedRoe<void, Err>;
  using CompleteCb = std::function<void(LinkRoe)>;

  PeerLink(std::string peer_key, std::string remote_peer_id, bool outbound, std::shared_ptr<adp::Connection> connection,
           MshIdentity local_identity, PeerLinkHostPorts host);

  /** Nested Session over circuit carrier (no ADP Connection). */
  PeerLink(std::string peer_key, std::string remote_peer_id, bool outbound, std::shared_ptr<ChannelSession> carrier,
           MshIdentity local_identity, PeerLinkHostPorts host);

  ~PeerLink();

  PeerLink(const PeerLink&) = delete;
  PeerLink& operator=(const PeerLink&) = delete;

  void StartOutboundHandshake(CompleteCb on_established);
  void StartInboundHandshake(CompleteCb on_established);

  void HandleAdpPayload(std::span<const uint8_t> payload);

  PeerLinkPhase Phase() const { return phase_; }
  bool IsOutbound() const { return outbound_; }
  bool IsCarrierBacked() const { return carrier_ != nullptr; }
  TransportClass Transport() const { return IsCarrierBacked() ? TransportClass::Carrier : TransportClass::Adp; }
  LinkId Id() const { return link_id_; }
  uint32_t Generation() const { return generation_; }
  LinkHandle Handle() const { return LinkHandle{link_id_, generation_}; }
  const std::string& PeerKey() const { return peer_key_; }
  const std::string& RemotePeerId() const { return remote_peer_id_; }
  /** Came up through a punch (BurstDial) — set by the manager at establish. */
  bool Punched() const { return punched_; }
  void MarkPunched() { punched_ = true; }
  const ByteVector& RemoteIdentityPublicKey() const { return remote_identity_public_key_; }
  adp::Connection* ConnectionOrNull() { return connection_.get(); }
  const adp::Connection* ConnectionOrNull() const { return connection_.get(); }
  ChannelMux* Mux() { return mux_.get(); }
  const ChannelMux* Mux() const { return mux_.get(); }
  Session* GetSession() { return session_.get(); }
  ChannelSession* Carrier() { return carrier_.get(); }

  const CapabilityPayload* RemoteCapability() const { return remote_capability_ ? &*remote_capability_ : nullptr; }

  void MarkWarm();
  void MarkHot();
  void ClearWarm();
  /** Kept alive (warm or hot) by a product mark, a channel open on it, or a link it carries. */
  bool IsWarm() const { return GetKeepaliveTier() != KeepaliveTier::None; }
  /**
   * The highest of the product mark (MarkWarm / MarkHot), the open channels' ChannelPolicy::keepalive and the
   * tier of the carrier-backed links riding this link's mux (SetCarriedKeepaliveTier).
   */
  KeepaliveTier GetKeepaliveTier() const;
  /** The product mark alone (what MarkWarm / MarkHot / ClearWarm set). */
  KeepaliveTier MarkedKeepaliveTier() const { return keepalive_tier_; }
  /**
   * The highest tier of the nested links carried over this link (PeerLinkManager sets it each sweep): a nested
   * link sends no keepalives of its own, so the link below it keeps the cadence its riders want.
   */
  void SetCarriedKeepaliveTier(KeepaliveTier tier) { carried_keepalive_tier_ = tier; }
  KeepaliveTier CarriedKeepaliveTier() const { return carried_keepalive_tier_; }
  /**
   * Nothing keeps the link alive any more (no mark, no channel asking) but it still announces a cadence:
   * tell the peer it stopped, as ClearWarm does, so both ends return to the cold window.
   */
  void StopKeepaliveIfUnheld();

  /** Scheduled keepalive announcing `interval_ms` (echo requested — docs/KEEPALIVE.md). */
  LinkRoe SendKeepalive(int64_t now_ms, uint32_t interval_ms);
  int64_t LastKeepaliveTxMs() const { return last_keepalive_tx_ms_; }
  void SetLastKeepaliveTxMs(int64_t ms) { last_keepalive_tx_ms_ = ms; }

  /**
   * Network changed under this link (PeerLinkManager::OnNetworkChanged): it is probed until an
   * authenticated packet arrives after `SuspectSinceMs()`, and evicted if none does within the grace.
   */
  void MarkSuspect(int64_t now_ms) {
    suspect_since_ms_ = now_ms;
    last_probe_ms_ = 0;
    suspect_from_verify_ = false;
  }
  /** Asked to prove it is alive (PeerLinkManager::VerifyLink): the same probing, its own drop reason. */
  void MarkVerifying(int64_t now_ms) {
    if (suspect_since_ms_ != 0) {
      return; // already probing (a network change): that verdict answers this too
    }
    suspect_since_ms_ = now_ms;
    last_probe_ms_ = 0;
    suspect_from_verify_ = true;
  }
  bool SuspectFromVerify() const { return suspect_from_verify_; }
  void ClearSuspect() {
    suspect_since_ms_ = 0;
    suspect_from_verify_ = false;
  }
  /** 0 when not suspect. */
  int64_t SuspectSinceMs() const { return suspect_since_ms_; }
  int64_t LastProbeMs() const { return last_probe_ms_; }
  /** Echo-requesting keepalive at the link's current cadence (ADP only). */
  LinkRoe SendProbe(int64_t now_ms);

  void RequestSessionRekey(std::function<void(Roe<void>)> on_complete);
  void HandleSessionControl(std::span<const uint8_t> payload);

  /** Carrier-backed: resend unacked reliable-lane frames (Tick). */
  void TickCarrierLane(int64_t now_ms);
  /**
   * Carrier-backed initiator: resend the last handshake message when no reply came (Tick). A carrier is BestEffort
   * and nothing below resends: one lost message used to end the handshake at its dial budget (two lossy long legs
   * through a relay, pp-browser #502).
   */
  void TickCarrierHandshake(int64_t now_ms);
  /** First resend of a carrier handshake message after this; each next one waits twice as long, up to the max. */
  static constexpr int64_t kCarrierHandshakeResendMs = 500;
  static constexpr int64_t kCarrierHandshakeResendMaxMs = 1500;
  /** Carrier-backed: a reliable-lane frame ran out of attempts — the path is gone. */
  bool CarrierLaneFailed() const { return lane_ && lane_->Failed(); }

  int64_t HandshakeStartedMs() const { return handshake_started_ms_; }
  void FailHandshakeTimeout();
  /** The peer closed the association during our handshake: it refused us (table full). */
  void FailRefused();
  /** A queued send of its handshake found no route to the peer (Endpoint::FlushSends): the dial fails now. */
  void FailUnreachable();
  /** Its dial's peer speaks none of our wire version (`versions` from its negotiation reply). */
  void FailVersionMismatch(const std::vector<uint8_t>& versions);
  void DemoteForScheduledDrop();

  /** Index / table ops — public so LinkTable needs no friendship into PeerLink. */
  void SetLinkIdentity(LinkId id, uint32_t generation) {
    link_id_ = id;
    generation_ = generation;
  }
  void RebindDialKey(std::string peer_key) { peer_key_ = std::move(peer_key); }

  void ApplyRemoteCapability(CapabilityPayload payload) { remote_capability_ = std::move(payload); }
  /** The peer's channel-0 extension (N032); null until it arrives, and always from a peer before it. */
  const CapabilityExtension* RemoteCapabilityExtension() const {
    return remote_extension_ ? &*remote_extension_ : nullptr;
  }
  void ApplyRemoteCapabilityExtension(CapabilityExtension extension) { remote_extension_ = std::move(extension); }
  bool CapabilityExtensionSent() const { return capability_extension_sent_; }
  void MarkCapabilityExtensionSent() { capability_extension_sent_ = true; }
  bool CapabilityExchangeStarted() const { return capability_exchange_started_; }
  void MarkCapabilityExchangeStarted() { capability_exchange_started_ = true; }
  bool CapabilityOfferSent() const { return capability_offer_sent_; }
  void MarkCapabilityOfferSent() { capability_offer_sent_ = true; }

  /** Exercises the MshChunk reassembly bound directly, without a live handshake. */
  Roe<std::optional<std::vector<uint8_t>>> PushMshChunkForTest(MshMessageType type, uint16_t index, uint16_t count,
                                                               std::span<const uint8_t> chunk) {
    return PushMshChunk(type, index, count, chunk);
  }

private:
  Roe<void> SendAdp(std::vector<uint8_t> payload, adp::QosClass qos);
  LinkRoe SendAdpLink(std::vector<uint8_t> payload, adp::QosClass qos);
  LinkRoe SendCarrierWire(std::vector<uint8_t> payload);
  void OnHandshakeComplete(Roe<MshAdpEstablished> established);
  void FinishEstablishment(MshAdpEstablished established);
  void FailAssociation(const Failure& failure);
  void FailAssociationMessage(const Error& error, Err code = Err::Generic);
  void AttachMuxTransport();
  void AttachCarrierFrameHandler();
  void HandleCarrierFrame(std::span<const uint8_t> payload);
  /** A carrier handshake message's piece (MshChunk): kept across resends until its message is whole. */
  void HandleCarrierMshChunk(std::span<const uint8_t> payload);
  void HandleLaneFrame(AmpAdpPayloadKind kind, std::span<const uint8_t> payload);
  void DeliverSealedCarrierWire(std::span<const uint8_t> wire);
  /** An opened frame to the mux, once Connected (a sealed packet before then is dropped; reliable ones resend). */
  void DeliverFrame(std::span<const uint8_t> frame);
  /** A carrier-backed link's packet: [kind][number] then `frame` sealed with the session's keys (A029 H1). */
  Roe<std::vector<uint8_t>> SealCarrierFrame(adp::QosClass qos, std::span<const uint8_t> frame);
  /** A carrier-backed link's frame budget: what one outer DATA frame carries, less the inner packet's wrapping. */
  size_t CarrierMaxFrameBytes() const;
  void FlushLaneBacklog();
  int64_t NowMs() const { return host_.now_ms ? host_.now_ms() : 0; }
  void StartHandshakeCommon(MshAdpHandshake::Role role, CompleteCb on_established);

  static Failure WrapConnectionFailure(const adp::Connection::Failure& child);

  /**
   * A carrier-backed link's replay windows: a hop between the two ends could replay an inner packet, and only the
   * window turns it away. The reliable one is wide: a lane resend arrives late, past newer best-effort traffic.
   */
  static constexpr size_t kCarrierBestEffortReplayWindow = 128;
  static constexpr size_t kCarrierReliableReplayWindow = 1024;

  LinkId link_id_;
  uint32_t generation_ = 0;
  std::string peer_key_;
  std::string remote_peer_id_;
  ByteVector remote_identity_public_key_;
  bool outbound_;
  std::shared_ptr<adp::Connection> connection_;
  std::shared_ptr<ChannelSession> carrier_;
  /** Carrier-backed only: reliable delivery for Reliable-class frames (CarrierLane). */
  std::unique_ptr<CarrierLane> lane_;
  /** Lane frames released in order before the mux existed (peer finished its handshake first). */
  std::vector<std::vector<uint8_t>> lane_backlog_;
  MshIdentity identity_;
  PeerLinkHostPorts host_;
  PeerLinkPhase phase_ = PeerLinkPhase::Handshaking;
  KeepaliveTier keepalive_tier_ = KeepaliveTier::None;
  KeepaliveTier carried_keepalive_tier_ = KeepaliveTier::None;
  int64_t last_keepalive_tx_ms_ = 0;
  int64_t suspect_since_ms_ = 0;
  bool suspect_from_verify_ = false;
  int64_t last_probe_ms_ = 0;
  bool capability_exchange_started_ = false;
  bool capability_offer_sent_ = false;
  std::optional<CapabilityPayload> remote_capability_;
  std::optional<CapabilityExtension> remote_extension_;
  bool capability_extension_sent_ = false;

  std::unique_ptr<MshAdpHandshake> handshake_;
  /** Carrier-backed initiator: when the last handshake message goes again, and the wait after that. */
  int64_t carrier_msh_resend_at_ms_ = 0;
  int64_t carrier_msh_resend_interval_ms_ = 0;
  /**
   * Carrier-backed responder, once Connected: its Finished, sent again if the initiator's Finished comes again (ours
   * was lost). Cleared at the first sealed frame from the peer, which it sends only once it has ours.
   */
  std::vector<std::vector<uint8_t>> carrier_final_msh_;
  /** Shared with the connection, which seals and opens with it (adp::IPacketProtection). */
  std::shared_ptr<Session> session_;
  /** Carrier-backed only: inner packet numbers, per space (the nonce's), and their replay windows. */
  uint32_t carrier_tx_be_ = 0;
  uint32_t carrier_tx_rel_ = 0;
  adp::ReplayWindow carrier_rx_be_{kCarrierBestEffortReplayWindow, /*slide_on_gap=*/true};
  adp::ReplayWindow carrier_rx_rel_{kCarrierReliableReplayWindow, /*slide_on_gap=*/true};
  std::unique_ptr<ChannelMux> mux_;
  ByteVector master_ikm_;
  ByteVector transcript_hash_;
  CompleteCb establish_cb_;

  MshMessageType msh_chunk_type_{};
  uint16_t msh_chunk_count_ = 0;
  std::vector<std::vector<uint8_t>> msh_chunk_parts_;
  size_t msh_chunk_bytes_ = 0;

  std::function<void(Roe<void>)> rekey_cb_;
  int64_t handshake_started_ms_ = 0;
  Roe<std::optional<std::vector<uint8_t>>> PushMshChunk(MshMessageType type, uint16_t index, uint16_t count,
                                                        std::span<const uint8_t> chunk);
  bool punched_ = false;
};

} // namespace pp::amp
