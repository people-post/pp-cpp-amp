#include "amp/L1/MemoryDatagramIo.h"

#include <utility>

namespace pp::adp {

std::shared_ptr<MemoryDatagramHub> MemoryDatagramIo::MakeHub() {
  return std::make_shared<MemoryDatagramHub>();
}

MemoryDatagramIo::MemoryDatagramIo(std::shared_ptr<MemoryDatagramHub> hub, IpEndpoint local)
    : hub_(std::move(hub)), local_(local) {
  hub_->Register(local_, this);
}

MemoryDatagramIo::~MemoryDatagramIo() {
  if (hub_) {
    FlushReorder();
    hub_->Unregister(local_);
  }
}

void MemoryDatagramIo::FlushReorder() {
  while (!pending_reorder_.empty()) {
    auto front = std::move(pending_reorder_.front());
    pending_reorder_.pop_front();
    (void)hub_->Deliver(local_, front.first, std::move(front.second));
  }
}

Roe<void> MemoryDatagramIo::SendTo(const IpEndpoint& peer, std::span<const uint8_t> datagram) {
  if (drop_next_ > 0) {
    --drop_next_;
    return {};
  }
  std::uniform_real_distribution<double> dist(0.0, 1.0);
  if (drop_rate_ > 0 && dist(rng_) < drop_rate_) {
    return {};
  }
  std::vector<uint8_t> copy(datagram.begin(), datagram.end());
  auto deliver = [&](std::vector<uint8_t> buf) {
    if (reorder_window_ > 0) {
      pending_reorder_.emplace_back(peer, std::move(buf));
      if (pending_reorder_.size() > reorder_window_) {
        auto front = std::move(pending_reorder_.front());
        pending_reorder_.pop_front();
        (void)hub_->Deliver(local_, front.first, std::move(front.second));
      }
      return;
    }
    (void)hub_->Deliver(local_, peer, std::move(buf));
  };
  deliver(copy);
  if (dup_rate_ > 0 && dist(rng_) < dup_rate_) {
    deliver(std::vector<uint8_t>(datagram.begin(), datagram.end()));
  }
  return {};
}

Roe<std::optional<std::pair<IpEndpoint, std::vector<uint8_t>>>> MemoryDatagramIo::RecvFrom() {
  // Flush any held reorder buffer occasionally when idle sends stopped — tests call FlushReorder.
  return hub_->Pop(local_);
}

void MemoryDatagramHub::Register(const IpEndpoint& local, MemoryDatagramIo* io) {
  ios_[local] = io;
}

void MemoryDatagramHub::Unregister(const IpEndpoint& local) {
  ios_.erase(local);
  queues_.erase(local);
}

Roe<void> MemoryDatagramHub::Deliver(const IpEndpoint& from, const IpEndpoint& to,
                                     std::vector<uint8_t> datagram) {
  Enqueue(to, from, std::move(datagram));
  return {};
}

void MemoryDatagramHub::Enqueue(const IpEndpoint& to, IpEndpoint from, std::vector<uint8_t> datagram) {
  queues_[to].emplace_back(std::move(from), std::move(datagram));
}

Roe<std::optional<std::pair<IpEndpoint, std::vector<uint8_t>>>> MemoryDatagramHub::Pop(
    const IpEndpoint& local) {
  auto it = queues_.find(local);
  if (it == queues_.end() || it->second.empty()) {
    return std::optional<std::pair<IpEndpoint, std::vector<uint8_t>>>{};
  }
  auto pkt = std::move(it->second.front());
  it->second.pop_front();
  return std::optional<std::pair<IpEndpoint, std::vector<uint8_t>>>{std::move(pkt)};
}

} // namespace pp::adp
