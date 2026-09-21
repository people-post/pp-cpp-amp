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

  void Pump();
  void Tick();

private:
  adp::Endpoint& endpoint_;
  PeerLinkManager& links_;
};

} // namespace pp::amp
