#pragma once

#include "amp/L1/Connection.h"
#include "amp/L3/Capability.h"
#include "amp/L3/ChannelMux.h"
#include "amp/L3/ChannelSession.h"
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
  /** Request parent erase after stack unwinds (A027). */
  std::function<void(std::string dial_key)> schedule_drop;
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
    Generic,
  };

  using Failure = CodedFailure<Err>;
  using LinkRoe = CodedRoe<void, Err>;
  using CompleteCb = std::function<void(LinkRoe)>;

  PeerLink(std::string peer_key, std::string remote_peer_id, bool outbound,
           std::shared_ptr<adp::Connection> connection, MshIdentity local_identity, PeerLinkHostPorts host);

  /** Nested Session over circuit carrier (no ADP Connection). */
  PeerLink(std::string peer_key, std::string remote_peer_id, bool outbound,
           std::shared_ptr<ChannelSession> carrier, MshIdentity local_identity, PeerLinkHostPorts host);

  ~PeerLink();

  PeerLink(const PeerLink&) = delete;
  PeerLink& operator=(const PeerLink&) = delete;

  void StartOutboundHandshake(CompleteCb on_established);
  void StartInboundHandshake(CompleteCb on_established);

  void HandleAdpPayload(std::span<const uint8_t> payload);

  PeerLinkPhase Phase() const { return phase_; }
  bool IsOutbound() const { return outbound_; }
  bool IsCarrierBacked() const { return carrier_ != nullptr; }
  TransportClass Transport() const {
    return IsCarrierBacked() ? TransportClass::Carrier : TransportClass::Adp;
  }
  LinkId Id() const { return link_id_; }
  uint32_t Generation() const { return generation_; }
  LinkHandle Handle() const { return LinkHandle{link_id_, generation_}; }
  const std::string& PeerKey() const { return peer_key_; }
  const std::string& RemotePeerId() const { return remote_peer_id_; }
  const ByteVector& RemoteIdentityPublicKey() const { return remote_identity_public_key_; }
  adp::Connection* ConnectionOrNull() { return connection_.get(); }
  ChannelMux* Mux() { return mux_.get(); }
  Session* GetSession() { return session_.get(); }
  ChannelSession* Carrier() { return carrier_.get(); }

  const CapabilityPayload* RemoteCapability() const {
    return remote_capability_ ? &*remote_capability_ : nullptr;
  }

  void MarkWarm();
  void MarkHot();
  void ClearWarm();
  bool IsWarm() const { return keepalive_tier_ != KeepaliveTier::None; }
  KeepaliveTier GetKeepaliveTier() const { return keepalive_tier_; }

  /** Scheduled keepalive announcing `interval_ms` (echo requested — docs/KEEPALIVE.md). */
  LinkRoe SendKeepalive(int64_t now_ms, uint32_t interval_ms);
  int64_t LastKeepaliveTxMs() const { return last_keepalive_tx_ms_; }
  void SetLastKeepaliveTxMs(int64_t ms) { last_keepalive_tx_ms_ = ms; }

  void RequestSessionRekey(std::function<void(Roe<void>)> on_complete);
  void HandleSessionControl(std::span<const uint8_t> payload);

  int64_t HandshakeStartedMs() const { return handshake_started_ms_; }
  void FailHandshakeTimeout();
  void DemoteForScheduledDrop();

  /** Index / table ops — public so LinkTable needs no friendship into PeerLink. */
  void SetLinkIdentity(LinkId id, uint32_t generation) {
    link_id_ = id;
    generation_ = generation;
  }
  void RebindDialKey(std::string peer_key) { peer_key_ = std::move(peer_key); }

  void ApplyRemoteCapability(CapabilityPayload payload) { remote_capability_ = std::move(payload); }
  bool CapabilityExchangeStarted() const { return capability_exchange_started_; }
  void MarkCapabilityExchangeStarted() { capability_exchange_started_ = true; }
  bool CapabilityOfferSent() const { return capability_offer_sent_; }
  void MarkCapabilityOfferSent() { capability_offer_sent_ = true; }

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
  void StartHandshakeCommon(MshAdpHandshake::Role role, CompleteCb on_established);

  static Failure WrapConnectionFailure(const adp::Connection::Failure& child);

  LinkId link_id_;
  uint32_t generation_ = 0;
  std::string peer_key_;
  std::string remote_peer_id_;
  ByteVector remote_identity_public_key_;
  bool outbound_;
  std::shared_ptr<adp::Connection> connection_;
  std::shared_ptr<ChannelSession> carrier_;
  MshIdentity identity_;
  PeerLinkHostPorts host_;
  PeerLinkPhase phase_ = PeerLinkPhase::Handshaking;
  KeepaliveTier keepalive_tier_ = KeepaliveTier::None;
  int64_t last_keepalive_tx_ms_ = 0;
  bool capability_exchange_started_ = false;
  bool capability_offer_sent_ = false;
  std::optional<CapabilityPayload> remote_capability_;

  std::unique_ptr<MshAdpHandshake> handshake_;
  std::unique_ptr<Session> session_;
  std::unique_ptr<ChannelMux> mux_;
  ByteVector master_ikm_;
  ByteVector transcript_hash_;
  CompleteCb establish_cb_;

  MshMessageType msh_chunk_type_{};
  uint16_t msh_chunk_count_ = 0;
  std::vector<std::vector<uint8_t>> msh_chunk_parts_;

  std::function<void(Roe<void>)> rekey_cb_;
  int64_t handshake_started_ms_ = 0;
  Roe<std::optional<std::vector<uint8_t>>> PushMshChunk(MshMessageType type, uint16_t index, uint16_t count,
                                                         std::span<const uint8_t> chunk);
};

} // namespace pp::amp
