#pragma once

#include "amp/L1/Clock.h"
#include "amp/L1/DatagramIo.h"
#include "amp/L1/Endpoint.h"
#include "amp/link/MeshRuntime.h"
#include "amp/L2/Types.h"


#include <memory>
#include <string>

namespace pp::amp {

/**
 * Owns ADP Endpoint + MeshRuntime for one local peer (D9 composition building block).
 * Product MeshHost may hold an AmpStack in parallel before traffic cutover ([A020]).
 */
class AmpStack {
public:
  struct Config {
    MshIdentity identity;
    std::string local_peer_id;
    PeerLinkConfig link_config;
  };

  static Roe<std::unique_ptr<AmpStack>> Create(std::shared_ptr<adp::DatagramIo> io,
                                               std::shared_ptr<adp::Clock> clock, Config config);

  adp::Endpoint& GetEndpoint() { return *endpoint_; }
  MeshRuntime& Runtime() { return *runtime_; }
  PeerLinkManager& Links() { return runtime_->Links(); }
  const std::string& LocalPeerId() const { return local_peer_id_; }
  adp::IpEndpoint LocalEndpoint() const { return io_->LocalEndpoint(); }

  void Start();
  void Stop();
  bool IsStarted() const { return runtime_ && runtime_->IsStarted(); }

  void Pump();
  void Tick();
  void PostToIo(MeshRuntime::IoTask task);

private:
  AmpStack() = default;

  std::shared_ptr<adp::DatagramIo> io_;
  std::shared_ptr<adp::Clock> clock_;
  std::unique_ptr<adp::Endpoint> endpoint_;
  std::unique_ptr<MeshRuntime> runtime_;
  std::string local_peer_id_;
};

} // namespace pp::amp
