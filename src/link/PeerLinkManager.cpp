#include "amp/link/PeerLinkManager.h"

#include "amp/link/CodedFailure.h"
#include "amp/L3/ChannelPolicy.h"
#include "amp/L3/ChannelSession.h"
#include "amp/L3/Types.h"
#include "amp/link/AdpMultiaddr.h"
#include "amp/link/DualDialElector.h"
#include "amp/L2/SessionControl.h"
#include "amp/link/Types.h"

#include <algorithm>
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
  // exists — LinkTable Clear order is arbitrary and ~ChannelSession would UAF.
  table_.ForEach([](PeerLink& link) {
    if (link.Carrier()) {
      link.Carrier()->ReleaseHandlers();
    }
  });
  table_.Clear();
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

Roe<void> PeerLinkManager::RegisterEndpoints(const std::string& peer_key,
                                             const std::vector<std::string>& multiaddrs) {
  std::lock_guard lock(strand_mu_);
  return book_.RegisterEndpoints(peer_key, multiaddrs);
}

PeerLink* PeerLinkManager::FindLink(const std::string& peer_key) {
  std::lock_guard lock(strand_mu_);
  if (auto* by_dial = table_.FindByDialKey(peer_key)) {
    return by_dial;
  }
  // Inbound protocol handlers / BindChannel often pass authenticated PeerId, not dial alias.
  return table_.FindByPeerId(peer_key);
}

const PeerLink* PeerLinkManager::FindLink(const std::string& peer_key) const {
  std::lock_guard lock(strand_mu_);
  if (const auto* by_dial = table_.FindByDialKey(peer_key)) {
    return by_dial;
  }
  return table_.FindByPeerId(peer_key);
}

PeerLink* PeerLinkManager::FindLinkByPeerId(const std::string& peer_id) {
  std::lock_guard lock(strand_mu_);
  return table_.FindByPeerId(peer_id);
}

const PeerLink* PeerLinkManager::FindLinkByPeerId(const std::string& peer_id) const {
  std::lock_guard lock(strand_mu_);
  return const_cast<PeerLinkManager*>(this)->FindLinkByPeerId(peer_id);
}

PeerLink* PeerLinkManager::FindConnectedLinkForPeerId(const std::string& peer_id) {
  std::lock_guard lock(strand_mu_);
  auto* link = table_.FindByPeerId(peer_id);
  if (link && link->Phase() == PeerLinkPhase::Connected) {
    return link;
  }
  return nullptr;
}

PeerLink* PeerLinkManager::FindAnyConnectedLinkForRemotePeerId(const std::string& remote_peer_id) {
  std::lock_guard lock(strand_mu_);
  if (remote_peer_id.empty()) {
    return nullptr;
  }
  PeerLink* found = nullptr;
  table_.ForEach([&](PeerLink& link) {
    if (found) {
      return;
    }
    if (link.Phase() == PeerLinkPhase::Connected && link.RemotePeerId() == remote_peer_id) {
      found = &link;
    }
  });
  return found;
}

PeerLink* PeerLinkManager::ElectDualDialWinner(PeerLink& existing, PeerLink& candidate) const {
  std::lock_guard lock(strand_mu_);
  return DualDialElector::Elect(existing, candidate, local_peer_id_);
}

