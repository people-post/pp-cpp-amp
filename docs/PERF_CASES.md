# AMP performance cases

Correctness coverage lives under `src/*/tests/` and `tests/integration/`. This document defines the **performance** case matrix and the scaffold under `tests/perf/`.

## Goals

- Isolate **L1 HMAC**, **L2 AEAD**, and **L3 FRAG** before blaming `AmpStack`.
- Prefer **MemoryDatagramIo** for CPU/alloc baselines; **OsUdpDatagramIo** for real latency (later).
- Shape workloads from product limits (`AmpChannelLimits`, ADP `kMaxPayload=1200`, FRAG chunk **900 B**).
- Report **distributions** (p50/p95/p99) where timing loops allow; always print CSV for regression diffs.
- Keep CI light: micro + FRAG smoke always on; soak / LAN behind env flags later.

## Case matrix

### A — Micro (CPU / ops)

| ID | What | Metrics | Status |
|----|------|---------|--------|
| **A1** | `WireCodec::Encode` + `HmacBinder::Seal`/`Verify` at 0 / 600 / 1200 B | ns/pkt, pkt/s | **scaffold** |
| **A2** | `SessionCrypto::Seal`/`Open` at 64 / 900 / 4 KiB | ns/op, MB/s | **scaffold** |
| **A3** | FRAG encode only (no net) at N bytes / 900 B chunks | frag count, encode time | planned |

### B — Transport (L1)

| ID | What | Metrics | Status |
|----|------|---------|--------|
| **B1** | Reliable window=16 saturation; loss 0/1/5/10% | stall, goodput | planned |
| **B2** | BestEffort flood @ 1200 B | pkt/s, drop rate | planned |
| **B3** | Media cadence ~20–60 B @ 50 pps | one-way p50/p99 | planned |

### C — Session / channel (L2–L3)

| ID | What | Metrics | Status |
|----|------|---------|--------|
| **C1** | `EnsureAssociation` → capability open | handshake ms | planned |
| **C2** | `OpenChannel` + first DATA RTT | ms | planned |
| **C3** | `ChannelMux::SendData` FRAG 900 / 64 / 128 / 256 KiB | reassembly time, MB/s, frags | **scaffold** (AmpTestLink) |
| **C4** | Realtime + Drop Oldest burst > 64 frames | drop %, queue age | planned |
| **C5** | Mux fairness (control + bulk + realtime) | HOL under Reliable | planned |

**C3 notes:**

- Large Reliable sends through ADP can hit `reliable_window=16` mid-FRAG if the caller does not pump ACKs between frames. The scaffold measures **AmpTestLink** (in-process seal → peer mux) so FRAG/AEAD throughput is not conflated with window pacing. Full-stack paced/OsUdp variants belong with B1 / OsUdp follow-ups.
- OPEN does not negotiate `max_message_bytes`; the responder keeps the `ChannelPolicy` default (`kMaxChatStreamJsonBytes` = 256 KiB). Scaffold sizes stop at 256 KiB. **512 KiB / 4 MiB** wait on wiring blob limits on both peers (or a test-only policy inject).

### D — Mesh runtime / scale

| ID | What | Metrics | Status |
|----|------|---------|--------|
| **D1** | `Drive()` with 1…48 links | µs/Drive | planned |
| **D2** | N-peer fan-in | aggregate MB/s | planned |
| **D3** | Keepalive warm/hot idle budget | bytes/hour | planned (see [KEEPALIVE.md](KEEPALIVE.md)) |

### E — Consumer-shaped

| ID | Profile | Status |
|----|---------|--------|
| **E1** | Ledger RPC ≤512 KiB (`/pp-ledger/rpc/1.0.0`) | planned (C3 sizes cover FRAG cost) |
| **E2** | Call media Realtime + Oldest | planned (C4) |
| **E3** | Chat blob ~4 MiB | planned |
| **E4** | Nested carrier vs direct | planned (after bridge e2e) |

### F — Adversarial cost

Timed reject of sealed garbage / partial FRAG (extends integration adversarial tests).

## Scaffold layout

```
tests/perf/
  perf_timer.h       # steady_clock helpers + percentiles
  perf_main.cpp      # runner: A1, A2, C3 → human + CSV
  CMakeLists.txt     # target pp_amp_perf
```

Build with `-DPP_AMP_BUILD_TESTS=ON`. Run:

```bash
./build/tests/perf/pp_amp_perf
# or
ctest --test-dir build -R pp_amp_perf --output-on-failure
```

Environment (optional):

| Variable | Default | Meaning |
|----------|---------|---------|
| `PP_AMP_PERF_ITERS` | `2000` | timed iterations for A1/A2 after warm-up |
| `PP_AMP_PERF_FRAG_ITERS` | `8` | timed iterations for C3 after warm-up |
| `PP_AMP_PERF_WARMUP` | `50` | discarded warm-up iterations (C3 caps warm-up at 2) |

CSV lines are prefixed with `csv,` for easy grep.

## Suggested build order (remaining)

1. C1 association latency (harness).
2. C4 media backpressure.
3. OsUdp loopback of C3 / B3.
4. D1 Drive cost.
5. E1 ledger-shaped full stack with paced Reliable send.

## Documentation targets (not CI gates yet)

Track on a fixed Release machine; tighten thresholds after a few runs:

- A1/A2: flag ~10–15% ns/op regressions across commits.
- C3 256 KiB (AmpTestLink): stable MB/s; same regression band.
- Assoc (when C1 lands): report p50/p99; PQ dominates — do not chase Memory RTT.
