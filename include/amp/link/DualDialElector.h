#pragma once

#include "amp/link/PeerLink.h"

#include <cstdint>
#include <string>

namespace pp::amp {

/** A026 dual-dial election policy (pure). Strand caller applies ScheduleDrop. */
class DualDialElector {
public:
  /**
   * An existing link that received nothing authenticated for this long is not in a glare with a new one: the peer
   * that dialed again has lost it (restarted, a new socket), so its new session wins. Glare is two links set up at
   * once, both alive; the keep-outbound rule below is for that alone.
   */
  static constexpr int64_t kSilentExistingMs = 2000;

  /** What decides an election between two Connected links to one PeerId. */
  struct Facts {
    bool existing_outbound = false;
    bool candidate_outbound = false;
    /** The existing link received nothing authenticated for kSilentExistingMs. */
    bool existing_silent = false;
    /** Our PeerId sorts above the remote's (A021 glare rule: the higher keeps its outbound link). */
    bool local_above_remote = false;
  };
  /** True: the candidate wins and the existing link is dropped. */
  static bool CandidateWins(const Facts& facts);

  /** Prefer outbound when local_peer_id > remote (same glare rule as A021), unless the existing link is silent. */
  static PeerLink* Elect(PeerLink& existing, PeerLink& candidate, const std::string& local_peer_id, int64_t now_ms);
};

} // namespace pp::amp
