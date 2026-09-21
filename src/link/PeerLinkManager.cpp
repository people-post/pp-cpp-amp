#include "amp/link/PeerLinkManager.h"

#include "amp/link/CodedFailure.h"
#include "amp/L3/ChannelPolicy.h"
#include "amp/L3/ChannelSession.h"
#include "amp/L3/Types.h"
#include "amp/link/AdpMultiaddr.h"
#include "amp/link/DualDialElector.h"
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
      book_(std::move(config)), strand_mu_(owned_mu_) {
  endpoint_.SetAcceptKey(PreSessionPeerKey());
  InstallAcceptHandler();
}

PeerLinkManager::PeerLinkManager(adp::Endpoint& endpoint, MshIdentity local_identity, std::string local_peer_id,
                                 PeerLinkConfig config, std::recursive_mutex& strand_mu)
    : endpoint_(endpoint), local_identity_(std::move(local_identity)), local_peer_id_(std::move(local_peer_id)),
      book_(std::move(config)), strand_mu_(strand_mu) {
  endpoint_.SetAcceptKey(PreSessionPeerKey());
  InstallAcceptHandler();
}

PeerLinkManager::~PeerLinkManager() {
  std::lock_guard lock(strand_mu_);
  // Nested carriers Bind to an outer link's Mux ([A024]). Unbind while every Mux still
  // exists — unordered_map destroy order is arbitrary and ~ChannelSession would UAF.
  for (auto& [_, link] : table_.LegacyDialMap()) {
    if (link && link->Carrier()) {
      link->Carrier()->ReleaseHandlers();
    }
  }
  table_.LegacyDialMap().clear();
  table_.SyncIndexesFromLegacy();
}

void PeerLinkManager::SetLocalListenMultiaddrs(std::vector<std::string> multiaddrs) {
  std::lock_guard lock(strand_mu_);
  local_listen_multiaddrs_ = std::move(multiaddrs);
}

void PeerLinkManager::SetAdvertisedProtocols(std::vector<std::string> protocols) {
  std::lock_guard lock(strand_mu_);
  advertised_protocols_ = std::move(protocols);
}

void PeerLinkManager::SetCapabilityHandler(CapabilityHandler handler) {
  std::lock_guard lock(strand_mu_);
  capability_handler_ = std::move(handler);
}

CapabilityPayload PeerLinkManager::LocalCapability() const {
  std::lock_guard lock(strand_mu_);
  CapabilityPayload payload;
  payload.local_peer_id = local_peer_id_;
  payload.listen_multiaddrs = local_listen_multiaddrs_;
  payload.protocols = advertised_protocols_;
  return payload;
}

std::optional<std::string> PeerLinkManager::PreferredMultiaddr(const std::string& peer_id) const {
  std::lock_guard lock(strand_mu_);
  if (peer_id.empty()) {
    return std::nullopt;
  }
  if (const auto it = book_.Endpoints().find(peer_id); it != book_.Endpoints().end()) {
    return it->second.multiaddr;
  }
  for (const auto& [_, rec] : book_.Endpoints()) {
    if (rec.peer_id == peer_id && !rec.multiaddr.empty()) {
      return rec.multiaddr;
    }
  }
  return std::nullopt;
}

void PeerLinkManager::InstallAcceptHandler() {
  endpoint_.SetAcceptHandler([this](std::shared_ptr<adp::Connection> connection) {
    std::lock_guard lock(strand_mu_);
    OnInboundConnection(std::move(connection));
  });
}

Roe<void> PeerLinkManager::RegisterEndpoint(const std::string& peer_key, const std::string& multiaddr) {
  std::lock_guard lock(strand_mu_);
  return book_.RegisterEndpoint(peer_key, multiaddr);
}

PeerLink* PeerLinkManager::FindLink(const std::string& peer_key) {
  std::lock_guard lock(strand_mu_);
  auto it = table_.LegacyDialMap().find(peer_key);
  if (it == table_.LegacyDialMap().end()) {
    return nullptr;
  }
  return it->second.get();
}

const PeerLink* PeerLinkManager::FindLink(const std::string& peer_key) const {
  std::lock_guard lock(strand_mu_);
  auto it = table_.LegacyDialMap().find(peer_key);
  if (it == table_.LegacyDialMap().end()) {
    return nullptr;
  }
  return it->second.get();
}

