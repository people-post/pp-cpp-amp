#pragma once

#include "amp/link/PeerLink.h"

#include <string>

namespace pp::amp {

/** A026 dual-dial election policy (pure). Strand caller applies ScheduleDrop. */
class DualDialElector {
public:
  /** Prefer outbound when local_peer_id > remote (same glare rule as A021). */
  static PeerLink* Elect(PeerLink& existing, PeerLink& candidate, const std::string& local_peer_id);
};

} // namespace pp::amp
