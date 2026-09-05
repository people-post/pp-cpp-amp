#pragma once

#include "amp/L1/Endpoint.h"
#include "amp/link/CodedFailure.h"
#include "amp/L3/Capability.h"
#include "amp/L3/ChannelPolicy.h"
#include "amp/link/PeerLink.h"
#include "amp/link/Types.h"
#include "amp/L2/Types.h"


#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pp::amp {

/** Dial + warm policy over ADP/AMP (replaces libp2p PeerSessionManager on the AMP path). */
class PeerLinkManager {
public:
  enum class Err : int32_t {
    Ok = 0,
    EndpointNotRegistered,
    DialInBackoff,
    TooManyConcurrentDials,
    MaxLinksReached,
    AssociationNotReady,
    LinkNotFound,
    NestedCarrierIncomplete,
    DialTimeout,
    HandshakeFailed,
    TransportFailed,
    DualDialLost,
    ChannelOpenFailed,
    Generic,
  };

  using Failure = CodedFailure<Err>;
  using LinkRoe = CodedRoe<void, Err>;
  using ChannelRoe = CodedRoe<uint32_t, Err>;

  using LinkCb = std::function<void(LinkRoe)>;
  using ChannelCb = std::function<void(ChannelRoe)>;
  using ProtocolHandler = std::function<void(PeerLink& link, uint32_t channel_id)>;
  /** Fired once per link when the remote capability payload is first decoded (ch0 / A016). */
  using CapabilityHandler = std::function<void(PeerLink& link, const CapabilityPayload& remote)>;

  PeerLinkManager(adp::Endpoint& endpoint, MshIdentity local_identity, std::string local_peer_id,
                  PeerLinkConfig config = {});
  ~PeerLinkManager();

  PeerLinkManager(const PeerLinkManager&) = delete;
  PeerLinkManager& operator=(const PeerLinkManager&) = delete;

  adp::Endpoint& GetEndpoint() { return endpoint_; }
  const std::string& LocalPeerId() const { return local_peer_id_; }

  /** Local ch0 advertisement (Identify replacement on the AMP path). */
  void SetLocalListenMultiaddrs(std::vector<std::string> multiaddrs);
  void SetAdvertisedProtocols(std::vector<std::string> protocols);
  void SetCapabilityHandler(CapabilityHandler handler);
  CapabilityPayload LocalCapability() const;

  /** Preferred ADP listen multiaddr learned for a PeerId (ch0 / RegisterEndpoint). */
  std::optional<std::string> PreferredMultiaddr(const std::string& peer_id) const;

  Roe<void> RegisterEndpoint(const std::string& peer_key, const std::string& multiaddr);

  void EnsureAssociation(const std::string& peer_key, LinkCb on_complete);
  void OpenChannel(const std::string& peer_key, const std::string& protocol_id, ChannelPolicy policy,
                   ChannelCb on_complete);
  void OpenChannelOnLink(PeerLink& link, const std::string& protocol_id, ChannelPolicy policy, ChannelCb on_complete);

  /**
   * [A024] Establish nested Session over a bridged ChannelSession and install as PeerLink.
   * `peer_key` is the provisional dial key (usually remote PeerId); rekeyed to authenticated id.
   */
  void EstablishNestedOverCarrier(const std::string& peer_key, std::shared_ptr<ChannelSession> carrier,
                                  bool initiator, LinkCb on_complete);

  /**
   * Accept inbound nested-Session carrier opens.
   * `protocol_id` defaults to Amp-owned `kAmpCircuitCarrierProtocolId` (`/amp/circuit-carrier/1.0.0`).
   * Products should keep the default; override only for Amp-internal tests.
   */
  void EnableNestedCarrierAccept(bool enable,
                                 std::string protocol_id = kAmpCircuitCarrierProtocolId);

  /** L4 entry — applied to every link mux (existing + future). */
  void SetProtocolHandler(const std::string& protocol_id, ProtocolHandler handler);
  void RemoveProtocolHandler(const std::string& protocol_id);
  void ClearProtocolHandlers();

  PeerLinkSnapshot GetLinkSnapshot(const std::string& peer_key) const;
  bool IsConnected(const std::string& peer_key) const;

  void MarkWarm(const std::string& peer_key);
  void MarkHot(const std::string& peer_key);
  void ClearWarm(const std::string& peer_key);

