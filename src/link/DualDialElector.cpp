#include "amp/link/DualDialElector.h"

namespace pp::amp {

PeerLink* DualDialElector::Elect(PeerLink& existing, PeerLink& candidate, const std::string& local_peer_id) {
  const std::string& remote = existing.RemotePeerId().empty() ? candidate.RemotePeerId() : existing.RemotePeerId();
  const bool existing_keep_out = existing.IsOutbound() && !remote.empty() && local_peer_id > remote;
  const bool cand_keep_out = candidate.IsOutbound() && !remote.empty() && local_peer_id > remote;
  if (cand_keep_out && !existing_keep_out) {
    return &candidate;
  }
  if (existing_keep_out) {
    return &existing;
  }
  if (existing.IsOutbound() && !candidate.IsOutbound()) {
    return &candidate;
  }
  if (candidate.IsOutbound() && !existing.IsOutbound()) {
    return &existing;
  }
  return &existing;
}

} // namespace pp::amp
