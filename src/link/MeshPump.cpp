#include "amp/link/MeshPump.h"

namespace pp::amp {

MeshPump::MeshPump(adp::Endpoint& endpoint, PeerLinkManager& links) : endpoint_(endpoint), links_(links) {}

void MeshPump::Pump() {
  // ADP I/O only — PeerLinkManager::Tick is owned by MeshRuntime::TickLocked
  // (exactly once per Drive). Do not call links_.Tick() here.
  endpoint_.Pump();
}

void MeshPump::Tick() {
  endpoint_.Tick();
  links_.Tick();
}

} // namespace pp::amp
