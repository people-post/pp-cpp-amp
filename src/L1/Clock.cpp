#include "amp/L1/Clock.h"

#include <chrono>

namespace pp::adp {

int64_t WallClock::NowMs() const {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

} // namespace pp::adp
