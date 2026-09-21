#include "amp/link/LinkTable.h"

namespace pp::amp {

PeerLink* LinkTable::FindById(LinkId id) {
  auto it = by_id_.find(id);
  return it == by_id_.end() ? nullptr : it->second;
}

const PeerLink* LinkTable::FindById(LinkId id) const {
  auto it = by_id_.find(id);
  return it == by_id_.end() ? nullptr : it->second;
}

PeerLink* LinkTable::FindByDialKey(const DialKey& key) {
  auto it = legacy_dial_.find(key);
  return it == legacy_dial_.end() ? nullptr : it->second.get();
}

const PeerLink* LinkTable::FindByDialKey(const DialKey& key) const {
  auto it = legacy_dial_.find(key);
  return it == legacy_dial_.end() ? nullptr : it->second.get();
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

PeerLink& LinkTable::Insert(std::unique_ptr<PeerLink> link) {
  const DialKey key = link->PeerKey();
  const LinkId id = AllocId();
  const uint32_t gen = NextGeneration();
  link->SetLinkIdentity(id, gen);
  PeerLink* raw = link.get();
  by_id_[id] = raw;
  by_dial_key_[key] = id;
  legacy_dial_[key] = std::move(link);
  return *raw;
}

void LinkTable::EraseByDialKey(const DialKey& key) {
  auto it = legacy_dial_.find(key);
  if (it == legacy_dial_.end()) {
    return;
  }
  PeerLink* link = it->second.get();
  if (link) {
    by_id_.erase(link->Id());
    if (!link->RemotePeerId().empty()) {
      ClearPresence(link->RemotePeerId(), link->Transport(), link->Id());
    }
  }
  by_dial_key_.erase(key);
  legacy_dial_.erase(it);
}

void LinkTable::EraseById(LinkId id) {
  auto* link = FindById(id);
  if (!link) {
    return;
  }
  EraseByDialKey(link->PeerKey());
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
  if (legacy_dial_.contains(key)) {
    return false;
  }
  auto node = legacy_dial_.extract(from);
  if (node.empty()) {
    return false;
  }
  by_dial_key_.erase(from);
  link->RebindDialKey(key);
  node.key() = key;
  legacy_dial_.insert(std::move(node));
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

void LinkTable::SyncIndexesFromLegacy() {
  by_id_.clear();
  by_dial_key_.clear();
  by_peer_.clear();
  for (auto& [key, link] : legacy_dial_) {
    if (!link) {
      continue;
    }
    if (!link->Id().valid()) {
      link->SetLinkIdentity(AllocId(), NextGeneration());
    }
    by_id_[link->Id()] = link.get();
    by_dial_key_[key] = link->Id();
    if (!link->RemotePeerId().empty() && link->Phase() == PeerLinkPhase::Connected) {
      SetPresence(link->RemotePeerId(), link->Transport(), link->Id());
    }
  }
}

} // namespace pp::amp
