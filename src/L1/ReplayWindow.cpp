#include "amp/L1/ReplayWindow.h"

namespace pp::adp {

ReplayWindow::ReplayWindow(const size_t window_size, const bool slide_on_gap)
    : window_size_(window_size), slide_on_gap_(slide_on_gap) {}

bool ReplayWindow::Accept(const uint64_t seq) {
  if (seq == 0) {
    return false;
  }
  if (seq <= last_contiguous_) {
    return false;
  }
  if (seq == last_contiguous_ + 1) {
    last_contiguous_ = seq;
    while (pending_.erase(last_contiguous_ + 1) > 0) {
      ++last_contiguous_;
    }
    return true;
  }
  if (seq > last_contiguous_ + window_size_) {
    if (!slide_on_gap_) {
      return false;
    }
    // Best-effort: slide past the hole so forward progress resumes after loss.
    last_contiguous_ = seq - 1;
    pending_.clear();
    last_contiguous_ = seq;
    return true;
  }
  return pending_.insert(seq).second;
}

} // namespace pp::adp
