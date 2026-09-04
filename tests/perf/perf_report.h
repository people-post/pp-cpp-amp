#pragma once

#include "perf_timer.h"

#include <cstdio>
#include <cstddef>

namespace pp::amp::perf {

inline void PrintHuman(const char* case_id, const char* label, const SampleStats& s, const double throughput,
                       const char* throughput_unit) {
  std::printf("%s  %-28s  n=%zu  p50=%.0fns  p95=%.0fns  p99=%.0fns  mean=%.0fns  %s=%.2f\n", case_id, label, s.n,
              s.p50_ns, s.p95_ns, s.p99_ns, s.mean_ns, throughput_unit, throughput);
}

inline void PrintCsv(const char* case_id, const char* label, const size_t payload_bytes, const SampleStats& s,
                     const double throughput, const char* throughput_unit) {
  std::printf("csv,%s,%s,%zu,%zu,%.3f,%.3f,%.3f,%.3f,%.6f,%s\n", case_id, label, payload_bytes, s.n, s.p50_ns, s.p95_ns,
              s.p99_ns, s.mean_ns, throughput, throughput_unit);
}

/** B1 Reliable window + loss; OsUdp loopback; E1 paced 512 KiB; D1 multi-link Drive. */
bool RunB1(int warmup, int iters);
bool RunOsUdp(int warmup, int iters);
bool RunE1(int warmup, int iters);
bool RunD1Multi(int warmup, int drive_iters);

} // namespace pp::amp::perf
