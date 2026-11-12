#pragma once

#include "amp/L1/Endpoint.h"
#include "amp/link/PeerLinkManager.h"

namespace pp::amp {

/**
 * ADP Endpoint pump/tick adapter.
 * Pump() is ADP-only; PeerLinkManager::Tick runs solely from MeshRuntime::TickLocked
 * so each Drive executes link housekeeping exactly once.
 */
class MeshPump {
public:
  MeshPump(adp::Endpoint& endpoint, PeerLinkManager& links);

  /** Datagrams handled. */
  size_t Pump();
  /** Housekeeping: resends every call; with `sweep` also the walk over every link (PeerLinkManager::Tick). */
  void Tick(bool sweep = true);

private:
  adp::Endpoint& endpoint_;
  PeerLinkManager& links_;
};

} // namespace pp::amp