PeerLink* PeerLinkManager::FindLinkByPeerId(const std::string& peer_id) {
  std::lock_guard lock(strand_mu_);
  if (peer_id.empty()) {
    return nullptr;
  }
  if (const auto it = peer_id_to_key_.find(peer_id); it != peer_id_to_key_.end()) {
    return FindLink(it->second);
  }
  for (auto& [key, link] : table_.LegacyDialMap()) {
    if (link->Phase() == PeerLinkPhase::Connected && link->RemotePeerId() == peer_id) {
      peer_id_to_key_[peer_id] = key;
      return link.get();
    }
  }
  return nullptr;
}

const PeerLink* PeerLinkManager::FindLinkByPeerId(const std::string& peer_id) const {
  std::lock_guard lock(strand_mu_);
  return const_cast<PeerLinkManager*>(this)->FindLinkByPeerId(peer_id);
}

PeerLink* PeerLinkManager::FindConnectedLinkForPeerId(const std::string& peer_id) {
  std::lock_guard lock(strand_mu_);
  if (auto* link = FindLinkByPeerId(peer_id)) {
    if (link->Phase() == PeerLinkPhase::Connected) {
      return link;
    }
  }
  return nullptr;
}

PeerLink* PeerLinkManager::FindAnyConnectedLinkForRemotePeerId(const std::string& remote_peer_id) {
  std::lock_guard lock(strand_mu_);
  if (remote_peer_id.empty()) {
    return nullptr;
  }
  for (auto& [_, link] : table_.LegacyDialMap()) {
    if (link && link->Phase() == PeerLinkPhase::Connected && link->RemotePeerId() == remote_peer_id) {
      return link.get();
    }
  }
  return nullptr;
}

PeerLink* PeerLinkManager::ElectDualDialWinner(PeerLink& existing, PeerLink& candidate) const {
  std::lock_guard lock(strand_mu_);
  return DualDialElector::Elect(existing, candidate, local_peer_id_);
}