  PeerLink* FindLink(const std::string& peer_key);
  const PeerLink* FindLink(const std::string& peer_key) const;
  PeerLink* FindLinkByPeerId(const std::string& peer_id);
  const PeerLink* FindLinkByPeerId(const std::string& peer_id) const;
  PeerLink* FindConnectedInboundLink();

  /** Connected links whose RemotePeerId matches (for dual-dial / A026 tests). */
  size_t CountConnectedLinksForPeerId(const std::string& peer_id) const;

  size_t CountLinks() const { return links_.size(); }

  static bool IsAssociationNotReady(const Failure& failure) {
    return failure.GetCode() == Err::AssociationNotReady;
  }

  void Tick();

private:
  friend class PeerLink;

  struct EndpointRecord {
    std::string multiaddr;
    adp::IpEndpoint endpoint;
    std::string peer_id;
  };

  void InstallAcceptHandler();
  void OnInboundConnection(std::shared_ptr<adp::Connection> connection);
  /** Returns false if `link` lost dual-dial election and must be torn down by the caller. */
  bool OnLinkEstablished(PeerLink& link);
  void ScheduleDropLink(std::string peer_key);
  void ApplyProtocolHandlers(PeerLink& link);
  void StartCapabilityExchange(PeerLink& link);
  void OnCapabilityData(const std::string& peer_key, std::vector<uint8_t> payload);
  void OnCh0Data(const std::string& peer_key, std::vector<uint8_t> payload);
  void IngestRemoteCapabilityAddrs(PeerLink& link, const CapabilityPayload& remote);
  void FinishDial(const std::string& peer_key, LinkRoe result);
  void FinishNestedCarrier(const std::string& provisional_key, LinkRoe result);
  void HandleInboundCarrierChannel(PeerLink& via_link, uint32_t channel_id);
  std::string DeriveRemotePeerId(const ByteVector& identity_public_key) const;
  /** Returns false if `link` was erased as the dual-dial loser ([A026]). */
  bool AdoptInboundOrDropDuplicate(PeerLink& inbound);
  void RekeyLink(const std::string& from_key, const std::string& to_key);
  PeerLink* FindConnectedLinkForPeerId(const std::string& peer_id);
  /** Scan all Connected links for `remote_peer_id` (not only peer_id_to_key_). */
  PeerLink* FindAnyConnectedLinkForRemotePeerId(const std::string& remote_peer_id);
  /** A026: elect one of two Connected links to the same remote PeerId. */
  PeerLink* ElectDualDialWinner(PeerLink& existing, PeerLink& candidate) const;
  void DropLink(const std::string& peer_key);
  /** After dual-dial loser outbound drops, rekey winner onto the dial alias. */
  void ScheduleAdoptDialAlias(std::string remote_peer_id, std::string dial_alias);

  void MaybeSendKeepalives(int64_t now_ms);

  static Failure WrapPeerLinkFailure(const PeerLink::Failure& child);
  static LinkRoe WrapPeerLinkResult(const PeerLink::LinkRoe& child);

  adp::Endpoint& endpoint_;
  MshIdentity local_identity_;
  std::string local_peer_id_;
  PeerLinkConfig config_;
  std::vector<std::string> local_listen_multiaddrs_;
  std::vector<std::string> advertised_protocols_;
  CapabilityHandler capability_handler_;
  bool nested_carrier_accept_ = false;
  std::string nested_carrier_protocol_id_;

  std::unordered_map<std::string, EndpointRecord> endpoints_;
  std::unordered_map<std::string, std::unique_ptr<PeerLink>> links_;
  std::unordered_map<std::string, ProtocolHandler> protocol_handlers_;
  std::unordered_map<std::string, std::string> peer_id_to_key_;
  std::unordered_map<std::string, std::vector<LinkCb>> inflight_associations_;
  std::unordered_map<std::string, std::chrono::steady_clock::time_point> dial_failed_until_;
  std::unordered_map<std::string, Failure> last_error_;
  size_t concurrent_dials_ = 0;
  /** Erase after PeerLink stack unwinds (dual-dial loser must not destroy `this` mid-callback). */
  std::vector<std::string> pending_drop_keys_;
  std::vector<std::pair<std::string, std::string>> pending_alias_adopt_;
};

} // namespace pp::amp
