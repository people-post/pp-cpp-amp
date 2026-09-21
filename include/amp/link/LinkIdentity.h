#pragma once

#include "amp/link/Types.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace pp::amp {

/** Opaque stable id for one PeerLink instance (primary LinkTable key). */
struct LinkId {
  uint64_t value = 0;

  bool valid() const { return value != 0; }
  friend bool operator==(LinkId a, LinkId b) { return a.value == b.value; }
  friend bool operator!=(LinkId a, LinkId b) { return !(a == b); }
};

/** Generation-bumping handle — invalid after DropLink of that generation. */
struct LinkHandle {
  LinkId id;
  uint32_t generation = 0;

  bool valid() const { return id.valid() && generation != 0; }
  friend bool operator==(LinkHandle a, LinkHandle b) {
    return a.id == b.id && a.generation == b.generation;
  }
  friend bool operator!=(LinkHandle a, LinkHandle b) { return !(a == b); }
};

enum class TransportClass : uint8_t {
  Adp = 0,
  Carrier = 1,
};

/** A024/A026: at most one Connected ADP + one Connected carrier per PeerId. */
struct PeerPresence {
  std::optional<LinkId> adp;
  std::optional<LinkId> carrier;
};

/** Product association key for RegisterEndpoint / EnsureAssociation. */
using DialKey = std::string;

/** Expanded snapshot for off-strand queries (no live PeerLink*). */
struct LinkSnapshotEx {
  PeerLinkSnapshot base;
  LinkHandle handle;
  DialKey dial_key;
  std::string peer_id;
  TransportClass transport = TransportClass::Adp;
};

} // namespace pp::amp

namespace std {
template <>
struct hash<pp::amp::LinkId> {
  size_t operator()(pp::amp::LinkId id) const noexcept {
    return std::hash<uint64_t>{}(id.value);
  }
};
} // namespace std
