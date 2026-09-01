#include "amp/L1/ReplayWindow.h"

namespace pp::adp {

ReplayWindow::ReplayWindow(const size_t window_size) : window_size_(window_size) {}

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
    return false;
  }
  return pending_.insert(seq).second;
}

} // namespace pp::adp
