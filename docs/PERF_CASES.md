# AMP performance cases

Correctness coverage lives under `src/*/tests/` and `tests/integration/`. This document defines the **performance** case matrix and the scaffold under `tests/perf/`.

## Goals

- Isolate **L1 HMAC**, **L2 AEAD**, and **L3 FRAG** before blaming `AmpStack`.
- Prefer **MemoryDatagramIo** for CPU/alloc baselines; **OsUdpDatagramIo** for real latency.
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
| **B1** | Reliable 64×600 B; loss 0 / 5 / 10% | MB/s under rtx | **scaffold** |
| **B2** | BestEffort flood @ 1200 B | pkt/s, drop rate | planned |
| **B3** | Media cadence ~20–60 B @ 50 pps | one-way p50/p99 | planned |
| **OsUdp** | Loopback Reliable ~76 KiB (64×1200) | MB/s | **scaffold** |

### C — Session / channel (L2–L3)

| ID | What | Metrics | Status |
|----|------|---------|--------|
| **C1** | `EnsureAssociation` → capability open | handshake ms | **scaffold** |
| **C2** | `OpenChannel` + first DATA RTT | ms | **scaffold** |
| **C3** | `ChannelMux::SendData` FRAG 900 / 64 / 256 / 512 KiB / 4 MiB | reassembly time, MB/s | **scaffold** |
| **C4** | Realtime + Drop Oldest burst > 64 frames | drop %, burst time | **scaffold** |
| **C5** | Mux fairness (control + bulk + realtime) | HOL under Reliable | planned |

### D — Mesh runtime / scale

| ID | What | Metrics | Status |
|----|------|---------|--------|
| **D1** | `Drive()` 1-link pair + hub fan-in 1/4/8 | µs/Drive | **scaffold** |
| **D2** | N-peer fan-in data plane | aggregate MB/s | planned |
| **D3** | Keepalive warm/hot idle budget | bytes/hour | planned (see [KEEPALIVE.md](KEEPALIVE.md)) |

### E — Consumer-shaped

| ID | Profile | Status |
|----|---------|--------|
| **E1** | Paced 512 KiB bulk over harness (`/pp-ledger/rpc/1.0.0`, 900 B chunks) | **scaffold** |
| **E2** | Call media Realtime + Oldest | **scaffold** (C4) |
| **E3** | Chat blob ~4 MiB | **scaffold** (C3) |
| **E4** | Nested carrier vs direct | planned |

### F — Adversarial cost

Timed reject of sealed garbage / partial FRAG (extends integration adversarial tests).

## Scaffold layout

```
tests/perf/
  perf_timer.h / perf_report.h
  perf_main.cpp      # A1–A2, C1–C4, D1(1-link)
  perf_more.cpp      # B1, OsUdp, D1 multi, E1
  CMakeLists.txt
```

```bash
./build/tests/perf/pp_amp_perf
ctest --test-dir build -R '^pp_amp_perf$' --output-on-failure
```

| Variable | Default | Meaning |
|----------|---------|---------|
| `PP_AMP_PERF_ITERS` | `2000` | A1/A2 |
| `PP_AMP_PERF_FRAG_ITERS` | `8` | C3 (4 MiB ≤2) |
| `PP_AMP_PERF_ASSOC_ITERS` | `3` | C1/C2 |
| `PP_AMP_PERF_MEDIA_ITERS` | `8` | C4 |
| `PP_AMP_PERF_DRIVE_ITERS` | `2000` | D1 1-link (multi uses min(500)) |
| `PP_AMP_PERF_XFER_ITERS` | `3` | B1 / OsUdp / E1 |
| `PP_AMP_PERF_WARMUP` | `50` | warm-up (cases may cap lower) |

## Suggested build order (remaining)

1. B2/B3 BestEffort + media cadence.
2. D1 toward 48 links / D2 data fan-in.
3. OsUdp of C3 FRAG (full stack).
4. Adversarial CPU cost (F).

## Documentation targets (not CI gates yet)

- A1/A2 / C3: ~10–15% regression band.
- C1: p50/p99 ms; PQ dominates.
- B1: MB/s should degrade gracefully 0→10% loss.
- E1: paced 512 KiB Memory path stable MB/s.
- D1: µs/Drive rises with link count but stays bounded.
