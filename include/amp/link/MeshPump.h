#pragma once

#include "amp/L1/Endpoint.h"
#include "amp/link/PeerLinkManager.h"

namespace pp::amp {

/** Io-thread driver: ADP Endpoint pump/tick for PeerLinkManager links. */
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
