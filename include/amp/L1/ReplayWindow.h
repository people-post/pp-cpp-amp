#pragma once

#include <cstdint>
#include <optional>
#include <unordered_set>

namespace pp::adp {

/** Out-of-order seq acceptance helper for ADP reliable / best-effort demux. */
class ReplayWindow {
public:
  explicit ReplayWindow(size_t window_size = 32);

  bool Accept(uint64_t seq);
  uint64_t LastContiguous() const { return last_contiguous_; }

private:
  size_t window_size_;
  uint64_t last_contiguous_ = 0;
  std::unordered_set<uint64_t> pending_;
};

} // namespace pp::adp
