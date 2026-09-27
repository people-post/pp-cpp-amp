#include "amp/link/CarrierLane.h"

#include <algorithm>

namespace pp::amp {

namespace {

/** Selective bit i acks cumulative + 2 + i (cumulative + 1 is the hole that holds it back). */
constexpr uint32_t kSelectiveBits = 64;

} // namespace

CarrierLane::CarrierLane(CarrierLaneConfig config) : config_(config), rto_ms_(config.initial_rto_ms) {}

std::optional<std::vector<uint8_t>> CarrierLane::Wrap(const std::span<const uint8_t> inner, const int64_t now_ms) {
  if (outstanding_.size() >= config_.window) {
    return std::nullopt;
  }
  Outstanding frame;
  frame.seq = next_tx_seq_++;
  frame.wire = AmpAdpCarrier::EncodeLaneData(frame.seq, inner);
  frame.sent_ms = now_ms;
  frame.next_resend_ms = now_ms + rto_ms_;
  outstanding_.push_back(frame);
  return std::move(frame.wire);
}

size_t CarrierLane::Credits() const {
  return outstanding_.size() >= config_.window ? 0 : config_.window - outstanding_.size();
}

void CarrierLane::OnAck(const LaneAckFields& ack, const int64_t now_ms) {
  auto acked = [&](const uint32_t seq) {
    if (seq <= ack.cumulative) {
      return true;
    }
    const uint32_t bit = seq - ack.cumulative - 2;
    return seq >= ack.cumulative + 2 && bit < kSelectiveBits && ((ack.selective >> bit) & 1U) != 0;
  };
  uint32_t highest_acked = ack.cumulative;
  for (uint32_t bit = 0; bit < kSelectiveBits; ++bit) {
    if ((ack.selective >> bit) & 1U) {
      highest_acked = ack.cumulative + 2 + bit;
    }
  }
  for (auto it = outstanding_.begin(); it != outstanding_.end();) {
    if (acked(it->seq)) {
      if (it->attempts == 1) {
        SampleRtt(now_ms - it->sent_ms);  // Karn: resent frames give no sample
      }
      it = outstanding_.erase(it);
      continue;
    }
    // A later frame got through while this one did not: lost or reordered. Resend once the
    // reordering tolerance is spent, without waiting out the RTO.
    if (it->seq < highest_acked && ++it->skips == config_.fast_retransmit_skips) {
      it->next_resend_ms = now_ms;
    }
    ++it;
  }
}

std::vector<std::vector<uint8_t>> CarrierLane::DueResends(const int64_t now_ms) {
  std::vector<std::vector<uint8_t>> out;
  for (auto& frame : outstanding_) {
    if (now_ms < frame.next_resend_ms) {
      continue;
    }
    if (frame.attempts >= config_.max_attempts) {
      failed_ = true;
      continue;
    }
    ++frame.attempts;
    frame.skips = 0;
    frame.next_resend_ms = now_ms + BackoffRto(frame.attempts);
    out.push_back(frame.wire);
  }
  return out;
}

CarrierLane::Received CarrierLane::OnData(const uint32_t seq, const std::span<const uint8_t> inner) {
  Received out;
  if (seq > rx_cumulative_ && !rx_hold_.contains(seq) && seq - rx_cumulative_ <= config_.window) {
    rx_hold_.emplace(seq, std::vector<uint8_t>(inner.begin(), inner.end()));
  }
  for (auto it = rx_hold_.begin(); it != rx_hold_.end() && it->first == rx_cumulative_ + 1;
       it = rx_hold_.erase(it)) {
    out.deliver.push_back(std::move(it->second));
    ++rx_cumulative_;
  }
  out.ack = CurrentAck();
  return out;
}

LaneAckFields CarrierLane::CurrentAck() const {
  LaneAckFields ack;
  ack.cumulative = rx_cumulative_;
  for (const auto& [seq, _] : rx_hold_) {
    const uint32_t bit = seq - rx_cumulative_ - 2;
    if (bit >= kSelectiveBits) {
      break;
    }
    ack.selective |= uint64_t{1} << bit;
  }
  return ack;
}

void CarrierLane::SampleRtt(const int64_t sample_ms) {
  const int64_t sample = std::max<int64_t>(sample_ms, 1);
  if (!have_rtt_) {
    srtt_ms_ = sample;
    rttvar_ms_ = sample / 2;
    have_rtt_ = true;
  } else {
    const int64_t err = srtt_ms_ > sample ? srtt_ms_ - sample : sample - srtt_ms_;
    rttvar_ms_ = (3 * rttvar_ms_ + err) / 4;
    srtt_ms_ = (7 * srtt_ms_ + sample) / 8;
  }
  rto_ms_ = std::clamp(srtt_ms_ + 4 * rttvar_ms_, config_.min_rto_ms, config_.max_rto_ms);
}

int64_t CarrierLane::BackoffRto(const int attempts) const {
  int64_t rto = rto_ms_;
  for (int i = 1; i < attempts && rto < config_.max_rto_ms; ++i) {
    rto *= 2;
  }
  return std::min(rto, config_.max_rto_ms);
}

} // namespace pp::amp
