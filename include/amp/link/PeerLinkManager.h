#pragma once

#include "amp/L1/Endpoint.h"
#include "amp/L3/Capability.h"
#include "amp/L3/ChannelPolicy.h"
#include "amp/L3/ChannelSession.h"
#include "amp/L2/Types.h"
#include "amp/link/CodedFailure.h"
#include "amp/link/DialBook.h"
#include "amp/link/LinkIdentity.h"
#include "amp/link/LinkTable.h"
#include "amp/link/PeerLink.h"
#include "amp/link/Types.h"

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pp::amp {

/**
 * Dial + warm policy over ADP/AMP (façade over DialBook / LinkTable / association).
 * Strand-affine: constructed with MeshRuntime::io_mu_ or owned test mutex.
 * Product should prefer MeshRuntime APIs; FindLink is Amp-internal / tests only.
 */
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
  /** Product/L4 inbound OPEN — no PeerLink& (ADR_LINK_PLANE). */
  using ProtocolHandler =
      std::function<void(LinkHandle handle, const std::string& remote_peer_id, uint32_t channel_id)>;
  /** Product capability observer — no PeerLink& (ADR_LINK_PLANE). */
  using CapabilityHandler =
      std::function<void(LinkHandle handle, const std::string& remote_peer_id, const CapabilityPayload& remote)>;
  /** Queue association/channel completions off the establish stack (MeshRuntime::PostToIo). */
  using CompletionPoster = std::function<void(std::function<void()>)>;

  PeerLinkManager(adp::Endpoint& endpoint, MshIdentity local_identity, std::string local_peer_id,
                  PeerLinkConfig config = {});
  PeerLinkManager(adp::Endpoint& endpoint, MshIdentity local_identity, std::string local_peer_id,
                  PeerLinkConfig config, std::recursive_mutex& strand_mu);
  ~PeerLinkManager();

  PeerLinkManager(const PeerLinkManager&) = delete;
  PeerLinkManager& operator=(const PeerLinkManager&) = delete;

  /** Wire MeshRuntime::PostToIo so FinishDial waiters never run under association mutation. */
  void SetCompletionPoster(CompletionPoster poster);

  adp::Endpoint& GetEndpoint() { return endpoint_; }
  const std::string& LocalPeerId() const { return local_peer_id_; }
  LinkTable& Table() { return table_; }
  const LinkTable& Table() const { return table_; }
  DialBook& Book() { return book_; }
  const DialBook& Book() const { return book_; }

  void SetLocalListenMultiaddrs(std::vector<std::string> multiaddrs);
  void SetAdvertisedProtocols(std::vector<std::string> protocols);
  void SetCapabilityHandler(CapabilityHandler handler);
  CapabilityPayload LocalCapability() const;

  std::optional<std::string> PreferredMultiaddr(const std::string& peer_id) const;
  Roe<void> RegisterEndpoint(const std::string& peer_key, const std::string& multiaddr);

  void EnsureAssociation(const std::string& peer_key, LinkCb on_complete);
  void OpenChannel(const std::string& peer_key, const std::string& protocol_id, ChannelPolicy policy,
                   ChannelCb on_complete);
  void OpenChannelOnLink(PeerLink& link, const std::string& protocol_id, ChannelPolicy policy, ChannelCb on_complete);

  void EstablishNestedOverCarrier(const std::string& peer_key, std::shared_ptr<ChannelSession> carrier,
                                  bool initiator, LinkCb on_complete);

  void EnableNestedCarrierAccept(bool enable,
                                 std::string protocol_id = kAmpCircuitCarrierProtocolId);

  void SetProtocolHandler(const std::string& protocol_id, ProtocolHandler handler);
  void RemoveProtocolHandler(const std::string& protocol_id);
  void ClearProtocolHandlers();

  PeerLinkSnapshot GetLinkSnapshot(const std::string& peer_key) const;
  LinkSnapshotEx GetSnapshotByDialKey(const DialKey& key) const;
  LinkSnapshotEx GetSnapshotByPeerId(const std::string& peer_id,
                                     TransportClass prefer = TransportClass::Adp) const;
  bool IsConnected(const std::string& peer_key) const;
  bool IsConnectedToPeerId(const std::string& peer_id) const;
  bool IsReachable(const std::string& peer_id) const;

  /** Run fn with live PeerLink while holding the strand — fn must not escape the reference. */
  template <typename Fn>
  bool WithLiveLink(LinkHandle handle, Fn&& fn) {
    std::lock_guard lock(strand_mu_);
    auto* link = table_.FindLive(handle);
    if (!link) {
      return false;
    }
    std::forward<Fn>(fn)(*link);
    return true;
  }
  template <typename Fn>
  bool WithLiveLinkByDialKey(const DialKey& key, Fn&& fn) {
    std::lock_guard lock(strand_mu_);
    auto* link = FindLink(key);
    if (!link) {
      return false;
    }
    std::forward<Fn>(fn)(*link);
    return true;
  }

  /**
   * Poll until mux channel is open or Amp-clock deadline; invokes done via completion poster.
   * `deadline_ms` is absolute `Endpoint::GetClock().NowMs()` — prefer WhenChannelOpenIn.
   */
  void WhenChannelOpen(const DialKey& peer_key, uint32_t channel_id, int64_t deadline_ms,
                       std::function<void(bool ok)> done);
  /** Remaining duration from Amp clock now → absolute deadline for WhenChannelOpen. */
  void WhenChannelOpenIn(const DialKey& peer_key, uint32_t channel_id, std::chrono::milliseconds remaining,
                         std::function<void(bool ok)> done);
  /**
   * Bind ChannelSession under strand lock; returns empty if link/mux missing.
   * `peer_key` may be a dial alias or authenticated PeerId.
   */
  std::shared_ptr<ChannelSession> BindChannel(const DialKey& peer_key, uint32_t channel_id,
                                              ChannelPolicy policy,
                                              ChannelSession::FrameHandler on_frame,
                                              ChannelSession::ClosedCallback on_closed = {});

  void MarkWarm(const std::string& peer_key);
  void MarkHot(const std::string& peer_key);
  void ClearWarm(const std::string& peer_key);

  void ClearDialBackoff(const std::string& peer_key);
  void AbortInflightDial(const std::string& peer_key);

  /**
   * Amp-internal / tests — prefer WithLiveLink / snapshots for product.
   * Resolves dial alias first, then Connected PeerId presence (inbound handlers pass PeerId).
   */
  PeerLink* FindLink(const std::string& peer_key);
  const PeerLink* FindLink(const std::string& peer_key) const;
  PeerLink* FindLinkByPeerId(const std::string& peer_id);
  const PeerLink* FindLinkByPeerId(const std::string& peer_id) const;
  PeerLink* FindConnectedInboundLink();

  size_t CountConnectedLinksForPeerId(const std::string& peer_id) const;
  size_t CountLinks() const;

  static bool IsAssociationNotReady(const Failure& failure) {
    return failure.GetCode() == Err::AssociationNotReady;
  }

  void Tick();

  /** Project manager capabilities into PeerLink's host ports (composition root). */
  PeerLinkHostPorts MakeHostPorts();

private:
  void InstallAcceptHandler();
  void OnInboundConnection(std::shared_ptr<adp::Connection> connection);
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
  bool AdoptInboundOrDropDuplicate(PeerLink& inbound);
  /** Rebind dial alias for a live LinkId (index-only; replaces map-key RekeyLink). */
  void BindDialAlias(LinkId id, DialKey to_key);
  PeerLink* FindConnectedLinkForPeerId(const std::string& peer_id);
  PeerLink* FindAnyConnectedLinkForRemotePeerId(const std::string& remote_peer_id);
  PeerLink* ElectDualDialWinner(PeerLink& existing, PeerLink& candidate) const;
  void DropLink(const std::string& peer_key);
  void ScheduleAdoptDialAlias(std::string remote_peer_id, std::string dial_alias);
  void MaybeSendKeepalives(int64_t now_ms);
  void PostCompletion(std::function<void()> fn);
  void AssignLinkIdentity(PeerLink& link);
  void RefreshPresence(PeerLink& link);
  LinkSnapshotEx SnapshotOf(const PeerLink* link, const DialKey& dial_key, bool has_endpoint,
                            const std::string& multiaddr) const;

  static Failure WrapPeerLinkFailure(const PeerLink::Failure& child);
  static LinkRoe WrapPeerLinkResult(const PeerLink::LinkRoe& child);

  adp::Endpoint& endpoint_;
  MshIdentity local_identity_;
  std::string local_peer_id_;
  DialBook book_;
  LinkTable table_;
  std::vector<std::string> local_listen_multiaddrs_;
  std::vector<std::string> advertised_protocols_;
  CapabilityHandler capability_handler_;
  CompletionPoster completion_poster_;
  bool nested_carrier_accept_ = false;
  std::string nested_carrier_protocol_id_;

  std::unordered_map<std::string, ProtocolHandler> protocol_handlers_;
  std::unordered_map<std::string, std::vector<LinkCb>> inflight_associations_;
  std::unordered_map<std::string, Failure> last_error_;
  std::unordered_set<std::string> suppress_dial_backoff_;
  std::vector<std::string> pending_drop_keys_;
  std::vector<std::pair<std::string, std::string>> pending_alias_adopt_;
  std::vector<std::tuple<DialKey, uint32_t, int64_t, std::function<void(bool)>>> channel_open_waiters_;

  std::recursive_mutex owned_mu_;
  std::recursive_mutex& strand_mu_;
};

} // namespace pp::amp
