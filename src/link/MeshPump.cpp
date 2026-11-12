#include "amp/link/MeshPump.h"

namespace pp::amp {

MeshPump::MeshPump(adp::Endpoint& endpoint, PeerLinkManager& links) : endpoint_(endpoint), links_(links) {}

size_t MeshPump::Pump() {
  // ADP I/O only — PeerLinkManager::Tick is owned by MeshRuntime::TickLocked
  // (exactly once per Drive). Do not call links_.Tick() here.
  return endpoint_.Pump();
}

void MeshPump::Tick(const bool sweep) {
  endpoint_.Tick(); // resends: only connections with packets in flight, every round
  links_.Tick(sweep);
}

} // namespace pp::amp
