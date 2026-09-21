#pragma once

#include "amp/link/LinkIdentity.h"
#include "amp/link/PeerLink.h"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pp::amp {

/**
 * Owns PeerLink instances keyed by LinkId with DialKey + PeerPresence secondary indexes.
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

  /** Install newly constructed link; assigns LinkId/generation. */
  PeerLink& Insert(std::unique_ptr<PeerLink> link);
  void EraseByDialKey(const DialKey& key);
  void EraseById(LinkId id);

  /** Bind dial alias without moving the unique_ptr (index rewrite). */
  bool BindDialKey(LinkId id, DialKey key);
  void UnbindDialKey(const DialKey& key);

  void SetPresence(const std::string& peer_id, TransportClass transport, LinkId id);
  void ClearPresence(const std::string& peer_id, TransportClass transport, LinkId id);
  PeerPresence Presence(const std::string& peer_id) const;

  size_t size() const { return by_id_.size(); }
  bool empty() const { return by_id_.empty(); }

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

  /** Temporary bridge: dial-key map used by legacy callers during migration. */
  std::unordered_map<DialKey, std::unique_ptr<PeerLink>>& LegacyDialMap() { return legacy_dial_; }
  const std::unordered_map<DialKey, std::unique_ptr<PeerLink>>& LegacyDialMap() const {
    return legacy_dial_;
  }

  void SyncIndexesFromLegacy();

private:
  uint64_t next_id_ = 0;
  uint32_t next_generation_ = 0;
  std::unordered_map<LinkId, PeerLink*> by_id_;
  std::unordered_map<DialKey, LinkId> by_dial_key_;
  std::unordered_map<std::string, PeerPresence> by_peer_;
  /** Still owns unique_ptrs under dial key until full LinkId ownership lands. */
  std::unordered_map<DialKey, std::unique_ptr<PeerLink>> legacy_dial_;
};

} // namespace pp::amp
