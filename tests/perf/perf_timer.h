#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace pp::amp::perf {

inline int EnvInt(const char* name, const int fallback) {
  const char* v = std::getenv(name);
  if (!v || !*v) {
    return fallback;
  }
  char* end = nullptr;
  const long parsed = std::strtol(v, &end, 10);
  if (end == v || parsed <= 0 || parsed > 1'000'000'000L) {
    return fallback;
  }
  return static_cast<int>(parsed);
}

struct SampleStats {
  double p50_ns = 0;
  double p95_ns = 0;
  double p99_ns = 0;
  double mean_ns = 0;
  double min_ns = 0;
  double max_ns = 0;
  size_t n = 0;
};

inline double PercentileNs(std::vector<double> sorted_ns, const double pct) {
  if (sorted_ns.empty()) {
    return 0;
  }
  if (sorted_ns.size() == 1) {
    return sorted_ns.front();
  }
  const double rank = (pct / 100.0) * static_cast<double>(sorted_ns.size() - 1);
  const size_t lo = static_cast<size_t>(rank);
  const size_t hi = std::min(lo + 1, sorted_ns.size() - 1);
  const double frac = rank - static_cast<double>(lo);
  return sorted_ns[lo] * (1.0 - frac) + sorted_ns[hi] * frac;
}

inline SampleStats Summarize(std::vector<double> samples_ns) {
  SampleStats s;
  s.n = samples_ns.size();
  if (samples_ns.empty()) {
    return s;
  }
  std::sort(samples_ns.begin(), samples_ns.end());
  s.min_ns = samples_ns.front();
  s.max_ns = samples_ns.back();
  s.p50_ns = PercentileNs(samples_ns, 50);
  s.p95_ns = PercentileNs(samples_ns, 95);
  s.p99_ns = PercentileNs(samples_ns, 99);
  double sum = 0;
  for (double v : samples_ns) {
    sum += v;
  }
  s.mean_ns = sum / static_cast<double>(samples_ns.size());
  return s;
}

/** Run `fn` warmup + iters times; return per-iteration durations in nanoseconds. */
template <typename Fn>
std::vector<double> TimeLoop(const int warmup, const int iters, Fn&& fn) {
  for (int i = 0; i < warmup; ++i) {
    fn();
  }
  std::vector<double> samples;
  samples.reserve(static_cast<size_t>(iters));
  for (int i = 0; i < iters; ++i) {
    const auto t0 = std::chrono::steady_clock::now();
    fn();
    const auto t1 = std::chrono::steady_clock::now();
    samples.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
  }
  return samples;
}

inline double OpsPerSec(const double mean_ns) {
  if (!(mean_ns > 0)) {
    return 0;
  }
  return 1e9 / mean_ns;
}

inline double MegaBytesPerSec(const double mean_ns, const size_t bytes) {
  if (!(mean_ns > 0)) {
    return 0;
  }
  return (static_cast<double>(bytes) / mean_ns) * 1e3; // bytes/ns → MB/s
}

} // namespace pp::amp::perf