void PeerLinkManager::DropLink(const std::string& peer_key) {
  std::lock_guard lock(strand_mu_);
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
    for (auto& [_, other] : table_.LegacyDialMap()) {
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
  const LinkId id = link->Id();
  const TransportClass transport = link->Transport();
  table_.LegacyDialMap().erase(peer_key);
  if (id.valid()) {
    table_.ClearPresence(remote, transport, id);
  }
  if (!remote.empty()) {
    if (auto it = peer_id_to_key_.find(remote); it != peer_id_to_key_.end() && it->second == peer_key) {
      peer_id_to_key_.erase(it);
    }
  }
  table_.SyncIndexesFromLegacy();
}

void PeerLinkManager::ScheduleDropLink(std::string peer_key) {
  std::lock_guard lock(strand_mu_);
  pending_drop_keys_.push_back(std::move(peer_key));
}

void PeerLinkManager::ScheduleAdoptDialAlias(std::string remote_peer_id, std::string dial_alias) {
  std::lock_guard lock(strand_mu_);
  if (remote_peer_id.empty() || dial_alias.empty()) {
    return;
  }
  pending_alias_adopt_.emplace_back(std::move(remote_peer_id), std::move(dial_alias));
}

size_t PeerLinkManager::CountConnectedLinksForPeerId(const std::string& peer_id) const {
  std::lock_guard lock(strand_mu_);
  if (peer_id.empty()) {
    return 0;
  }
  size_t n = 0;
  for (const auto& [_, link] : table_.LegacyDialMap()) {
    if (link && link->Phase() == PeerLinkPhase::Connected && link->RemotePeerId() == peer_id) {
      ++n;
    }
  }
  return n;
}

bool PeerLinkManager::OnLinkEstablished(PeerLink& link) {
  std::lock_guard lock(strand_mu_);
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
  RefreshPresence(link);
  // Nested carrier links skip ch0 — product reachability already established via outer mesh.
  if (!link.IsCarrierBacked()) {
    StartCapabilityExchange(link);
  }
  return true;
}

bool PeerLinkManager::AdoptInboundOrDropDuplicate(PeerLink& candidate) {
  std::lock_guard lock(strand_mu_);
  if (candidate.RemotePeerId().empty()) {
    return true;
  }
  const std::string remote = candidate.RemotePeerId();

  // Find another Connected link to the same PeerId (map may still point at candidate).
  PeerLink* existing = nullptr;
  for (auto& [key, link] : table_.LegacyDialMap()) {
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
      for (const auto& [alias, rec] : book_.Endpoints()) {
        if (rec.peer_id == remote && !table_.LegacyDialMap().contains(alias)) {
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
  loser->DemoteForScheduledDrop();
  ScheduleDropLink(loser->PeerKey());
  // Winner is the new candidate — prefer dial alias when free.
  if (!candidate.IsOutbound()) {
    for (const auto& [alias, rec] : book_.Endpoints()) {
      if (rec.peer_id == remote && !table_.LegacyDialMap().contains(alias) && candidate.PeerKey() != alias) {
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
  std::lock_guard lock(strand_mu_);
  if (book_.Config().peer_id_from_identity) {
    return book_.Config().peer_id_from_identity(identity_public_key);
  }
  return IdentityPublicKeyFingerprint(identity_public_key);
}

PeerLink* PeerLinkManager::FindConnectedInboundLink() {
  std::lock_guard lock(strand_mu_);
  for (auto& [_, link] : table_.LegacyDialMap()) {
    if (!link->IsOutbound() && link->Phase() == PeerLinkPhase::Connected) {
      return link.get();
    }
  }
  return nullptr;
}

bool PeerLinkManager::IsConnected(const std::string& peer_key) const {
  std::lock_guard lock(strand_mu_);
  if (const auto* link = FindLink(peer_key)) {
    return link->Phase() == PeerLinkPhase::Connected;
  }
  return false;
}

PeerLinkSnapshot PeerLinkManager::GetLinkSnapshot(const std::string& peer_key) const {
  std::lock_guard lock(strand_mu_);
  PeerLinkSnapshot snap;
  snap.has_endpoint = book_.Contains(peer_key);
  if (const auto* link = FindLink(peer_key); link && link->Phase() == PeerLinkPhase::Connected) {
    snap.phase = PeerLinkPhase::Connected;
    snap.carrier_backed = link->IsCarrierBacked();
    if (snap.has_endpoint) {
      snap.multiaddr = book_.Endpoints().at(peer_key).multiaddr;
    }
    return snap;
  }
  if (!snap.has_endpoint) {
    snap.phase = PeerLinkPhase::Unavailable;
    return snap;
  }
  snap.multiaddr = book_.Endpoints().at(peer_key).multiaddr;
  if (const auto* link = FindLink(peer_key)) {
    snap.phase = link->Phase();
  } else if (auto remaining = book_.BackoffRemaining(peer_key, std::chrono::steady_clock::now())) {
    snap.phase = PeerLinkPhase::Backoff;
    snap.backoff_remaining = *remaining;
  } else {
    snap.phase = PeerLinkPhase::Idle;
  }
  if (const auto err = last_error_.find(peer_key); err != last_error_.end()) {
    snap.detail = err->second.message;
  }
  return snap;
}

void PeerLinkManager::EnsureAssociation(const std::string& peer_key, LinkCb on_complete) {
  std::lock_guard lock(strand_mu_);
  if (IsConnected(peer_key)) {
    if (on_complete) {
      on_complete(LinkRoe());
    }
    return;
  }

  const auto ep_it = book_.Endpoints().find(peer_key);
  if (ep_it != book_.Endpoints().end()) {
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
    // Stale Backoff/Idle occupant blocks a fresh dial; drop and continue.
    if (existing->Phase() != PeerLinkPhase::Connected) {
      DropLink(peer_key);
    }
  }

  if (ep_it == book_.Endpoints().end()) {
    if (on_complete) {
      on_complete(LinkRoe::error(Failure::Of(Err::EndpointNotRegistered, "amp link: peer endpoint not registered")));
    }
    return;
  }

  const auto now = std::chrono::steady_clock::now();
  if (book_.InBackoff(peer_key, now)) {
    if (on_complete) {
      on_complete(LinkRoe::error(Failure::Of(Err::DialInBackoff, "amp link: dial in backoff")));
    }
    return;
  }
  book_.EraseExpiredBackoff(peer_key, now);

  if (book_.ConcurrentDials() >= book_.Config().max_concurrent_dials) {
    if (on_complete) {
      on_complete(LinkRoe::error(Failure::Of(Err::TooManyConcurrentDials, "amp link: too many concurrent dials")));
    }
    return;
  }

  if (table_.LegacyDialMap().size() >= book_.Config().max_links) {
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
  // Same-ms mint collision (pre-seq fix) or rare id clash — remint once.
  if (!opened && opened.error().message.find("assoc already open") != std::string::npos) {
    params.id = {};
    params.mint_id = true;
    opened = endpoint_.Open(params);
  }
  if (!opened) {
    if (on_complete) {
      on_complete(LinkRoe::error(Failure::Of(Err::TransportFailed, opened.error().message)));
    }
    return;
  }

  book_.IncConcurrentDials();
  inflight_associations_[peer_key].push_back(std::move(on_complete));

  auto link = std::make_unique<PeerLink>(peer_key, ep_it->second.peer_id, true, *opened, local_identity_,
                                         MakeHostPorts());
  link->StartOutboundHandshake([this, peer_key](PeerLink::LinkRoe result) {
    FinishDial(peer_key, WrapPeerLinkResult(result));
  });
  AssignLinkIdentity(*link);
  table_.LegacyDialMap()[peer_key] = std::move(link);
  table_.SyncIndexesFromLegacy();
}

void PeerLinkManager::OpenChannelOnLink(PeerLink& link, const std::string& protocol_id, ChannelPolicy policy,
                                        ChannelCb on_complete) {
  std::lock_guard lock(strand_mu_);
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
  std::lock_guard lock(strand_mu_);
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
  std::lock_guard lock(strand_mu_);
  protocol_handlers_[protocol_id] = std::move(handler);
  for (auto& [_, link] : table_.LegacyDialMap()) {
    ApplyProtocolHandlers(*link);
  }
}

void PeerLinkManager::RemoveProtocolHandler(const std::string& protocol_id) {
  std::lock_guard lock(strand_mu_);
  protocol_handlers_.erase(protocol_id);
  for (auto& [_, link] : table_.LegacyDialMap()) {
    if (link->Mux()) {
      link->Mux()->SetProtocolHandler(protocol_id, {});
    }
  }
}

void PeerLinkManager::ClearProtocolHandlers() {
  std::lock_guard lock(strand_mu_);
  protocol_handlers_.clear();
  for (auto& [_, link] : table_.LegacyDialMap()) {
    if (link->Mux()) {
      link->Mux()->ClearProtocolHandlers();
    }
  }
}

void PeerLinkManager::ApplyProtocolHandlers(PeerLink& link) {
  std::lock_guard lock(strand_mu_);
  if (!link.Mux()) {
    return;
  }
  if (!link.Id().valid()) {
    AssignLinkIdentity(link);
  }
  const auto handle = link.Handle();
  const std::string remote = link.RemotePeerId();
  link.Mux()->ClearProtocolHandlers();
  for (const auto& [protocol_id, handler] : protocol_handlers_) {
    link.Mux()->SetProtocolHandler(protocol_id, [this, handle, remote, handler](const uint32_t channel_id,
                                                                                const std::string&) {
      if (!handler) {
        return;
      }
      if (table_.FindLive(handle)) {
        handler(handle, remote, channel_id);
      }
    });
  }
}

void PeerLinkManager::FinishDial(const std::string& peer_key, LinkRoe result) {
  std::vector<LinkCb> waiters;
  {
    std::lock_guard lock(strand_mu_);
    book_.DecConcurrentDials();
    const bool suppress_backoff = suppress_dial_backoff_.erase(peer_key) > 0;
    if (!result) {
      last_error_[peer_key] = result.error();
      if (!suppress_backoff) {
        book_.ArmBackoff(peer_key);
      }
      ScheduleDropLink(peer_key);
    } else {
      last_error_.erase(peer_key);
      if (auto* link = FindLink(peer_key)) {
        RefreshPresence(*link);
      }
    }

    waiters = std::move(inflight_associations_[peer_key]);
    inflight_associations_.erase(peer_key);
  }
  PostCompletion([waiters = std::move(waiters), result]() mutable {
    for (auto& waiter : waiters) {
      if (waiter) {
        waiter(result);
      }
    }
  });
}

void PeerLinkManager::ClearDialBackoff(const std::string& peer_key) {
  std::lock_guard lock(strand_mu_);
  book_.ClearBackoff(peer_key);
}

void PeerLinkManager::AbortInflightDial(const std::string& peer_key) {
  std::lock_guard lock(strand_mu_);
  book_.ClearBackoff(peer_key);
  last_error_.erase(peer_key);

  std::vector<LinkCb> waiters;
  if (auto it = inflight_associations_.find(peer_key); it != inflight_associations_.end()) {
    waiters = std::move(it->second);
    inflight_associations_.erase(it);
  }
  suppress_dial_backoff_.insert(peer_key);

  // FailAssociation → establish_cb → FinishDial (ScheduleDropLink, no backoff). Do not
  // DropLink/ScheduleDrop here: destroying a Handshaking PeerLink races the handshake path.
  if (auto* link = FindLink(peer_key)) {
    const auto phase = link->Phase();
    if (link->IsOutbound() &&
        (phase == PeerLinkPhase::Handshaking || phase == PeerLinkPhase::Dialing)) {
      link->FailHandshakeTimeout();
    } else if (phase != PeerLinkPhase::Connected) {
      ScheduleDropLink(peer_key);
      suppress_dial_backoff_.erase(peer_key);
    } else {
      suppress_dial_backoff_.erase(peer_key);
    }
  } else {
    suppress_dial_backoff_.erase(peer_key);
  }

  const auto aborted = LinkRoe::error(Failure::Of(Err::Generic, "amp link: dial aborted"));
  PostCompletion([waiters = std::move(waiters), aborted]() mutable {
    for (auto& waiter : waiters) {
      if (waiter) {
        waiter(aborted);
      }
    }
  });
}

void PeerLinkManager::OnInboundConnection(std::shared_ptr<adp::Connection> connection) {
  std::lock_guard lock(strand_mu_);
  if (table_.LegacyDialMap().size() >= book_.Config().max_links) {
    return;
  }
  std::string peer_key = "inbound:";
  for (size_t i = 0; i < connection->Id().bytes.size(); ++i) {
    peer_key.push_back(static_cast<char>('0' + (connection->Id().bytes[i] >> 4)));
    peer_key.push_back(static_cast<char>('0' + (connection->Id().bytes[i] & 0x0f)));
  }
  if (table_.LegacyDialMap().contains(peer_key)) {
    return;
  }
  auto link = std::make_unique<PeerLink>(peer_key, std::string{}, false, std::move(connection), local_identity_,
                                         MakeHostPorts());
  AssignLinkIdentity(*link);
  link->StartInboundHandshake({});
  table_.LegacyDialMap()[peer_key] = std::move(link);
  table_.SyncIndexesFromLegacy();
}

void PeerLinkManager::RekeyLink(const std::string& from_key, const std::string& to_key) {
  std::lock_guard lock(strand_mu_);
  if (from_key == to_key) {
    return;
  }
  if (table_.LegacyDialMap().contains(to_key)) {
    auto* occupant = FindLink(to_key);
    // Dual-dial losers stay until Tick; displace non-Connected corpses so inbound can adopt alias.
    if (occupant && occupant->Phase() != PeerLinkPhase::Connected) {
      DropLink(to_key);
    } else {
      return;
    }
  }
  auto node = table_.LegacyDialMap().extract(from_key);
  if (node.empty()) {
    return;
  }
  node.mapped()->RebindDialKey(to_key);
  if (!node.mapped()->RemotePeerId().empty()) {
    peer_id_to_key_[node.mapped()->RemotePeerId()] = to_key;
  }
  auto* link = node.mapped().get();
  table_.LegacyDialMap().emplace(to_key, std::move(node.mapped()));
  // Protocol handlers capture peer_key; refresh after rekey so FindLink succeeds.
  ApplyProtocolHandlers(*link);
}

void PeerLinkManager::StartCapabilityExchange(PeerLink& link) {
  std::lock_guard lock(strand_mu_);
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
  std::lock_guard lock(strand_mu_);
  if (SessionControlCodec::LooksLike(payload)) {
    if (auto* link = FindLink(peer_key)) {
      link->HandleSessionControl(payload);
    }
    return;
  }
  OnCapabilityData(peer_key, std::move(payload));
}

void PeerLinkManager::OnCapabilityData(const std::string& peer_key, std::vector<uint8_t> payload) {
  std::lock_guard lock(strand_mu_);
  auto* link = FindLink(peer_key);
  if (!link || !link->Mux()) {
    return;
  }
  auto decoded = CapabilityCodec::Decode(payload);
  if (!decoded) {
    return;
  }

  const bool first = link->RemoteCapability() == nullptr;
  link->ApplyRemoteCapability(*decoded);

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
  std::lock_guard lock(strand_mu_);
  const std::string peer_id = !link.RemotePeerId().empty() ? link.RemotePeerId() : remote.local_peer_id;
  if (peer_id.empty()) {
    return;
  }
  book_.IngestRemoteAddrs(peer_id, remote.listen_multiaddrs);
}

void PeerLinkManager::MarkWarm(const std::string& peer_key) {
  std::lock_guard lock(strand_mu_);
  if (auto* link = FindLink(peer_key)) {
    link->MarkWarm();
  }
}

void PeerLinkManager::MarkHot(const std::string& peer_key) {
  std::lock_guard lock(strand_mu_);
  if (auto* link = FindLink(peer_key)) {
    link->MarkHot();
  }
}

void PeerLinkManager::ClearWarm(const std::string& peer_key) {
  std::lock_guard lock(strand_mu_);
  if (auto* link = FindLink(peer_key)) {
    link->ClearWarm();
  }
}

void PeerLinkManager::MaybeSendKeepalives(const int64_t now_ms) {
  std::lock_guard lock(strand_mu_);
  for (auto& [_, link] : table_.LegacyDialMap()) {
    if (link->Phase() != PeerLinkPhase::Connected || link->IsCarrierBacked() || !link->IsOutbound()) {
      continue;
    }
    const auto tier = link->GetKeepaliveTier();
    if (tier == KeepaliveTier::None) {
      continue;
    }
    const int64_t interval_ms = tier == KeepaliveTier::Hot ? book_.Config().keepalive_hot_interval.count()
                                                           : book_.Config().keepalive_warm_interval.count();
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
  std::lock_guard lock(strand_mu_);
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
  const int64_t dial_timeout_ms = book_.Config().dial_timeout.count();
  std::vector<std::string> timed_out;
  for (auto& [key, link] : table_.LegacyDialMap()) {
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
      // FailHandshakeTimeout → establish_cb → FinishDial for outbound; do not FinishDial twice.
      if (link->IsOutbound()) {
        link->FailHandshakeTimeout();
      } else {
        link->FailHandshakeTimeout();
        ScheduleDropLink(key);
      }
    }
  }

  std::vector<std::string> evict;
  for (auto& [key, link] : table_.LegacyDialMap()) {
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
    // DropLink (not raw erase) so Mux/handlers/peer_id maps stay consistent — raw erase after a
    // concurrent dial left MeshRuntime Io racing a half-dead hop (dirty-book StartBridge SIGSEGV).
    DropLink(key);
  }

  // WhenChannelOpen waiters (product H1/H2 replacement).
  if (!channel_open_waiters_.empty()) {
    std::vector<std::tuple<DialKey, uint32_t, int64_t, std::function<void(bool)>>> remaining;
    std::vector<std::function<void()>> completions;
    for (auto& [key, ch, deadline, done] : channel_open_waiters_) {
      auto* link = FindLink(key);
      bool open = false;
      if (link && link->Mux()) {
        // Channel is "open" once mux has the channel id registered as open outbound/inbound.
        // Best-effort: treat Connected link + valid mux as ready when channel_id is non-zero
        // and OpenChannel already succeeded (caller polls after OpenChannel).
        open = link->Phase() == PeerLinkPhase::Connected && ch != 0;
      }
      if (open) {
        if (done) {
          completions.push_back([done = std::move(done)]() mutable { done(true); });
        }
      } else if (deadline > 0 && now >= deadline) {
        if (done) {
          completions.push_back([done = std::move(done)]() mutable { done(false); });
        }
      } else {
        remaining.emplace_back(std::move(key), ch, deadline, std::move(done));
      }
    }
    channel_open_waiters_ = std::move(remaining);
    for (auto& c : completions) {
      PostCompletion(std::move(c));
    }
  }

  MaybeSendKeepalives(now);
}

void PeerLinkManager::EnableNestedCarrierAccept(const bool enable, std::string protocol_id) {
  std::lock_guard lock(strand_mu_);
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
                       [this](LinkHandle handle, const std::string& /*remote*/, const uint32_t channel_id) {
                         WithLiveLink(handle, [this, channel_id](PeerLink& link) {
                           HandleInboundCarrierChannel(link, channel_id);
                         });
                       });
  } else {
    nested_carrier_protocol_id_.clear();
  }
}

void PeerLinkManager::EstablishNestedOverCarrier(const std::string& peer_key,
                                                 std::shared_ptr<ChannelSession> carrier, const bool initiator,
                                                 LinkCb on_complete) {
  std::lock_guard lock(strand_mu_);
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

  if (table_.LegacyDialMap().size() >= book_.Config().max_links) {
    if (on_complete) {
      on_complete(LinkRoe::error(Failure::Of(Err::MaxLinksReached, "amp link: max links reached")));
    }
    return;
  }

  inflight_associations_[peer_key].push_back(std::move(on_complete));
  auto link = std::make_unique<PeerLink>(peer_key, peer_key, initiator, std::move(carrier), local_identity_,
                                         MakeHostPorts());
  if (initiator) {
    link->StartOutboundHandshake([this, peer_key](PeerLink::LinkRoe result) {
      FinishNestedCarrier(peer_key, WrapPeerLinkResult(result));
    });
  } else {
    link->StartInboundHandshake([this, peer_key](PeerLink::LinkRoe result) {
      FinishNestedCarrier(peer_key, WrapPeerLinkResult(result));
    });
  }
  table_.LegacyDialMap()[peer_key] = std::move(link);
}

void PeerLinkManager::FinishNestedCarrier(const std::string& provisional_key, LinkRoe result) {
  std::lock_guard lock(strand_mu_);
  auto* link = FindLink(provisional_key);
  if (result && link && !link->RemotePeerId().empty() && link->RemotePeerId() != provisional_key) {
    PeerLink* adp = nullptr;
    for (auto& [_, other] : table_.LegacyDialMap()) {
      if (!other || other.get() == link || other->Phase() != PeerLinkPhase::Connected) {
        continue;
      }
      if (other->RemotePeerId() == link->RemotePeerId() && !other->IsCarrierBacked()) {
        adp = other.get();
        break;
      }
    }
    // Prefer authenticated PeerId as the stable key when unused; keep provisional when ADP owns it ([A024]).
    if (!adp && !table_.LegacyDialMap().contains(link->RemotePeerId())) {
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
    // Always DropLink (mux/handler/orphan cleanup) — never raw erase ([A027]).
    DropLink(provisional_key);
    if (notify_key != provisional_key) {
      DropLink(notify_key);
    }
  }
  for (auto& cb : waiters) {
    if (cb) {
      cb(result);
    }
  }
}

void PeerLinkManager::HandleInboundCarrierChannel(PeerLink& via_link, const uint32_t channel_id) {
  std::lock_guard lock(strand_mu_);
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
  if (table_.LegacyDialMap().contains(provisional)) {
    provisional += ":";
    provisional += std::to_string(table_.LegacyDialMap().size());
  }

  EstablishNestedOverCarrier(provisional, std::move(carrier), false, {});
}

size_t PeerLinkManager::CountLinks() const {
  std::lock_guard lock(strand_mu_);
  return table_.LegacyDialMap().size();
}

PeerLinkHostPorts PeerLinkManager::MakeHostPorts() {
  return PeerLinkHostPorts{
      .now_ms = [this]() { return endpoint_.GetClock().NowMs(); },
      .derive_peer_id = [this](const ByteVector& pk) { return DeriveRemotePeerId(pk); },
      .on_established = [this](PeerLink& link) { return OnLinkEstablished(link); },
      .schedule_drop = [this](std::string key) { ScheduleDropLink(std::move(key)); },
      .schedule_adopt_alias =
          [this](std::string remote, std::string alias) {
            ScheduleAdoptDialAlias(std::move(remote), std::move(alias));
          },
      .has_other_connected =
          [this](const std::string& remote) {
            return FindAnyConnectedLinkForRemotePeerId(remote) != nullptr;
          },
  };
}

void PeerLinkManager::SetCompletionPoster(CompletionPoster poster) {
  std::lock_guard lock(strand_mu_);
  completion_poster_ = std::move(poster);
}

void PeerLinkManager::PostCompletion(std::function<void()> fn) {
  if (!fn) {
    return;
  }
  if (completion_poster_) {
    completion_poster_(std::move(fn));
    return;
  }
  // Standalone tests: run inline (still after release when caller unlocked).
  fn();
}

void PeerLinkManager::AssignLinkIdentity(PeerLink& link) {
  if (!link.Id().valid()) {
    link.SetLinkIdentity(table_.AllocId(), table_.NextGeneration());
  }
}

void PeerLinkManager::RefreshPresence(PeerLink& link) {
  if (link.RemotePeerId().empty() || link.Phase() != PeerLinkPhase::Connected) {
    return;
  }
  if (!link.Id().valid()) {
    AssignLinkIdentity(link);
  }
  table_.SetPresence(link.RemotePeerId(), link.Transport(), link.Id());
  // Prefer ADP for peer_id_to_key_ compatibility index.
  if (!link.IsCarrierBacked()) {
    peer_id_to_key_[link.RemotePeerId()] = link.PeerKey();
  } else if (!peer_id_to_key_.contains(link.RemotePeerId())) {
    peer_id_to_key_[link.RemotePeerId()] = link.PeerKey();
  }
}

LinkSnapshotEx PeerLinkManager::SnapshotOf(const PeerLink* link, const DialKey& dial_key, bool has_endpoint,
                                           const std::string& multiaddr) const {
  LinkSnapshotEx out;
  out.dial_key = dial_key;
  out.base.has_endpoint = has_endpoint;
  out.base.multiaddr = multiaddr;
  if (!link) {
    out.base.phase = has_endpoint ? PeerLinkPhase::Idle : PeerLinkPhase::Unavailable;
    return out;
  }
  out.handle = link->Handle();
  out.peer_id = link->RemotePeerId();
  out.transport = link->Transport();
  out.base.phase = link->Phase();
  out.base.carrier_backed = link->IsCarrierBacked();
  return out;
}

LinkSnapshotEx PeerLinkManager::GetSnapshotByDialKey(const DialKey& key) const {
  std::lock_guard lock(strand_mu_);
  const auto* ep = book_.Find(key);
  const auto* link = FindLink(key);
  return SnapshotOf(link, key, ep != nullptr, ep ? ep->multiaddr : std::string{});
}

LinkSnapshotEx PeerLinkManager::GetSnapshotByPeerId(const std::string& peer_id, TransportClass prefer) const {
  std::lock_guard lock(strand_mu_);
  LinkSnapshotEx empty;
  empty.peer_id = peer_id;
  if (peer_id.empty()) {
    return empty;
  }
  const auto presence = table_.Presence(peer_id);
  LinkId id;
  if (prefer == TransportClass::Carrier && presence.carrier) {
    id = *presence.carrier;
  } else if (presence.adp) {
    id = *presence.adp;
  } else if (presence.carrier) {
    id = *presence.carrier;
  }
  if (!id.valid()) {
    // Fall back to peer_id_to_key_ / scan for pre-sync links.
    if (auto* link = const_cast<PeerLinkManager*>(this)->FindLinkByPeerId(peer_id)) {
      const auto* ep = book_.Find(link->PeerKey());
      return SnapshotOf(link, link->PeerKey(), ep != nullptr, ep ? ep->multiaddr : std::string{});
    }
    return empty;
  }
  auto* link = table_.FindById(id);
  if (!link) {
    return empty;
  }
  const auto* ep = book_.Find(link->PeerKey());
  return SnapshotOf(link, link->PeerKey(), ep != nullptr, ep ? ep->multiaddr : std::string{});
}

bool PeerLinkManager::IsConnectedToPeerId(const std::string& peer_id) const {
  auto snap = GetSnapshotByPeerId(peer_id, TransportClass::Adp);
  return snap.base.phase == PeerLinkPhase::Connected;
}

bool PeerLinkManager::IsReachable(const std::string& peer_id) const {
  std::lock_guard lock(strand_mu_);
  if (peer_id.empty()) {
    return false;
  }
  const auto presence = table_.Presence(peer_id);
  auto live = [&](const std::optional<LinkId>& id) {
    if (!id) {
      return false;
    }
    auto* link = table_.FindById(*id);
    return link && link->Phase() == PeerLinkPhase::Connected;
  };
  if (live(presence.adp) || live(presence.carrier)) {
    return true;
  }
  return const_cast<PeerLinkManager*>(this)->FindLinkByPeerId(peer_id) != nullptr &&
         const_cast<PeerLinkManager*>(this)->FindLinkByPeerId(peer_id)->Phase() == PeerLinkPhase::Connected;
}

void PeerLinkManager::WhenChannelOpen(const DialKey& peer_key, uint32_t channel_id, int64_t deadline_ms,
                                      std::function<void(bool ok)> done) {
  std::lock_guard lock(strand_mu_);
  channel_open_waiters_.emplace_back(peer_key, channel_id, deadline_ms, std::move(done));
}

std::shared_ptr<ChannelSession> PeerLinkManager::BindChannel(const DialKey& peer_key, uint32_t channel_id,
                                                             ChannelPolicy policy,
                                                             ChannelSession::FrameHandler on_frame,
                                                             ChannelSession::ClosedCallback on_closed) {
  std::lock_guard lock(strand_mu_);
  auto* link = FindLink(peer_key);
  if (!link || link->Phase() != PeerLinkPhase::Connected || !link->Mux()) {
    return {};
  }
  auto session = std::make_shared<ChannelSession>();
  session->Bind(*link->Mux(), channel_id, std::move(policy), std::move(on_frame), std::move(on_closed));
  return session;
}

} // namespace pp::amp
