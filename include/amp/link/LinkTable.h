#pragma once

#include "amp/link/LinkIdentity.h"
#include "amp/link/PeerLink.h"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

namespace pp::amp {

/**
 * Owns PeerLink instances keyed by LinkId.
 * DialKey and PeerPresence are secondary indexes (ADR_LINK_PLANE) — rebinding a dial
 * alias does not move the unique_ptr (no map-key surgery / RekeyLink).
 * Strand-affine — caller holds MeshRuntime io lock.
 */
class LinkTable {
public:
  LinkId AllocId() { return LinkId{++next_id_}; }
  uint32_t NextGeneration() { return ++next_generation_; }

  PeerLink* FindById(LinkId id);
  const PeerLink* FindById(LinkId id) const;
  PeerLink* FindByDialKey(const DialKey& key);
  const PeerLink* FindByDialKey(const DialKey& key) const;
  PeerLink* FindLive(LinkHandle handle);
  const PeerLink* FindLive(LinkHandle handle) const;

  /** Prefer Connected ADP, else Connected carrier, for PeerId. */
  PeerLink* FindByPeerId(const std::string& peer_id, TransportClass prefer = TransportClass::Adp);
  const PeerLink* FindByPeerId(const std::string& peer_id,
                               TransportClass prefer = TransportClass::Adp) const;

  bool ContainsDialKey(const DialKey& key) const { return by_dial_key_.contains(key); }

  /** Install newly constructed link; assigns LinkId/generation; indexes PeerKey(). */
  PeerLink& Insert(std::unique_ptr<PeerLink> link);
  void EraseByDialKey(const DialKey& key);
  void EraseById(LinkId id);

  /**
   * Rebind dial alias for an existing LinkId — updates PeerLink::PeerKey and by_dial_key_
   * only. Fails if `key` is already bound to a different live link.
   */
  bool BindDialKey(LinkId id, DialKey key);
  void UnbindDialKey(const DialKey& key);

  void SetPresence(const std::string& peer_id, TransportClass transport, LinkId id);
  void ClearPresence(const std::string& peer_id, TransportClass transport, LinkId id);
  PeerPresence Presence(const std::string& peer_id) const;

  size_t size() const { return by_id_.size(); }
  bool empty() const { return by_id_.empty(); }
  void Clear() {
    by_id_.clear();
    by_dial_key_.clear();
    by_peer_.clear();
  }

  template <typename Fn>
  void ForEach(Fn&& fn) {
    for (auto& [_, link] : by_id_) {
      if (link) {
        fn(*link);
      }
    }
  }
  template <typename Fn>
  void ForEach(Fn&& fn) const {
    for (const auto& [_, link] : by_id_) {
      if (link) {
        fn(*link);
      }
    }
  }

private:
  uint64_t next_id_ = 0;
  uint32_t next_generation_ = 0;
  std::unordered_map<LinkId, std::unique_ptr<PeerLink>> by_id_;
  std::unordered_map<DialKey, LinkId> by_dial_key_;
  std::unordered_map<std::string, PeerPresence> by_peer_;
};

} // namespace pp::amp
