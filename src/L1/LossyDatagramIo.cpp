#include "amp/L1/LossyDatagramIo.h"

#include <cstddef>
#include <utility>

namespace pp::adp {

void LossyDatagramIo::FlushReorder() {
  while (!pending_reorder_.empty()) {
    auto front = std::move(pending_reorder_.front());
    pending_reorder_.pop_front();
    (void)inner_->SendTo(front.first, front.second);
  }
}

Roe<void> LossyDatagramIo::Deliver(const IpEndpoint& peer, std::vector<uint8_t> buf) {
  if (reorder_window_ > 0) {
    pending_reorder_.emplace_back(peer, std::move(buf));
    if (pending_reorder_.size() > reorder_window_) {
      std::uniform_int_distribution<size_t> pick(0, pending_reorder_.size() - 1);
      const size_t idx = pick(rng_);
      auto chosen = std::move(pending_reorder_[idx]);
      pending_reorder_.erase(pending_reorder_.begin() + static_cast<std::ptrdiff_t>(idx));
      return inner_->SendTo(chosen.first, chosen.second);
    }
    return {};
  }
  return inner_->SendTo(peer, buf);
}

Roe<void> LossyDatagramIo::SendTo(const IpEndpoint& peer, std::span<const uint8_t> datagram) {
  if (drop_next_ > 0) {
    --drop_next_;
    return {};
  }
  std::uniform_real_distribution<double> dist(0.0, 1.0);
  if (drop_rate_ > 0 && dist(rng_) < drop_rate_) {
    return {};
  }
  std::vector<uint8_t> copy(datagram.begin(), datagram.end());
  auto sent = Deliver(peer, copy);
  if (!sent) {
    return sent;
  }
  if (dup_rate_ > 0 && dist(rng_) < dup_rate_) {
    return Deliver(peer, std::vector<uint8_t>(datagram.begin(), datagram.end()));
  }
  return {};
}

Roe<std::optional<std::pair<IpEndpoint, std::vector<uint8_t>>>> LossyDatagramIo::RecvFrom() {
  return inner_->RecvFrom();
}

} // namespace pp::adp