void PeerLinkManager::DropLink(const std::string& peer_key) {
  std::lock_guard lock(strand_mu_);
  auto* link = table_.FindByDialKey(peer_key);
  if (!link) {
    return;
  }
  // Tell the peer to evict too — silent local-only drop left the far side holding a zombie
  // inbound that rejected redial as "ESTABLISH rejected (duplicate)" (LAN B21).
  // Clear OnMessage before Close/erase so Endpoint/Pump cannot UAF into a dying PeerLink
  // (Windows SEH 0xc0000005 in punch blackhole / dial-timeout teardown).
  if (!link->IsCarrierBacked()) {
    if (auto* conn = link->ConnectionOrNull()) {
      conn->OnMessage({});
      if (!conn->IsClosed()) {
        conn->Close();
      }
    }
  }
  if (link->Carrier()) {
    // Nested link: unbind from outer Mux while that Mux is still alive.
    link->Carrier()->ReleaseHandlers();
  }
  ChannelMux* dying_mux = link->Mux();
  if (dying_mux) {
    // Other nested carriers may still point at this Mux — orphan before it dies.
    table_.ForEach([&](PeerLink& other) {
      if (&other == link) {
        return;
      }
      if (other.Carrier() && other.Carrier()->Mux() == dying_mux) {
        other.Carrier()->OrphanFromMux();
      }
    });
    dying_mux->ClearProtocolHandlers();
  }
  table_.EraseByDialKey(peer_key);
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
  table_.ForEach([&](const PeerLink& link) {
    if (link.Phase() == PeerLinkPhase::Connected && link.RemotePeerId() == peer_id) {
      ++n;
    }
  });
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
  ApplyProtocolHandlers(link);
  RefreshPresence(link);
  // Nested carrier links skip ch0 — product reachability already established via outer mesh.
  if (!link.IsCarrierBacked()) {
    StartCapabilityExchange(link);
  }
  const std::string peer_id = link.RemotePeerId();
  if (!peer_id.empty()) {
    std::vector<PeerConnectedListener> listeners;
    {
      // Copy under lock — already hold strand_mu_.
      listeners.reserve(peer_connected_listeners_.size());
      for (const auto& [_, fn] : peer_connected_listeners_) {
        if (fn) {
          listeners.push_back(fn);
        }
      }
    }
    if (!listeners.empty()) {
      // Off establish stack — L4 waiters must not run under association mutation.
      PostCompletion([listeners = std::move(listeners), peer_id]() {
        for (const auto& fn : listeners) {
          fn(peer_id);
        }
      });
    }
  }
  return true;
}

bool PeerLinkManager::AdoptInboundOrDropDuplicate(PeerLink& candidate) {
  std::lock_guard lock(strand_mu_);
  if (candidate.RemotePeerId().empty()) {
    return true;
  }
  const std::string remote = candidate.RemotePeerId();

  // Find another Connected link to the same PeerId (same PeerId may already be present).
  PeerLink* existing = nullptr;
  table_.ForEach([&](PeerLink& link) {
    if (existing || &link == &candidate || link.Phase() != PeerLinkPhase::Connected) {
      return;
    }
    if (link.RemotePeerId() == remote) {
      existing = &link;
    }
  });

  if (!existing) {
    if (!candidate.IsOutbound()) {
      for (const auto& [alias, rec] : book_.Endpoints()) {
        if (rec.peer_id == remote && !table_.ContainsDialKey(alias)) {
          BindDialAlias(candidate.Id(), alias);
          return true;
        }
      }
    }
    return true;
  }

  // [A024] ADP Session and nested carrier Session to the same PeerId coexist.
  if (existing->IsCarrierBacked() != candidate.IsCarrierBacked()) {
    return true;
  }

  // Dual-dial ([A026]): elect one Session per PeerId — reject keep-both.
  PeerLink* winner = ElectDualDialWinner(*existing, candidate);
  PeerLink* loser = (winner == existing) ? &candidate : existing;

  if (loser == &candidate) {
    // Do not erase `candidate` here — PeerLink is still on the stack ([A026]).
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
      if (rec.peer_id == remote && !table_.ContainsDialKey(alias) && candidate.PeerKey() != alias) {
        BindDialAlias(candidate.Id(), alias);
        return true;
      }
    }
  }
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
  PeerLink* found = nullptr;
  table_.ForEach([&](PeerLink& link) {
    if (found) {
      return;
    }
    if (!link.IsOutbound() && link.Phase() == PeerLinkPhase::Connected) {
      found = &link;
    }
  });
  return found;
}

