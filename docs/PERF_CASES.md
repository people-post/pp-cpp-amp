# AMP performance cases

Correctness coverage lives under `src/*/tests/` and `tests/integration/`. This document defines the **performance** case matrix and the scaffold under `tests/perf/`.

## Goals

- Isolate **L1 HMAC**, **L2 AEAD**, and **L3 FRAG** before blaming `AmpStack`.
- Prefer **MemoryDatagramIo** for CPU/alloc baselines; **OsUdpDatagramIo** for real latency.
- Shape workloads from product limits (`AmpChannelLimits`, ADP `kMaxPayload=1200`, FRAG chunk **900 B**).
- Report **distributions** (p50/p95/p99); CSV lines prefixed with `csv,`.
- Keep CI light; heavier link counts via env.

## Case matrix

| ID | What | Status |
|----|------|--------|
| **A1** | Wire+HMAC 0/600/1200 | **scaffold** |
| **A2** | SessionCrypto Seal/Open 64/900/4KiB | **scaffold** |
| **A3** | FRAG ChannelWire encode only | **scaffold** |
| **B1** | Reliable + loss 0/5/10% | **scaffold** |
| **B2** | BestEffort flood 1200 B | **scaffold** |
| **B3** | Media ~40 B proc latency (50 pps clock) | **scaffold** |
| **OsUdp** | Loopback Reliable ~76 KiB | **scaffold** |
| **C1–C4** | Assoc / open+data / FRAG / Drop Oldest | **scaffold** |
| **D1** | Drive 1-link + hub fan-in 1/4/8/16 (+32/48 via env) | **scaffold** |
| **D2** | Hub fan-in data 4×64 KiB | **scaffold** |
| **E1** | Paced 512 KiB bulk (900 B frames) | **scaffold** |
| **E2** | Paced call-media 40 B @ 50 pps | **scaffold** |
| **E3** | Paced 4 MiB FRAG (64×900 B msgs, window-safe) | **scaffold** |
| **F** | Sealed-garbage reject cost | **scaffold** |
| **C5** | Mux 3-channel interleave (control/bulk/rt) | **scaffold** |
| **D3** | Hot/warm keepalive B/hour (simulated) | **scaffold** |
| **E4** | Nested carrier assoc + DATA vs direct | **scaffold** |
| **OsUdpAmp** | MeshRuntime on OsUdp paced 64 KiB | **scaffold** |

## Layout

```
tests/perf/
  perf_timer.h / perf_report.h
  perf_main.cpp    # A1–A2, C1–C4, D1(1-link)
  perf_more.cpp    # A3, B*, OsUdp, D1 multi, D2, E1–E3, F
  perf_finish.cpp  # C5, D3, E4, OsUdpAmp
```

```bash
./build/tests/perf/pp_amp_perf
```

| Variable | Default | Meaning |
|----------|---------|---------|
| `PP_AMP_PERF_ITERS` | `2000` | A1/A2/A3 |
| `PP_AMP_PERF_FRAG_ITERS` | `8` | C3/C5 |
| `PP_AMP_PERF_ASSOC_ITERS` | `3` | C1/C2/E4 |
| `PP_AMP_PERF_MEDIA_ITERS` | `8` | B3/C4/E2 |
| `PP_AMP_PERF_DRIVE_ITERS` | `2000` | D1 1-link |
| `PP_AMP_PERF_XFER_ITERS` | `3` | B1/B2/OsUdp/D2/D3/E1/E3/F/OsUdpAmp |
| `PP_AMP_PERF_MAX_LINKS` | `16` | D1 multi ladder cap (`32` / `48` optional) |
| `PP_AMP_PERF_WARMUP` | `50` | warm-up |

Matrix complete for the documented A–F / C5 / D3 / E4 / OsUdpAmp set. Heavier D1 link counts remain opt-in via `PP_AMP_PERF_MAX_LINKS`.

ADP bulk throughput notes: default `kDefaultReliableWindow` is **128** (was 16). `ChannelMux::SendData` preflights reliable FRAG against transport credits and propagates transport `WindowFull` so callers can pump/retry without stranding partial FRAG. OsUdp recv uses a reusable scratch buffer (no alloc on empty poll).
