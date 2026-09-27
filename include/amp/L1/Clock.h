#pragma once

#include <atomic>
#include <cstdint>

namespace pp::adp {

/** Injectable clock — tests Advance; production uses wall time. */
class Clock {
public:
  virtual ~Clock() = default;
  virtual int64_t NowMs() const = 0;
};

class WallClock final : public Clock {
public:
  int64_t NowMs() const override;
};

/** Test clock. Atomic: a test thread advances it while product threads (media capture) read it. */
class VirtualClock final : public Clock {
public:
  explicit VirtualClock(int64_t start_ms = 0) : now_ms_(start_ms) {}
  int64_t NowMs() const override { return now_ms_.load(std::memory_order_acquire); }
  void Advance(int64_t delta_ms) { now_ms_.fetch_add(delta_ms, std::memory_order_acq_rel); }
  void Set(int64_t now_ms) { now_ms_.store(now_ms, std::memory_order_release); }

private:
  std::atomic<int64_t> now_ms_{0};
};

} // namespace pp::adp
