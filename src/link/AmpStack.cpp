#include "amp/link/AmpStack.h"

namespace pp::amp {

Roe<std::unique_ptr<AmpStack>> AmpStack::Create(std::shared_ptr<adp::DatagramIo> io,
                                                std::shared_ptr<adp::Clock> clock, Config config) {
  if (!io || !clock) {
    return Error("amp stack: io and clock required");
  }
  if (config.local_peer_id.empty()) {
    return Error("amp stack: local_peer_id required");
  }
  auto stack = std::unique_ptr<AmpStack>(new AmpStack());
  stack->io_ = std::move(io);
  stack->clock_ = std::move(clock);
  stack->local_peer_id_ = std::move(config.local_peer_id);
  stack->endpoint_ = std::make_unique<adp::Endpoint>(stack->io_, stack->clock_);
  stack->runtime_ = std::make_unique<MeshRuntime>(*stack->endpoint_, std::move(config.identity),
                                                  stack->local_peer_id_, std::move(config.link_config));
  return stack;
}

void AmpStack::Start() {
  if (runtime_) {
    runtime_->Start();
  }
}

void AmpStack::Stop() {
  if (runtime_) {
    runtime_->Stop();
  }
}

void AmpStack::Pump() {
  if (runtime_) {
    runtime_->Pump();
  }
}

void AmpStack::Tick() {
  if (runtime_) {
    runtime_->Tick();
  }
}

void AmpStack::PostToIo(MeshRuntime::IoTask task) {
  if (runtime_) {
    runtime_->PostToIo(std::move(task));
  }
}

} // namespace pp::amp
