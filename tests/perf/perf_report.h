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
bool RunB2(int warmup, int iters);
bool RunB3(int warmup, int iters);
bool RunA3(int warmup, int iters);
bool RunOsUdp(int warmup, int iters);
bool RunE1(int warmup, int iters);
bool RunE2(int warmup, int iters);
bool RunE3(int warmup, int iters);
bool RunD1Multi(int warmup, int drive_iters);
bool RunD2(int warmup, int iters);
bool RunF(int warmup, int iters);

/** C5 mux HOL; D3 keepalive B/hour; E4 nested carrier; OsUdp full-stack AMP. */
bool RunC5(int warmup, int iters);
bool RunD3(int warmup, int iters);
bool RunE4(int warmup, int iters);
bool RunOsUdpAmp(int warmup, int iters);

} // namespace pp::amp::perf
