#pragma once

#include "amp/L1/Connection.h"
#include "amp/L3/Capability.h"
#include "amp/L3/ChannelMux.h"
#include "amp/L3/ChannelSession.h"
#include "amp/link/CodedFailure.h"
#include "amp/link/MshAdpHandshake.h"
#include "amp/link/Types.h"
#include "amp/L2/Session.h"


#include <functional>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace pp::amp {

class PeerLinkManager;

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

  PeerLink(std::string peer_key, std::string remote_peer_id, const bool outbound,
           std::shared_ptr<adp::Connection> connection, MshIdentity local_identity, PeerLinkManager& owner);

  /** Nested Session over circuit carrier (no ADP Connection). */
  PeerLink(std::string peer_key, std::string remote_peer_id, const bool outbound,
           std::shared_ptr<ChannelSession> carrier, MshIdentity local_identity, PeerLinkManager& owner);

  ~PeerLink();

  PeerLink(const PeerLink&) = delete;
  PeerLink& operator=(const PeerLink&) = delete;

  void StartOutboundHandshake(CompleteCb on_established);
  void StartInboundHandshake(CompleteCb on_established);

  void HandleAdpPayload(std::span<const uint8_t> payload);

  PeerLinkPhase Phase() const { return phase_; }
  bool IsOutbound() const { return outbound_; }
  bool IsCarrierBacked() const { return carrier_ != nullptr; }
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

  LinkRoe SendKeepalive(int64_t now_ms);
  int64_t LastKeepaliveTxMs() const { return last_keepalive_tx_ms_; }
  void SetLastKeepaliveTxMs(int64_t ms) { last_keepalive_tx_ms_ = ms; }

  /** Wire-coordinated session rekey on channel 0 (after capability exchange). */
  void RequestSessionRekey(std::function<void(Roe<void>)> on_complete);

  void HandleSessionControl(std::span<const uint8_t> payload);

  int64_t HandshakeStartedMs() const { return handshake_started_ms_; }
  void FailHandshakeTimeout();

private:
  friend class PeerLinkManager;

  void SetPeerKey(std::string peer_key) { peer_key_ = std::move(peer_key); }
  void SetRemoteCapability(CapabilityPayload payload) { remote_capability_ = std::move(payload); }
  bool CapabilityExchangeStarted() const { return capability_exchange_started_; }
  void MarkCapabilityExchangeStarted() { capability_exchange_started_ = true; }
  bool CapabilityOfferSent() const { return capability_offer_sent_; }
  void MarkCapabilityOfferSent() { capability_offer_sent_ = true; }

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

  std::string peer_key_;
  std::string remote_peer_id_;
  ByteVector remote_identity_public_key_;
  bool outbound_;
  std::shared_ptr<adp::Connection> connection_;
  std::shared_ptr<ChannelSession> carrier_;
  MshIdentity identity_;
  PeerLinkManager& owner_;
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
