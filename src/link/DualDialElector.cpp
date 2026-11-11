#include "amp/link/DualDialElector.h"

namespace pp::amp {

bool DualDialElector::CandidateWins(const Facts& facts) {
  // A silent existing link lost its peer: a stale outbound link kept every redial of a restarted peer out until its
  // liveness window ended (pp-browser app lab caller_restarts_mid_call, 2026-11-11).
  if (facts.existing_silent) {
    return true;
  }
  const bool existing_keep_out = facts.existing_outbound && facts.local_above_remote;
  const bool cand_keep_out = facts.candidate_outbound && facts.local_above_remote;
  if (cand_keep_out && !existing_keep_out) {
    return true;
  }
  if (existing_keep_out) {
    return false;
  }
  if (facts.existing_outbound && !facts.candidate_outbound) {
    return true;
  }
  if (facts.candidate_outbound && !facts.existing_outbound) {
    return false;
  }
  // Both inbound (or both outbound without keep-out): prefer the newer candidate so a redial
  // after silent peer eviction can replace a stale inbound link (LAN B21 / field duplicate ESTABLISH).
  return !facts.existing_outbound && !facts.candidate_outbound;
}

PeerLink* DualDialElector::Elect(PeerLink& existing, PeerLink& candidate, const std::string& local_peer_id,
                                 const int64_t now_ms) {
  const std::string& remote = existing.RemotePeerId().empty() ? candidate.RemotePeerId() : existing.RemotePeerId();
  Facts facts;
  facts.existing_outbound = existing.IsOutbound();
  facts.candidate_outbound = candidate.IsOutbound();
  facts.local_above_remote = !remote.empty() && local_peer_id > remote;
  if (const auto* conn = existing.ConnectionOrNull(); conn && !existing.IsCarrierBacked() && conn->LastAuthRxMs() > 0) {
    facts.existing_silent = now_ms - conn->LastAuthRxMs() > kSilentExistingMs;
  }
  return CandidateWins(facts) ? &candidate : &existing;
}

} // namespace pp::amp
