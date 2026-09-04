#pragma once

#include "amp/L1/DatagramIo.h"

#include <deque>
#include <memory>
#include <random>
#include <unordered_map>
#include <vector>

namespace pp::adp {

/**
 * In-process datagram fabric for tests. Endpoints share a Hub; optional
 * loss/reorder/dup injection is applied on SendTo.
 *
 * SetReorderWindow(N): hold up to N datagrams; when a send would exceed N,
 * deliver one randomly chosen buffered datagram (true reorder). Call
 * FlushReorder() to drain any remainder (FIFO).
 */
class MemoryDatagramHub;

class MemoryDatagramIo final : public DatagramIo {
public:
  static std::shared_ptr<MemoryDatagramHub> MakeHub();

  MemoryDatagramIo(std::shared_ptr<MemoryDatagramHub> hub, IpEndpoint local);

  Roe<void> SendTo(const IpEndpoint& peer, std::span<const uint8_t> datagram) override;
  Roe<std::optional<std::pair<IpEndpoint, std::vector<uint8_t>>>> RecvFrom() override;
  IpEndpoint LocalEndpoint() const override { return local_; }

  void SetDropRate(double rate) { drop_rate_ = rate; }
  void SetDupRate(double rate) { dup_rate_ = rate; }
  void SetReorderWindow(size_t n) { reorder_window_ = n; }
  void SetRngSeed(uint32_t seed) { rng_.seed(seed); }

  /** Force next N sends to be dropped (after which drop_rate applies). */
  void DropNext(size_t n) { drop_next_ = n; }

  /** Deliver any datagrams held for reorder (FIFO drain). */
  void FlushReorder();

  ~MemoryDatagramIo() override;

private:
  std::shared_ptr<MemoryDatagramHub> hub_;
  IpEndpoint local_;
  double drop_rate_ = 0;
  double dup_rate_ = 0;
  size_t reorder_window_ = 0;
  size_t drop_next_ = 0;
  std::mt19937 rng_{1};
  std::deque<std::pair<IpEndpoint, std::vector<uint8_t>>> pending_reorder_;
};

class MemoryDatagramHub {
public:
  void Register(const IpEndpoint& local, MemoryDatagramIo* io);
  void Unregister(const IpEndpoint& local);
  Roe<void> Deliver(const IpEndpoint& from, const IpEndpoint& to, std::vector<uint8_t> datagram);

  void Enqueue(const IpEndpoint& to, IpEndpoint from, std::vector<uint8_t> datagram);
  Roe<std::optional<std::pair<IpEndpoint, std::vector<uint8_t>>>> Pop(const IpEndpoint& local);

private:
  std::unordered_map<IpEndpoint, MemoryDatagramIo*, IpEndpointHash> ios_;
  std::unordered_map<IpEndpoint, std::deque<std::pair<IpEndpoint, std::vector<uint8_t>>>, IpEndpointHash>
      queues_;
};

} // namespace pp::adp
