#pragma once

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

class VirtualClock final : public Clock {
public:
  explicit VirtualClock(int64_t start_ms = 0) : now_ms_(start_ms) {}
  int64_t NowMs() const override { return now_ms_; }
  void Advance(int64_t delta_ms) { now_ms_ += delta_ms; }
  void Set(int64_t now_ms) { now_ms_ = now_ms; }

private:
  int64_t now_ms_ = 0;
};

} // namespace pp::adp
