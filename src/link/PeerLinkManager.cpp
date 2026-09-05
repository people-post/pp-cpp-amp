#include "amp/link/PeerLinkManager.h"

#include "amp/link/CodedFailure.h"
#include "amp/L3/ChannelPolicy.h"
#include "amp/L3/ChannelSession.h"
#include "amp/L3/Types.h"
#include "amp/link/AdpMultiaddr.h"
#include "amp/L2/SessionControl.h"
#include "amp/link/Types.h"

#include <iterator>

namespace pp::amp {

PeerLinkManager::Failure PeerLinkManager::WrapPeerLinkFailure(const PeerLink::Failure& child) {
  switch (child.GetCode()) {
  case PeerLink::Err::DialTimeout:
    return Failure::Of(Err::DialTimeout,
                       detail::AppendFrom("amp link manager: dial timeout", "link", child.message));
  case PeerLink::Err::DualDialLost:
    return Failure::Of(Err::DualDialLost,
                       detail::AppendFrom("amp link manager: dual-dial lost", "link", child.message));
  case PeerLink::Err::HandshakeFailed:
    return Failure::Of(Err::HandshakeFailed,
                       detail::AppendFrom("amp link manager: handshake failed", "link", child.message));
  case PeerLink::Err::TransportUnavailable:
    return Failure::Of(Err::TransportFailed,
                       detail::AppendFrom("amp link manager: transport unavailable", "link", child.message));
  case PeerLink::Err::TransportFailed:
  case PeerLink::Err::NoConnection:
  case PeerLink::Err::CarrierClosed:
  case PeerLink::Err::CarrierEnqueueFailed:
    return Failure::Of(Err::TransportFailed,
                       detail::AppendFrom("amp link manager: transport failed", "link", child.message));
  case PeerLink::Err::NotConnected:
    return Failure::Of(Err::AssociationNotReady,
                       detail::AppendFrom("amp link manager: association not ready", "link", child.message));
  default:
    return Failure::Of(Err::Generic, detail::AppendFrom("amp link manager: link error", "link", child.message));
  }
}

PeerLinkManager::LinkRoe PeerLinkManager::WrapPeerLinkResult(const PeerLink::LinkRoe& child) {
  if (child.isOk()) {
    return LinkRoe();
  }
  return LinkRoe::error(WrapPeerLinkFailure(child.error()));
}

PeerLinkManager::PeerLinkManager(adp::Endpoint& endpoint, MshIdentity local_identity, std::string local_peer_id,
                                 PeerLinkConfig config)
    : endpoint_(endpoint), local_identity_(std::move(local_identity)), local_peer_id_(std::move(local_peer_id)),
      config_(config) {
  endpoint_.SetAcceptKey(PreSessionPeerKey());
  InstallAcceptHandler();
}

PeerLinkManager::~PeerLinkManager() {
  // Nested carriers Bind to an outer link's Mux ([A024]). Unbind while every Mux still
  // exists — unordered_map destroy order is arbitrary and ~ChannelSession would UAF.
  for (auto& [_, link] : links_) {
    if (link && link->Carrier()) {
      link->Carrier()->ReleaseHandlers();
    }
  }
  links_.clear();
}

void PeerLinkManager::SetLocalListenMultiaddrs(std::vector<std::string> multiaddrs) {
  local_listen_multiaddrs_ = std::move(multiaddrs);
}

void PeerLinkManager::SetAdvertisedProtocols(std::vector<std::string> protocols) {
  advertised_protocols_ = std::move(protocols);
}

void PeerLinkManager::SetCapabilityHandler(CapabilityHandler handler) {
  capability_handler_ = std::move(handler);
}

CapabilityPayload PeerLinkManager::LocalCapability() const {
  CapabilityPayload payload;
  payload.local_peer_id = local_peer_id_;
  payload.listen_multiaddrs = local_listen_multiaddrs_;
  payload.protocols = advertised_protocols_;
  return payload;
}

std::optional<std::string> PeerLinkManager::PreferredMultiaddr(const std::string& peer_id) const {
  if (peer_id.empty()) {
    return std::nullopt;
  }
  if (const auto it = endpoints_.find(peer_id); it != endpoints_.end()) {
    return it->second.multiaddr;
  }
  for (const auto& [_, rec] : endpoints_) {
    if (rec.peer_id == peer_id && !rec.multiaddr.empty()) {
      return rec.multiaddr;
    }
  }
  return std::nullopt;
}

void PeerLinkManager::InstallAcceptHandler() {
  endpoint_.SetAcceptHandler([this](std::shared_ptr<adp::Connection> connection) {
    OnInboundConnection(std::move(connection));
  });
}

Roe<void> PeerLinkManager::RegisterEndpoint(const std::string& peer_key, const std::string& multiaddr) {
  auto parsed = ParseAdpMultiaddr(multiaddr);
  if (!parsed) {
    return parsed.error();
  }
  EndpointRecord rec;
  rec.multiaddr = multiaddr;
  rec.endpoint = parsed->endpoint;
  rec.peer_id = parsed->peer_id;
  endpoints_[peer_key] = std::move(rec);
  return Roe<void>();
}

PeerLink* PeerLinkManager::FindLink(const std::string& peer_key) {
  auto it = links_.find(peer_key);
  if (it == links_.end()) {
    return nullptr;
  }
  return it->second.get();
}

const PeerLink* PeerLinkManager::FindLink(const std::string& peer_key) const {
  auto it = links_.find(peer_key);
  if (it == links_.end()) {
    return nullptr;
  }
  return it->second.get();
}

PeerLink* PeerLinkManager::FindLinkByPeerId(const std::string& peer_id) {
  if (peer_id.empty()) {
    return nullptr;
  }
  if (const auto it = peer_id_to_key_.find(peer_id); it != peer_id_to_key_.end()) {
    return FindLink(it->second);
  }
  for (auto& [key, link] : links_) {
    if (link->Phase() == PeerLinkPhase::Connected && link->RemotePeerId() == peer_id) {
      peer_id_to_key_[peer_id] = key;
      return link.get();
    }
  }
  return nullptr;
}

const PeerLink* PeerLinkManager::FindLinkByPeerId(const std::string& peer_id) const {
  return const_cast<PeerLinkManager*>(this)->FindLinkByPeerId(peer_id);
}

PeerLink* PeerLinkManager::FindConnectedLinkForPeerId(const std::string& peer_id) {
  if (auto* link = FindLinkByPeerId(peer_id)) {
    if (link->Phase() == PeerLinkPhase::Connected) {
      return link;
    }
  }
  return nullptr;
}

PeerLink* PeerLinkManager::FindAnyConnectedLinkForRemotePeerId(const std::string& remote_peer_id) {
  if (remote_peer_id.empty()) {
    return nullptr;
  }
  for (auto& [_, link] : links_) {
    if (link && link->Phase() == PeerLinkPhase::Connected && link->RemotePeerId() == remote_peer_id) {
      return link.get();
    }
  }
  return nullptr;
}

PeerLink* PeerLinkManager::ElectDualDialWinner(PeerLink& existing, PeerLink& candidate) const {
  const std::string& remote = existing.RemotePeerId().empty() ? candidate.RemotePeerId() : existing.RemotePeerId();
  const bool existing_keep_out = existing.IsOutbound() && !remote.empty() && local_peer_id_ > remote;
  const bool cand_keep_out = candidate.IsOutbound() && !remote.empty() && local_peer_id_ > remote;
  if (cand_keep_out && !existing_keep_out) {
    return &candidate;
  }
  if (existing_keep_out) {
    return &existing;
  }
  // Local does not win glare: prefer inbound over own outbound (A021 yield).
  if (existing.IsOutbound() && !candidate.IsOutbound()) {
    return &candidate;
  }
  if (candidate.IsOutbound() && !existing.IsOutbound()) {
    return &existing;
  }
  return &existing;
}

void PeerLinkManager::DropLink(const std::string& peer_key) {
  auto* link = FindLink(peer_key);
  if (!link) {
    return;
  }
  if (link->Carrier()) {
    // Nested link: unbind from outer Mux while that Mux is still alive.
    link->Carrier()->ReleaseHandlers();
  }
  ChannelMux* dying_mux = link->Mux();
  if (dying_mux) {
    // Other nested carriers may still point at this Mux — orphan before it dies.
    for (auto& [_, other] : links_) {
      if (!other || other.get() == link) {
        continue;
      }
      if (other->Carrier() && other->Carrier()->Mux() == dying_mux) {
        other->Carrier()->OrphanFromMux();
      }
    }
    dying_mux->ClearProtocolHandlers();
  }
  const std::string remote = link->RemotePeerId();
  links_.erase(peer_key);
  if (!remote.empty()) {
    if (auto it = peer_id_to_key_.find(remote); it != peer_id_to_key_.end() && it->second == peer_key) {
      peer_id_to_key_.erase(it);
    }
  }
}

void PeerLinkManager::ScheduleDropLink(std::string peer_key) {
  pending_drop_keys_.push_back(std::move(peer_key));
}

void PeerLinkManager::ScheduleAdoptDialAlias(std::string remote_peer_id, std::string dial_alias) {
  if (remote_peer_id.empty() || dial_alias.empty()) {
    return;
  }
  pending_alias_adopt_.emplace_back(std::move(remote_peer_id), std::move(dial_alias));
}

size_t PeerLinkManager::CountConnectedLinksForPeerId(const std::string& peer_id) const {
  if (peer_id.empty()) {
    return 0;
  }
  size_t n = 0;
  for (const auto& [_, link] : links_) {
    if (link && link->Phase() == PeerLinkPhase::Connected && link->RemotePeerId() == peer_id) {
      ++n;
    }
  }
  return n;
}

bool PeerLinkManager::OnLinkEstablished(PeerLink& link) {
  if (!AdoptInboundOrDropDuplicate(link)) {
    if (link.Mux()) {
      link.Mux()->ClearProtocolHandlers();
    }
    return false;
  }
  if (!link.RemotePeerId().empty()) {
    const auto it = peer_id_to_key_.find(link.RemotePeerId());
    // Prefer ADP (non-carrier) as the primary PeerId index; nested carriers coexist ([A024]).
    if (it == peer_id_to_key_.end() || !link.IsCarrierBacked()) {
      peer_id_to_key_[link.RemotePeerId()] = link.PeerKey();
    }
  }
  ApplyProtocolHandlers(link);
  // Nested carrier links skip ch0 — product reachability already established via outer mesh.
  if (!link.IsCarrierBacked()) {
    StartCapabilityExchange(link);
  }
  return true;
}

bool PeerLinkManager::AdoptInboundOrDropDuplicate(PeerLink& candidate) {
  if (candidate.RemotePeerId().empty()) {
    return true;
  }
  const std::string remote = candidate.RemotePeerId();

  // Find another Connected link to the same PeerId (map may still point at candidate).
  PeerLink* existing = nullptr;
  for (auto& [key, link] : links_) {
    if (!link || link.get() == &candidate || link->Phase() != PeerLinkPhase::Connected) {
      continue;
    }
    if (link->RemotePeerId() == remote) {
      existing = link.get();
      break;
    }
  }

  if (!existing) {
    if (!candidate.IsOutbound()) {
      for (const auto& [alias, rec] : endpoints_) {
        if (rec.peer_id == remote && !links_.contains(alias)) {
          RekeyLink(candidate.PeerKey(), alias);
          peer_id_to_key_[remote] = alias;
          return true;
        }
      }
    }
    peer_id_to_key_[remote] = candidate.PeerKey();
    return true;
  }

  // [A024] ADP Session and nested carrier Session to the same PeerId coexist.
  if (existing->IsCarrierBacked() != candidate.IsCarrierBacked()) {
    if (!existing->IsCarrierBacked()) {
      peer_id_to_key_[remote] = existing->PeerKey();
    } else if (!candidate.IsCarrierBacked()) {
      peer_id_to_key_[remote] = candidate.PeerKey();
    }
    return true;
  }

  // Dual-dial ([A026]): elect one Session per PeerId — reject keep-both.
  PeerLink* winner = ElectDualDialWinner(*existing, candidate);
  PeerLink* loser = (winner == existing) ? &candidate : existing;
  const std::string winner_key = winner->PeerKey();

  if (loser == &candidate) {
    // Do not erase `candidate` here — PeerLink is still on the stack ([A026]).
    peer_id_to_key_[remote] = winner_key;
    return false;
  }

  // Existing link loses: never DropLink here — may be mid-pump or still referenced by L4
  // ChannelSession mux pointers ([A026]/A027]/OWNERSHIP). Clear handlers, demote, erase on Tick.
  if (loser->Mux()) {
    loser->Mux()->ClearProtocolHandlers();
  }
  loser->phase_ = PeerLinkPhase::Backoff;
  ScheduleDropLink(loser->PeerKey());
  // Winner is the new candidate — prefer dial alias when free.
  if (!candidate.IsOutbound()) {
    for (const auto& [alias, rec] : endpoints_) {
      if (rec.peer_id == remote && !links_.contains(alias) && candidate.PeerKey() != alias) {
        RekeyLink(candidate.PeerKey(), alias);
        peer_id_to_key_[remote] = alias;
        return true;
      }
    }
  }
  peer_id_to_key_[remote] = candidate.PeerKey();
  return true;
}

std::string PeerLinkManager::DeriveRemotePeerId(const ByteVector& identity_public_key) const {
  if (config_.peer_id_from_identity) {
    return config_.peer_id_from_identity(identity_public_key);
  }
  return IdentityPublicKeyFingerprint(identity_public_key);
}

PeerLink* PeerLinkManager::FindConnectedInboundLink() {
  for (auto& [_, link] : links_) {
    if (!link->IsOutbound() && link->Phase() == PeerLinkPhase::Connected) {
      return link.get();
    }
  }
  return nullptr;
}

bool PeerLinkManager::IsConnected(const std::string& peer_key) const {
  if (const auto* link = FindLink(peer_key)) {
    return link->Phase() == PeerLinkPhase::Connected;
  }
  return false;
}

PeerLinkSnapshot PeerLinkManager::GetLinkSnapshot(const std::string& peer_key) const {
  PeerLinkSnapshot snap;
  snap.has_endpoint = endpoints_.contains(peer_key);
  if (const auto* link = FindLink(peer_key); link && link->Phase() == PeerLinkPhase::Connected) {
    snap.phase = PeerLinkPhase::Connected;
    if (snap.has_endpoint) {
      snap.multiaddr = endpoints_.at(peer_key).multiaddr;
    }
    return snap;
  }
  if (!snap.has_endpoint) {
    snap.phase = PeerLinkPhase::Unavailable;
    return snap;
  }
  snap.multiaddr = endpoints_.at(peer_key).multiaddr;
  if (const auto* link = FindLink(peer_key)) {
    snap.phase = link->Phase();
  } else if (const auto it = dial_failed_until_.find(peer_key); it != dial_failed_until_.end()) {
    const auto now = std::chrono::steady_clock::now();
    if (it->second > now) {
      snap.phase = PeerLinkPhase::Backoff;
      snap.backoff_remaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(it->second - now);
    } else {
      snap.phase = PeerLinkPhase::Idle;
    }
  } else {
    snap.phase = PeerLinkPhase::Idle;
  }
  if (const auto err = last_error_.find(peer_key); err != last_error_.end()) {
    snap.detail = err->second.message;
  }
  return snap;
}

void PeerLinkManager::EnsureAssociation(const std::string& peer_key, LinkCb on_complete) {
  if (IsConnected(peer_key)) {
    if (on_complete) {
      on_complete(LinkRoe());
    }
    return;
  }

  const auto ep_it = endpoints_.find(peer_key);
  if (ep_it != endpoints_.end()) {
    if (auto* existing = FindConnectedLinkForPeerId(ep_it->second.peer_id)) {
      // Nested/circuit carrier Sessions coexist with ADP ([A024]). A carrier-backed
      // link must not satisfy EnsureAssociation for a new ADP dial alias — otherwise
      // upgrade-from-circuit punch rekeys the nested Session and never opens direct.
      if (!existing->IsCarrierBacked()) {
        if (existing->PeerKey() != peer_key) {
          RekeyLink(existing->PeerKey(), peer_key);
        }
        if (on_complete) {
          on_complete(LinkRoe());
        }
        return;
      }
    }
  }

  if (auto* existing = FindLink(peer_key)) {
    if (existing->Phase() == PeerLinkPhase::Handshaking || existing->Phase() == PeerLinkPhase::Dialing) {
      inflight_associations_[peer_key].push_back(std::move(on_complete));
      return;
    }
  }

  if (ep_it == endpoints_.end()) {
    if (on_complete) {
      on_complete(LinkRoe::error(Failure::Of(Err::EndpointNotRegistered, "amp link: peer endpoint not registered")));
    }
    return;
  }

  const auto now = std::chrono::steady_clock::now();
  if (const auto backoff = dial_failed_until_.find(peer_key); backoff != dial_failed_until_.end()) {
    if (backoff->second > now) {
      if (on_complete) {
        on_complete(LinkRoe::error(Failure::Of(Err::DialInBackoff, "amp link: dial in backoff")));
      }
      return;
    }
    dial_failed_until_.erase(backoff);
  }

  if (concurrent_dials_ >= config_.max_concurrent_dials) {
    if (on_complete) {
      on_complete(LinkRoe::error(Failure::Of(Err::TooManyConcurrentDials, "amp link: too many concurrent dials")));
    }
    return;
  }

  if (links_.size() >= config_.max_links) {
    if (on_complete) {
      on_complete(LinkRoe::error(Failure::Of(Err::MaxLinksReached, "amp link: max links reached")));
    }
    return;
  }

  adp::OpenParams params;
  params.key = PreSessionPeerKey();
  params.mint_id = true;
  params.peer = ep_it->second.endpoint;
  auto opened = endpoint_.Open(params);
  if (!opened) {
    if (on_complete) {
      on_complete(LinkRoe::error(Failure::Of(Err::TransportFailed, opened.error().message)));
    }
    return;
  }

  ++concurrent_dials_;
  inflight_associations_[peer_key].push_back(std::move(on_complete));

  auto link = std::make_unique<PeerLink>(peer_key, ep_it->second.peer_id, true, *opened, local_identity_, *this);
  link->StartOutboundHandshake([this, peer_key](PeerLink::LinkRoe result) {
    FinishDial(peer_key, WrapPeerLinkResult(result));
  });
  links_[peer_key] = std::move(link);
}

void PeerLinkManager::OpenChannelOnLink(PeerLink& link, const std::string& protocol_id, ChannelPolicy policy,
                                        ChannelCb on_complete) {
  if (link.Phase() != PeerLinkPhase::Connected || !link.Mux()) {
    if (on_complete) {
      on_complete(ChannelRoe::error(Failure::Of(Err::AssociationNotReady, "amp link: association not ready")));
    }
    return;
  }
  auto channel_id = link.Mux()->OpenOutbound(protocol_id, policy);
  if (on_complete) {
    if (!channel_id) {
      on_complete(ChannelRoe::error(
          Failure::Of(Err::ChannelOpenFailed, channel_id.error().message)));
    } else {
      on_complete(*channel_id);
    }
  }
}

void PeerLinkManager::OpenChannel(const std::string& peer_key, const std::string& protocol_id, ChannelPolicy policy,
                                  ChannelCb on_complete) {
  EnsureAssociation(peer_key, [this, peer_key, protocol_id, policy = std::move(policy),
                                 on_complete = std::move(on_complete)](LinkRoe assoc) mutable {
    if (!assoc) {
      if (on_complete) {
        on_complete(ChannelRoe::error(assoc.error()));
      }
      return;
    }
    auto* link = FindLink(peer_key);
    if (!link) {
      if (on_complete) {
        on_complete(ChannelRoe::error(Failure::Of(Err::AssociationNotReady, "amp link: association not ready")));
      }
      return;
    }
    OpenChannelOnLink(*link, protocol_id, policy, std::move(on_complete));
  });
}

void PeerLinkManager::SetProtocolHandler(const std::string& protocol_id, ProtocolHandler handler) {
  protocol_handlers_[protocol_id] = std::move(handler);
  for (auto& [_, link] : links_) {
    ApplyProtocolHandlers(*link);
  }
}

void PeerLinkManager::RemoveProtocolHandler(const std::string& protocol_id) {
  protocol_handlers_.erase(protocol_id);
  for (auto& [_, link] : links_) {
    if (link->Mux()) {
      link->Mux()->SetProtocolHandler(protocol_id, {});
    }
  }
}

void PeerLinkManager::ClearProtocolHandlers() {
  protocol_handlers_.clear();
  for (auto& [_, link] : links_) {
    if (link->Mux()) {
      link->Mux()->ClearProtocolHandlers();
    }
  }
}

void PeerLinkManager::ApplyProtocolHandlers(PeerLink& link) {
  if (!link.Mux()) {
    return;
  }
  const std::string peer_key = link.PeerKey();
  link.Mux()->ClearProtocolHandlers();
  for (const auto& [protocol_id, handler] : protocol_handlers_) {
    link.Mux()->SetProtocolHandler(protocol_id, [this, peer_key, handler](const uint32_t channel_id,
                                                                           const std::string&) {
      if (!handler) {
        return;
      }
      if (auto* live = FindLink(peer_key)) {
        handler(*live, channel_id);
      }
    });
  }
}

void PeerLinkManager::FinishDial(const std::string& peer_key, LinkRoe result) {
  if (concurrent_dials_ > 0) {
    --concurrent_dials_;
  }
  if (!result) {
    last_error_[peer_key] = result.error();
    dial_failed_until_[peer_key] = std::chrono::steady_clock::now() + config_.dial_failure_backoff;
    ScheduleDropLink(peer_key);
  } else {
    last_error_.erase(peer_key);
  }

  auto waiters = std::move(inflight_associations_[peer_key]);
  inflight_associations_.erase(peer_key);
  for (auto& waiter : waiters) {
    if (waiter) {
      waiter(result);
    }
  }
}

void PeerLinkManager::OnInboundConnection(std::shared_ptr<adp::Connection> connection) {
  if (links_.size() >= config_.max_links) {
    return;
  }
  std::string peer_key = "inbound:";
  for (size_t i = 0; i < connection->Id().bytes.size(); ++i) {
    peer_key.push_back(static_cast<char>('0' + (connection->Id().bytes[i] >> 4)));
    peer_key.push_back(static_cast<char>('0' + (connection->Id().bytes[i] & 0x0f)));
  }
  if (links_.contains(peer_key)) {
    return;
  }
  auto link = std::make_unique<PeerLink>(peer_key, std::string{}, false, std::move(connection), local_identity_, *this);
  link->StartInboundHandshake({});
  links_[peer_key] = std::move(link);
}

void PeerLinkManager::RekeyLink(const std::string& from_key, const std::string& to_key) {
  if (from_key == to_key) {
    return;
  }
  if (links_.contains(to_key)) {
    auto* occupant = FindLink(to_key);
    // Dual-dial losers stay until Tick; displace non-Connected corpses so inbound can adopt alias.
    if (occupant && occupant->Phase() != PeerLinkPhase::Connected) {
      DropLink(to_key);
    } else {
      return;
    }
  }
  auto node = links_.extract(from_key);
  if (node.empty()) {
    return;
  }
  node.mapped()->SetPeerKey(to_key);
  if (!node.mapped()->RemotePeerId().empty()) {
    peer_id_to_key_[node.mapped()->RemotePeerId()] = to_key;
  }
  auto* link = node.mapped().get();
  links_.emplace(to_key, std::move(node.mapped()));
  // Protocol handlers capture peer_key; refresh after rekey so FindLink succeeds.
  ApplyProtocolHandlers(*link);
}

void PeerLinkManager::StartCapabilityExchange(PeerLink& link) {
  if (!link.Mux()) {
    return;
  }
  const std::string peer_key = link.PeerKey();
  link.Mux()->SetDataHandler(kCapabilityChannelId, [this, peer_key](uint32_t, std::vector<uint8_t> payload) {
    OnCh0Data(peer_key, std::move(payload));
  });

  if (link.CapabilityExchangeStarted()) {
    return;
  }
  link.MarkCapabilityExchangeStarted();

  // Responder completes MSH first; dialer opens ch0 after that so the inbound handler is armed.
  if (!link.IsOutbound()) {
    return;
  }
  link.MarkCapabilityOfferSent();
  (void)ChannelMux::SendCapabilityOffer(*link.Mux(), LocalCapability());
}

void PeerLinkManager::OnCh0Data(const std::string& peer_key, std::vector<uint8_t> payload) {
  if (SessionControlCodec::LooksLike(payload)) {
    if (auto* link = FindLink(peer_key)) {
      link->HandleSessionControl(payload);
    }
    return;
  }
  OnCapabilityData(peer_key, std::move(payload));
}

void PeerLinkManager::OnCapabilityData(const std::string& peer_key, std::vector<uint8_t> payload) {
  auto* link = FindLink(peer_key);
  if (!link || !link->Mux()) {
    return;
  }
  auto decoded = CapabilityCodec::Decode(payload);
  if (!decoded) {
    return;
  }

  const bool first = link->RemoteCapability() == nullptr;
  link->SetRemoteCapability(*decoded);

  // Inbound peer replies once with local caps on the same ch0.
  if (!link->CapabilityOfferSent()) {
    link->MarkCapabilityOfferSent();
    auto encoded = CapabilityCodec::Encode(LocalCapability());
    if (encoded && link->Mux()->State(kCapabilityChannelId) == ChannelState::Open) {
      (void)link->Mux()->SendData(kCapabilityChannelId, std::move(*encoded));
    }
  }

  if (first) {
    IngestRemoteCapabilityAddrs(*link, *decoded);
    if (capability_handler_) {
      if (const auto* remote = link->RemoteCapability()) {
        capability_handler_(*link, *remote);
      }
    }
  }
}

void PeerLinkManager::IngestRemoteCapabilityAddrs(PeerLink& link, const CapabilityPayload& remote) {
  // Trust MSH-authenticated PeerId over self-asserted capability peer id.
  const std::string peer_id = !link.RemotePeerId().empty() ? link.RemotePeerId() : remote.local_peer_id;
  if (peer_id.empty()) {
    return;
  }
  if (!remote.local_peer_id.empty() && remote.local_peer_id != peer_id) {
    // Spoofed identify peer id — still ingest addrs that match the authenticated id.
  }

  for (const auto& ma : remote.listen_multiaddrs) {
    if (ma.empty()) {
      continue;
    }
    auto parsed = ParseAdpMultiaddr(ma);
    if (!parsed) {
      continue;
    }
    if (!parsed->peer_id.empty() && parsed->peer_id != peer_id) {
      continue;
    }

    EndpointRecord rec;
    rec.multiaddr = ma;
    rec.endpoint = parsed->endpoint;
    rec.peer_id = peer_id;
    endpoints_[peer_id] = rec;

    // Refresh dial aliases that already target this PeerId.
    for (auto& [alias, existing] : endpoints_) {
      if (alias != peer_id && existing.peer_id == peer_id) {
        existing.multiaddr = ma;
        existing.endpoint = parsed->endpoint;
      }
    }
    break; // first valid ADP listen addr is preferred for now
  }
}

void PeerLinkManager::MarkWarm(const std::string& peer_key) {
  if (auto* link = FindLink(peer_key)) {
    link->MarkWarm();
  }
}

void PeerLinkManager::MarkHot(const std::string& peer_key) {
  if (auto* link = FindLink(peer_key)) {
    link->MarkHot();
  }
}

void PeerLinkManager::ClearWarm(const std::string& peer_key) {
  if (auto* link = FindLink(peer_key)) {
    link->ClearWarm();
  }
}

void PeerLinkManager::MaybeSendKeepalives(const int64_t now_ms) {
  for (auto& [_, link] : links_) {
    if (link->Phase() != PeerLinkPhase::Connected || link->IsCarrierBacked() || !link->IsOutbound()) {
      continue;
    }
    const auto tier = link->GetKeepaliveTier();
    if (tier == KeepaliveTier::None) {
      continue;
    }
    const int64_t interval_ms = tier == KeepaliveTier::Hot ? config_.keepalive_hot_interval.count()
                                                           : config_.keepalive_warm_interval.count();
    if (interval_ms <= 0) {
      continue;
    }
    if (link->LastKeepaliveTxMs() != 0 && now_ms - link->LastKeepaliveTxMs() < interval_ms) {
      continue;
    }
    (void)link->SendKeepalive(now_ms);
  }
}

void PeerLinkManager::Tick() {
  if (!pending_drop_keys_.empty()) {
    auto pending = std::move(pending_drop_keys_);
    pending_drop_keys_.clear();
    for (const auto& key : pending) {
      DropLink(key);
    }
  }
  if (!pending_alias_adopt_.empty()) {
    auto pending = std::move(pending_alias_adopt_);
    pending_alias_adopt_.clear();
    for (auto& [remote, alias] : pending) {
      if (auto* winner = FindAnyConnectedLinkForRemotePeerId(remote)) {
        if (winner->PeerKey() != alias) {
          RekeyLink(winner->PeerKey(), alias);
        } else {
          peer_id_to_key_[remote] = alias;
        }
      }
    }
  }

  const int64_t now = endpoint_.GetClock().NowMs();
  const int64_t dial_timeout_ms = config_.dial_timeout.count();
  std::vector<std::string> timed_out;
  for (auto& [key, link] : links_) {
    if (link->IsCarrierBacked()) {
      continue;
    }
    const auto phase = link->Phase();
    if (phase != PeerLinkPhase::Handshaking && phase != PeerLinkPhase::Dialing) {
      continue;
    }
    if (link->HandshakeStartedMs() > 0 && now - link->HandshakeStartedMs() > dial_timeout_ms) {
      timed_out.push_back(key);
    }
  }
  for (const auto& key : timed_out) {
    if (auto* link = FindLink(key)) {
      const bool outbound = link->IsOutbound();
      link->FailHandshakeTimeout();
      if (outbound) {
        FinishDial(key, LinkRoe::error(Failure::Of(Err::DialTimeout, "amp link: dial timeout")));
      } else {
        ScheduleDropLink(key);
      }
    }
  }

  std::vector<std::string> evict;
  for (auto& [key, link] : links_) {
    if (link->IsCarrierBacked()) {
      if (link->Carrier() && link->Carrier()->IsClosed() && link->Phase() == PeerLinkPhase::Connected) {
        evict.push_back(key);
      }
      continue;
    }
    auto* conn = link->ConnectionOrNull();
    if (conn && !conn->LooksAlive(now) && !link->IsWarm() && link->Phase() == PeerLinkPhase::Connected) {
      evict.push_back(key);
    }
  }
  for (const auto& key : evict) {
    links_.erase(key);
  }

  MaybeSendKeepalives(now);
}

void PeerLinkManager::EnableNestedCarrierAccept(const bool enable, std::string protocol_id) {
  if (!nested_carrier_protocol_id_.empty()) {
    RemoveProtocolHandler(nested_carrier_protocol_id_);
  }
  nested_carrier_accept_ = enable;
  if (enable) {
    if (protocol_id.empty()) {
      protocol_id = kAmpCircuitCarrierProtocolId;
    }
    nested_carrier_protocol_id_ = std::move(protocol_id);
    SetProtocolHandler(nested_carrier_protocol_id_,
                       [this](PeerLink& link, const uint32_t channel_id) {
                         HandleInboundCarrierChannel(link, channel_id);
                       });
  } else {
    nested_carrier_protocol_id_.clear();
  }
}

void PeerLinkManager::EstablishNestedOverCarrier(const std::string& peer_key,
                                                 std::shared_ptr<ChannelSession> carrier, const bool initiator,
                                                 LinkCb on_complete) {
  if (peer_key.empty() || !carrier) {
    if (on_complete) {
      on_complete(LinkRoe::error(Failure::Of(Err::NestedCarrierIncomplete, "amp link: nested carrier incomplete")));
    }
    return;
  }
  if (IsConnected(peer_key)) {
    if (on_complete) {
      on_complete(LinkRoe());
    }
    return;
  }
  if (auto* existing = FindLink(peer_key)) {
    if (existing->Phase() == PeerLinkPhase::Handshaking) {
      inflight_associations_[peer_key].push_back(std::move(on_complete));
      return;
    }
  }

  if (links_.size() >= config_.max_links) {
    if (on_complete) {
      on_complete(LinkRoe::error(Failure::Of(Err::MaxLinksReached, "amp link: max links reached")));
    }
    return;
  }

  inflight_associations_[peer_key].push_back(std::move(on_complete));
  auto link = std::make_unique<PeerLink>(peer_key, peer_key, initiator, std::move(carrier), local_identity_, *this);
  if (initiator) {
    link->StartOutboundHandshake([this, peer_key](PeerLink::LinkRoe result) {
      FinishNestedCarrier(peer_key, WrapPeerLinkResult(result));
    });
  } else {
    link->StartInboundHandshake([this, peer_key](PeerLink::LinkRoe result) {
      FinishNestedCarrier(peer_key, WrapPeerLinkResult(result));
    });
  }
  links_[peer_key] = std::move(link);
}

void PeerLinkManager::FinishNestedCarrier(const std::string& provisional_key, LinkRoe result) {
  auto* link = FindLink(provisional_key);
  if (result && link && !link->RemotePeerId().empty() && link->RemotePeerId() != provisional_key) {
    PeerLink* adp = nullptr;
    for (auto& [_, other] : links_) {
      if (!other || other.get() == link || other->Phase() != PeerLinkPhase::Connected) {
        continue;
      }
      if (other->RemotePeerId() == link->RemotePeerId() && !other->IsCarrierBacked()) {
        adp = other.get();
        break;
      }
    }
    // Prefer authenticated PeerId as the stable key when unused; keep provisional when ADP owns it ([A024]).
    if (!adp && !links_.contains(link->RemotePeerId())) {
      RekeyLink(provisional_key, link->RemotePeerId());
      link = FindLink(link->RemotePeerId());
    } else if (adp) {
      peer_id_to_key_[link->RemotePeerId()] = adp->PeerKey();
    } else {
      peer_id_to_key_[link->RemotePeerId()] = provisional_key;
    }
  }

  const std::string notify_key = (link ? link->PeerKey() : provisional_key);
  auto waiters = std::move(inflight_associations_[provisional_key]);
  inflight_associations_.erase(provisional_key);
  if (notify_key != provisional_key) {
    auto extras = std::move(inflight_associations_[notify_key]);
    inflight_associations_.erase(notify_key);
    waiters.insert(waiters.end(), std::make_move_iterator(extras.begin()),
                   std::make_move_iterator(extras.end()));
  }
  if (!result) {
    last_error_[provisional_key] = result.error();
    links_.erase(provisional_key);
    if (notify_key != provisional_key) {
      links_.erase(notify_key);
    }
  }
  for (auto& cb : waiters) {
    if (cb) {
      cb(result);
    }
  }
}

void PeerLinkManager::HandleInboundCarrierChannel(PeerLink& via_link, const uint32_t channel_id) {
  if (!nested_carrier_accept_ || !via_link.Mux()) {
    return;
  }
  auto carrier = std::make_shared<ChannelSession>();
  carrier->Bind(*via_link.Mux(), channel_id, CircuitCarrierChannelPolicy(),
                [](Roe<std::vector<uint8_t>>) { return true; });

  // Provisional key until nested MSH authenticates the far peer.
  std::string provisional = "carrier:";
  provisional += via_link.RemotePeerId().empty() ? via_link.PeerKey() : via_link.RemotePeerId();
  provisional.push_back(':');
  provisional += std::to_string(channel_id);
  if (links_.contains(provisional)) {
    provisional += ":";
    provisional += std::to_string(links_.size());
  }

  EstablishNestedOverCarrier(provisional, std::move(carrier), false, {});
}

} // namespace pp::amp
