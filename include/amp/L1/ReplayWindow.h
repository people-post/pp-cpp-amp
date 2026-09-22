#pragma once

#include <cstdint>
#include <unordered_set>

namespace pp::adp {

/** Out-of-order seq acceptance helper for ADP reliable / best-effort demux.
 * Reliable payloads are held and delivered in order by Connection; this window
 * only gates freshness / replay.
 *
 * With `slide_on_gap` (best-effort): a seq more than `window_size` past
 * `last_contiguous_` advances the window instead of rejecting forever. One lost
 * datagram must not pin the receiver and drop every later frame (LAN audio).
 * Reliable keeps the strict window (retransmit fills holes). */
class ReplayWindow {
public:
  explicit ReplayWindow(size_t window_size = 32, bool slide_on_gap = false);

  bool Accept(uint64_t seq);
  uint64_t LastContiguous() const { return last_contiguous_; }

private:
  size_t window_size_;
  bool slide_on_gap_ = false;
  uint64_t last_contiguous_ = 0;
  std::unordered_set<uint64_t> pending_;
};

} // namespace pp::adp
