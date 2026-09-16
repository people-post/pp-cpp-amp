#pragma once

#include "amp/L1/DatagramIo.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <random>
#include <utility>
#include <vector>

namespace pp::adp {

/**
 * Fault-injection decorator over any DatagramIo (Memory or OsUdp).
 * Drop / dup / reorder are applied on SendTo; RecvFrom is pass-through.
 *
 * SetReorderWindow(N): hold up to N datagrams; when a send would exceed N,
 * deliver one randomly chosen buffered datagram. Call FlushReorder() to drain.
 */
class LossyDatagramIo final : public DatagramIo {
public:
  explicit LossyDatagramIo(std::shared_ptr<DatagramIo> inner) : inner_(std::move(inner)) {}

  Roe<void> SendTo(const IpEndpoint& peer, std::span<const uint8_t> datagram) override;
  Roe<std::optional<std::pair<IpEndpoint, std::vector<uint8_t>>>> RecvFrom() override;
  IpEndpoint LocalEndpoint() const override { return inner_->LocalEndpoint(); }

  void SetDropRate(double rate) { drop_rate_ = rate; }
  void SetDupRate(double rate) { dup_rate_ = rate; }
  void SetReorderWindow(size_t n) { reorder_window_ = n; }
  void SetRngSeed(uint32_t seed) { rng_.seed(seed); }
  void DropNext(size_t n) { drop_next_ = n; }
  void FlushReorder();

  DatagramIo& Inner() { return *inner_; }
  const DatagramIo& Inner() const { return *inner_; }

private:
  Roe<void> Deliver(const IpEndpoint& peer, std::vector<uint8_t> buf);

  std::shared_ptr<DatagramIo> inner_;
  double drop_rate_ = 0;
  double dup_rate_ = 0;
  size_t reorder_window_ = 0;
  size_t drop_next_ = 0;
  std::mt19937 rng_{1};
  std::deque<std::pair<IpEndpoint, std::vector<uint8_t>>> pending_reorder_;
};

} // namespace pp::adp
