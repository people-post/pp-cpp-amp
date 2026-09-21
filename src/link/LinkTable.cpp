#include "amp/link/LinkTable.h"

namespace pp::amp {

PeerLink* LinkTable::FindById(LinkId id) {
  auto it = by_id_.find(id);
  return it == by_id_.end() ? nullptr : it->second.get();
}

const PeerLink* LinkTable::FindById(LinkId id) const {
  auto it = by_id_.find(id);
  return it == by_id_.end() ? nullptr : it->second.get();
}

PeerLink* LinkTable::FindByDialKey(const DialKey& key) {
  auto it = by_dial_key_.find(key);
  if (it == by_dial_key_.end()) {
    return nullptr;
  }
  return FindById(it->second);
}

const PeerLink* LinkTable::FindByDialKey(const DialKey& key) const {
  return const_cast<LinkTable*>(this)->FindByDialKey(key);
}

PeerLink* LinkTable::FindLive(LinkHandle handle) {
  auto* link = FindById(handle.id);
  if (!link || link->Generation() != handle.generation) {
    return nullptr;
  }
  return link;
}

const PeerLink* LinkTable::FindLive(LinkHandle handle) const {
  return const_cast<LinkTable*>(this)->FindLive(handle);
}

PeerLink* LinkTable::FindByPeerId(const std::string& peer_id, TransportClass prefer) {
  if (peer_id.empty()) {
    return nullptr;
  }
  const auto presence = Presence(peer_id);
  auto pick = [&](const std::optional<LinkId>& id) -> PeerLink* {
    if (!id) {
      return nullptr;
    }
    auto* link = FindById(*id);
    if (link && link->Phase() == PeerLinkPhase::Connected) {
      return link;
    }
    return nullptr;
  };
  if (prefer == TransportClass::Carrier) {
    if (auto* c = pick(presence.carrier)) {
      return c;
    }
    if (auto* a = pick(presence.adp)) {
      return a;
    }
  } else {
    if (auto* a = pick(presence.adp)) {
      return a;
    }
    if (auto* c = pick(presence.carrier)) {
      return c;
    }
  }
  // Fallback scan (presence not yet refreshed).
  PeerLink* found = nullptr;
  ForEach([&](PeerLink& link) {
    if (found) {
      return;
    }
    if (link.Phase() == PeerLinkPhase::Connected && link.RemotePeerId() == peer_id) {
      found = &link;
    }
  });
  return found;
}

const PeerLink* LinkTable::FindByPeerId(const std::string& peer_id, TransportClass prefer) const {
  return const_cast<LinkTable*>(this)->FindByPeerId(peer_id, prefer);
}

PeerLink& LinkTable::Insert(std::unique_ptr<PeerLink> link) {
  const DialKey key = link->PeerKey();
  if (!link->Id().valid()) {
    link->SetLinkIdentity(AllocId(), NextGeneration());
  }
  const LinkId id = link->Id();
  PeerLink* raw = link.get();
  by_id_[id] = std::move(link);
  by_dial_key_[key] = id;
  return *raw;
}

void LinkTable::EraseById(LinkId id) {
  auto it = by_id_.find(id);
  if (it == by_id_.end() || !it->second) {
    return;
  }
  PeerLink& link = *it->second;
  by_dial_key_.erase(link.PeerKey());
  if (!link.RemotePeerId().empty()) {
    ClearPresence(link.RemotePeerId(), link.Transport(), id);
  }
  by_id_.erase(it);
}

void LinkTable::EraseByDialKey(const DialKey& key) {
  auto it = by_dial_key_.find(key);
  if (it == by_dial_key_.end()) {
    return;
  }
  EraseById(it->second);
}

bool LinkTable::BindDialKey(LinkId id, DialKey key) {
  auto* link = FindById(id);
  if (!link) {
    return false;
  }
  const DialKey from = link->PeerKey();
  if (from == key) {
    by_dial_key_[key] = id;
    return true;
  }
  if (auto existing = by_dial_key_.find(key); existing != by_dial_key_.end() && existing->second != id) {
    return false;
  }
  // Only drop the old index when it still names this LinkId (avoid erasing a rebound alias).
  if (auto it = by_dial_key_.find(from); it != by_dial_key_.end() && it->second == id) {
    by_dial_key_.erase(it);
  }
  link->RebindDialKey(key);
  by_dial_key_[key] = id;
  return true;
}

void LinkTable::UnbindDialKey(const DialKey& key) { by_dial_key_.erase(key); }

void LinkTable::SetPresence(const std::string& peer_id, TransportClass transport, LinkId id) {
  if (peer_id.empty() || !id.valid()) {
    return;
  }
  auto& p = by_peer_[peer_id];
  if (transport == TransportClass::Carrier) {
    p.carrier = id;
  } else {
    p.adp = id;
  }
}

void LinkTable::ClearPresence(const std::string& peer_id, TransportClass transport, LinkId id) {
  auto it = by_peer_.find(peer_id);
  if (it == by_peer_.end()) {
    return;
  }
  if (transport == TransportClass::Carrier) {
    if (it->second.carrier && *it->second.carrier == id) {
      it->second.carrier.reset();
    }
  } else if (it->second.adp && *it->second.adp == id) {
    it->second.adp.reset();
  }
  if (!it->second.adp && !it->second.carrier) {
    by_peer_.erase(it);
  }
}

PeerPresence LinkTable::Presence(const std::string& peer_id) const {
  auto it = by_peer_.find(peer_id);
  return it == by_peer_.end() ? PeerPresence{} : it->second;
}

} // namespace pp::amp