bool PeerLinkManager::IsConnected(const std::string& peer_key) const {
  std::lock_guard lock(strand_mu_);
  const PeerLink* link = table_.FindByDialKey(peer_key);
  if (!link) {
    link = table_.FindByPeerId(peer_key);
  }
  return link && link->Phase() == PeerLinkPhase::Connected;
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
  if (auto* connected = table_.FindByDialKey(peer_key);
      connected && connected->Phase() == PeerLinkPhase::Connected) {
    if (on_complete) {
      on_complete(LinkRoe());
    }
    return;
  }

  const auto ep_it = book_.Endpoints().find(peer_key);
  if (ep_it != book_.Endpoints().end()) {
    // Prefer Connected ADP for this PeerId ([A024]/[A026]); carriers must not satisfy ADP dials.
    if (auto* existing = table_.FindByPeerId(ep_it->second.peer_id, TransportClass::Adp);
        existing && existing->Phase() == PeerLinkPhase::Connected && !existing->IsCarrierBacked()) {
      if (existing->PeerKey() != peer_key) {
        BindDialAlias(existing->Id(), peer_key);
      }
      if (on_complete) {
        on_complete(LinkRoe());
      }
      return;
    }
  }

  if (auto* existing = table_.FindByDialKey(peer_key)) {
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

  if (table_.size() >= book_.Config().max_links) {
    if (on_complete) {
      on_complete(LinkRoe::error(Failure::Of(Err::MaxLinksReached, "amp link: max links reached")));
    }
    return;
  }

  book_.ResetDialIndex(peer_key);
  inflight_associations_[peer_key].push_back(std::move(on_complete));
  BeginOutboundDialLocked(peer_key);
}

void PeerLinkManager::BeginOutboundDialLocked(const std::string& peer_key) {
  const auto ep_it = book_.Endpoints().find(peer_key);
  if (ep_it == book_.Endpoints().end()) {
    auto waiters = std::move(inflight_associations_[peer_key]);
    inflight_associations_.erase(peer_key);
    PostCompletion([waiters = std::move(waiters)]() mutable {
      for (auto& waiter : waiters) {
        if (waiter) {
          waiter(LinkRoe::error(Failure::Of(Err::EndpointNotRegistered, "amp link: peer endpoint not registered")));
        }
      }
    });
    return;
  }

  if (book_.ConcurrentDials() >= book_.Config().max_concurrent_dials) {
    auto waiters = std::move(inflight_associations_[peer_key]);
    inflight_associations_.erase(peer_key);
    PostCompletion([waiters = std::move(waiters)]() mutable {
      for (auto& waiter : waiters) {
        if (waiter) {
          waiter(LinkRoe::error(Failure::Of(Err::TooManyConcurrentDials, "amp link: too many concurrent dials")));
        }
      }
    });
    return;
  }

  if (table_.size() >= book_.Config().max_links) {
    auto waiters = std::move(inflight_associations_[peer_key]);
    inflight_associations_.erase(peer_key);
    PostCompletion([waiters = std::move(waiters)]() mutable {
      for (auto& waiter : waiters) {
        if (waiter) {
          waiter(LinkRoe::error(Failure::Of(Err::MaxLinksReached, "amp link: max links reached")));
        }
      }
    });
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
    auto waiters = std::move(inflight_associations_[peer_key]);
    inflight_associations_.erase(peer_key);
    PostCompletion([waiters = std::move(waiters), msg = opened.error().message]() mutable {
      for (auto& waiter : waiters) {
        if (waiter) {
          waiter(LinkRoe::error(Failure::Of(Err::TransportFailed, msg)));
        }
      }
    });
    return;
  }

  book_.IncConcurrentDials();

  auto link = std::make_unique<PeerLink>(peer_key, ep_it->second.peer_id, true, *opened, local_identity_,
                                         MakeHostPorts());
  AssignLinkIdentity(*link);
  // Insert before StartOutboundHandshake: Start may FailAssociation synchronously (send
  // failure), and FinishDial must FindLink / ScheduleDropLink against a table occupant.
  // Inserting after Start left a zombie link and raced DropLink on Windows (SEH 0xc0000005).
  PeerLink* raw = link.get();
  table_.Insert(std::move(link));
  raw->StartOutboundHandshake([this, peer_key](PeerLink::LinkRoe result) {
    FinishDial(peer_key, WrapPeerLinkResult(result));
  });
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
  table_.ForEach([&](PeerLink& link) { ApplyProtocolHandlers(link); });
}

void PeerLinkManager::RemoveProtocolHandler(const std::string& protocol_id) {
  std::lock_guard lock(strand_mu_);
  protocol_handlers_.erase(protocol_id);
  table_.ForEach([&](PeerLink& link) {
    if (link.Mux()) {
      link.Mux()->SetProtocolHandler(protocol_id, {});
    }
  });
}

void PeerLinkManager::ClearProtocolHandlers() {
  std::lock_guard lock(strand_mu_);
  protocol_handlers_.clear();
  table_.ForEach([&](PeerLink& link) {
    if (link.Mux()) {
      link.Mux()->ClearProtocolHandlers();
    }
  });
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
      // B15/B28: try the next DialBook candidate before arming long backoff / failing waiters.
      // Defer DropLink + redial to Tick — destroying the PeerLink inside its handshake cb segfaults.
      if (!suppress_backoff && book_.AdvanceDialCandidate(peer_key)) {
        ScheduleDropLink(peer_key);
        pending_candidate_retry_.push_back(peer_key);
      } else {
        book_.ResetDialIndex(peer_key);
        if (!suppress_backoff) {
          book_.ArmBackoff(peer_key);
        }
        ScheduleDropLink(peer_key);
        waiters = std::move(inflight_associations_[peer_key]);
        inflight_associations_.erase(peer_key);
      }
    } else {
      last_error_.erase(peer_key);
      if (auto* rec = book_.Find(peer_key)) {
        book_.PromoteDialWinner(peer_key, rec->multiaddr);
      } else {
        book_.ResetDialIndex(peer_key);
      }
      if (auto* link = FindLink(peer_key)) {
        RefreshPresence(*link);
      }
      waiters = std::move(inflight_associations_[peer_key]);
      inflight_associations_.erase(peer_key);
    }
  }
  if (!waiters.empty()) {
    PostCompletion([waiters = std::move(waiters), result]() mutable {
      for (auto& waiter : waiters) {
        if (waiter) {
          waiter(result);
        }
      }
    });
  }
}

void PeerLinkManager::ClearDialBackoff(const std::string& peer_key) {
  std::lock_guard lock(strand_mu_);
  book_.ClearBackoff(peer_key);
}

void PeerLinkManager::AbortInflightDial(const std::string& peer_key) {
  std::lock_guard lock(strand_mu_);
  book_.ClearBackoff(peer_key);
  last_error_.erase(peer_key);
  pending_candidate_retry_.erase(
      std::remove(pending_candidate_retry_.begin(), pending_candidate_retry_.end(), peer_key),
      pending_candidate_retry_.end());

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
  if (table_.size() >= book_.Config().max_links) {
    return;
  }
  std::string peer_key = "inbound:";
  for (size_t i = 0; i < connection->Id().bytes.size(); ++i) {
    peer_key.push_back(static_cast<char>('0' + (connection->Id().bytes[i] >> 4)));
    peer_key.push_back(static_cast<char>('0' + (connection->Id().bytes[i] & 0x0f)));
  }
  if (table_.ContainsDialKey(peer_key)) {
    return;
  }
  auto link = std::make_unique<PeerLink>(peer_key, std::string{}, false, std::move(connection), local_identity_,
                                         MakeHostPorts());
  AssignLinkIdentity(*link);
  PeerLink* raw = link.get();
  table_.Insert(std::move(link));
  raw->StartInboundHandshake({});
}

void PeerLinkManager::BindDialAlias(LinkId id, DialKey to_key) {
  std::lock_guard lock(strand_mu_);
  auto* link = table_.FindById(id);
  if (!link) {
    return;
  }
  const DialKey from_key = link->PeerKey();
  if (from_key == to_key) {
    return;
  }
  if (table_.ContainsDialKey(to_key)) {
    auto* occupant = table_.FindByDialKey(to_key);
    // Dual-dial losers stay until Tick; displace non-Connected corpses so inbound can adopt alias.
    if (occupant && occupant->Phase() != PeerLinkPhase::Connected && occupant->Id() != id) {
      DropLink(to_key);
    } else if (occupant && occupant->Id() != id) {
      return;
    }
  }
  if (!table_.BindDialKey(id, to_key)) {
    return;
  }
  // Protocol handlers capture dial key via HostPorts; refresh after rebind.
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
        capability_handler_(link->Handle(), link->RemotePeerId(), *remote);
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
  table_.ForEach([&](PeerLink& link) {
    if (link.Phase() != PeerLinkPhase::Connected || link.IsCarrierBacked() || !link.IsOutbound()) {
      return;
    }
    const auto tier = link.GetKeepaliveTier();
    if (tier == KeepaliveTier::None) {
      return;
    }
    const int64_t interval_ms = tier == KeepaliveTier::Hot ? book_.Config().keepalive_hot_interval.count()
                                                           : book_.Config().keepalive_warm_interval.count();
    if (interval_ms <= 0) {
      return;
    }
    if (link.LastKeepaliveTxMs() != 0 && now_ms - link.LastKeepaliveTxMs() < interval_ms) {
      return;
    }
    (void)link.SendKeepalive(now_ms);
  });
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
  if (!pending_candidate_retry_.empty()) {
    auto retries = std::move(pending_candidate_retry_);
    pending_candidate_retry_.clear();
    for (const auto& key : retries) {
      if (inflight_associations_.contains(key)) {
        BeginOutboundDialLocked(key);
      }
    }
  }
  if (!pending_alias_adopt_.empty()) {
    auto pending = std::move(pending_alias_adopt_);
    pending_alias_adopt_.clear();
    for (auto& [remote, alias] : pending) {
      if (auto* winner = FindAnyConnectedLinkForRemotePeerId(remote)) {
        if (winner->PeerKey() != alias) {
          BindDialAlias(winner->Id(), alias);
        }
      }
    }
  }

  const int64_t now = endpoint_.GetClock().NowMs();
  const int64_t dial_timeout_ms = book_.Config().dial_timeout.count();
  const int64_t dial_attempt_ms = book_.Config().dial_attempt_timeout.count();
  std::vector<std::string> timed_out;
  table_.ForEach([&](PeerLink& link) {
    if (link.IsCarrierBacked()) {
      return;
    }
    const auto phase = link.Phase();
    if (phase != PeerLinkPhase::Handshaking && phase != PeerLinkPhase::Dialing) {
      return;
    }
    if (link.HandshakeStartedMs() <= 0) {
      return;
    }
    int64_t budget = dial_timeout_ms;
    if (const auto* rec = book_.Find(link.PeerKey());
        rec && rec->dial_index + 1 < rec->candidates.size() && dial_attempt_ms > 0) {
      budget = std::min(budget, dial_attempt_ms);
    }
    if (now - link.HandshakeStartedMs() > budget) {
      timed_out.push_back(link.PeerKey());
    }
  });
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
  table_.ForEach([&](PeerLink& link) {
    if (link.IsCarrierBacked()) {
      if (link.Carrier() && link.Carrier()->IsClosed() && link.Phase() == PeerLinkPhase::Connected) {
        evict.push_back(link.PeerKey());
      }
      return;
    }
    auto* conn = link.ConnectionOrNull();
    // A closed ADP association is dead whatever the keepalive tier (B25): after a network
    // change the link object can stay Connected+Warm while Connection is already closed.
    if (conn && link.Phase() == PeerLinkPhase::Connected &&
        (conn->IsClosed() || (!conn->LooksAlive(now) && !link.IsWarm()))) {
      evict.push_back(link.PeerKey());
    }
  });
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
      auto* link = table_.FindByDialKey(key);
      if (!link) {
        link = table_.FindByPeerId(key);
      }
      bool open = false;
      if (link && link->Mux() && link->Phase() == PeerLinkPhase::Connected && ch != 0) {
        open = link->Mux()->State(ch) == ChannelState::Open;
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

  if (table_.size() >= book_.Config().max_links) {
    if (on_complete) {
      on_complete(LinkRoe::error(Failure::Of(Err::MaxLinksReached, "amp link: max links reached")));
    }
    return;
  }

  inflight_associations_[peer_key].push_back(std::move(on_complete));
  auto link = std::make_unique<PeerLink>(peer_key, peer_key, initiator, std::move(carrier), local_identity_,
                                         MakeHostPorts());
  AssignLinkIdentity(*link);
  PeerLink* raw = link.get();
  table_.Insert(std::move(link));
  if (initiator) {
    raw->StartOutboundHandshake([this, peer_key](PeerLink::LinkRoe result) {
      FinishNestedCarrier(peer_key, WrapPeerLinkResult(result));
    });
  } else {
    raw->StartInboundHandshake([this, peer_key](PeerLink::LinkRoe result) {
      FinishNestedCarrier(peer_key, WrapPeerLinkResult(result));
    });
  }
}

void PeerLinkManager::FinishNestedCarrier(std::string provisional_key, LinkRoe result) {
  std::lock_guard lock(strand_mu_);
  auto* link = FindLink(provisional_key);
  if (result && link && !link->RemotePeerId().empty() && link->RemotePeerId() != provisional_key) {
    PeerLink* adp = nullptr;
    const std::string remote = link->RemotePeerId();
    table_.ForEach([&](PeerLink& other) {
      if (adp || &other == link || other.Phase() != PeerLinkPhase::Connected) {
        return;
      }
      if (other.RemotePeerId() == remote && !other.IsCarrierBacked()) {
        adp = &other;
      }
    });
    // Prefer authenticated PeerId as the stable dial alias when unused; keep provisional when ADP
    // already owns that PeerId key ([A024]).
    if (!adp && !table_.ContainsDialKey(remote)) {
      BindDialAlias(link->Id(), remote);
      link = FindLink(remote);
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
    // Defer DropLink (mux/handler/orphan cleanup, never raw erase — [A027]) to Tick: we are inside
    // the dying link's establish_cb_ (often via its carrier's closed callback), so a synchronous
    // drop frees the running lambda, the PeerLink and the carrier handler (dogfood SIGSEGV).
    ScheduleDropLink(provisional_key);
    if (notify_key != provisional_key) {
      ScheduleDropLink(notify_key);
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
  if (table_.ContainsDialKey(provisional)) {
    provisional += ":";
    provisional += std::to_string(table_.size());
  }

  EstablishNestedOverCarrier(provisional, std::move(carrier), false, {});
}

size_t PeerLinkManager::CountLinks() const {
  std::lock_guard lock(strand_mu_);
  return table_.size();
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

PeerLinkManager::PeerConnectedListenerId PeerLinkManager::AddPeerConnectedListener(
    PeerConnectedListener listener) {
  std::lock_guard lock(strand_mu_);
  const auto id = next_peer_connected_listener_id_.fetch_add(1, std::memory_order_relaxed);
  if (listener) {
    peer_connected_listeners_[id] = std::move(listener);
  }
  return id;
}

void PeerLinkManager::RemovePeerConnectedListener(const PeerConnectedListenerId id) {
  std::lock_guard lock(strand_mu_);
  peer_connected_listeners_.erase(id);
}

void PeerLinkManager::SetPeerConnectedListener(PeerConnectedListener listener) {
  std::lock_guard lock(strand_mu_);
  peer_connected_listeners_.clear();
  if (listener) {
    const auto id = next_peer_connected_listener_id_.fetch_add(1, std::memory_order_relaxed);
    peer_connected_listeners_[id] = std::move(listener);
  }
}

void PeerLinkManager::ClearPeerConnectedListeners() {
  std::lock_guard lock(strand_mu_);
  peer_connected_listeners_.clear();
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
    if (auto* link = table_.FindByPeerId(peer_id)) {
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
  std::lock_guard lock(strand_mu_);
  if (peer_id.empty()) {
    return false;
  }
  // ADP only — carrier presence must not satisfy EnsureAssociation / BurstDial wins (A024).
  const auto presence = table_.Presence(peer_id);
  if (!presence.adp) {
    return false;
  }
  auto* link = table_.FindById(*presence.adp);
  return link && link->Phase() == PeerLinkPhase::Connected && !link->IsCarrierBacked();
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
  auto* link = table_.FindByPeerId(peer_id);
  return link && link->Phase() == PeerLinkPhase::Connected;
}

void PeerLinkManager::WhenChannelOpen(const DialKey& peer_key, uint32_t channel_id, int64_t deadline_ms,
                                      std::function<void(bool ok)> done) {
  std::lock_guard lock(strand_mu_);
  // deadline_ms is absolute Amp clock (Endpoint::GetClock().NowMs()). Prefer WhenChannelOpenIn
  // when converting from a steady_clock wall deadline — never pass steady epoch ms here.
  channel_open_waiters_.emplace_back(peer_key, channel_id, deadline_ms, std::move(done));
}

void PeerLinkManager::WhenChannelOpenIn(const DialKey& peer_key, uint32_t channel_id,
                                        std::chrono::milliseconds remaining,
                                        std::function<void(bool ok)> done) {
  const int64_t now = endpoint_.GetClock().NowMs();
  const int64_t rem = remaining.count() < 0 ? 0 : remaining.count();
  WhenChannelOpen(peer_key, channel_id, now + rem, std::move(done));
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
