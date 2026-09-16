# AMP fault / bad-network cases

Correctness coverage for lossy, corrupt, reordered, and duplicate traffic.
Performance under loss lives in [PERF_CASES.md](PERF_CASES.md) (B1). This matrix is **pass/fail**.

## Design rules

1. **Deterministic first** — `DropNext(N)`, seeded reorder/dup. Probabilistic rates only as soak.
2. **One primary fault per case** — plus a small combo row (loss + reorder).
3. **Outcomes**
   - **Survive**: payload eventually delivered; link stays connected
   - **Reject**: no crash, no false accept; fail clean or ignore junk
4. **Layer placement**
   - Wire / HMAC / AEAD mutilation → L1 / L2 unit
   - FRAG reassembly / dup frag → L3 unit
   - “App message still arrives” → `tests/integration/` (`AmpIntegrationTest`)

Injection knobs: `MemoryDatagramIo` (`DropNext`, `SetDropRate`, `SetReorderWindow`, `SetDupRate`, `SetRngSeed`) and
`AmpIntegrationHarness` (`ConfigureLoss`, `ConfigureReorder`, `ConfigureDup`, `FlushReorder`, `ClearFaultInjection`, inject helpers).

## Case matrix

| ID | Fault | When | Payload | Expect | Layer | Status |
|----|-------|------|---------|--------|-------|--------|
| **N1** | Drop next N | Steady | Small Reliable | Survive (rtx) | L1 + integ | **landed** (`DeliverUnderLoss`, `ReliableDataSurvivesLoss`) |
| **N2** | Drop rate 5/10% | Steady | Window-filling Reliable | Survive | L1 / soak | Perf B1 only |
| **N3** | Drop BestEffort | Steady | Realtime / BE | No rtx; loss OK | L1 | **landed** (`BestEffortNoRtxOnDrop`) |
| **N4** | Reorder hold+flush | Steady | Multi-packet Reliable | Survive, app-ordered | Integ | **landed** (`ReliableDataSurvivesReorder`) — delay only; see gap |
| **N5** | Reorder hold+flush | Steady | Multi-frag Bulk | Survive | L3 unit + integ | **landed** (`AssemblesOutOfOrder`, `BulkFragSurvivesReorder`) |
| **N6** | Dup rate | Steady | Reliable | Survive once | L1 + integ | **landed** (`DeliverUnderDup`, `ReliableDataSurvivesDup`) |
| **N7** | Replay / late seq | Steady | BE + Reliable | Reject duplicate | L1 | **landed** (replay / dup-ACK) |
| **N8** | Bit-flip / truncate wire | Any | Encoded ADP | Reject, no crash | L1 | **landed** (mutilator + HMAC) |
| **N9** | Tamper sealed / Finished | HS / post | MSH / AEAD | Reject | L2 | **landed** |
| **N10** | Garbage mid-HS | Handshake | Raw + MSH-shaped | Recover or dial fail clean | Integ | **landed** (Adv03) |
| **N11** | Sealed garbage flood | Post-assoc | Invalid AEAD | Stay up; good data works | Integ | **landed** (Adv06) |
| **N12** | Partial FRAG bomb | Post-open | Incomplete frags | Bound memory; sweep | Integ | **landed** (Adv08) |
| **N13** | Loss + hold/flush | Steady | Small Reliable | Survive | Integ | **landed** (`ReliableSurvivesLossPlusReorder`, sequential phases) |
| **N14** | Loss during rekey | Grace window | Mixed epochs | Survive / drop stale | Integ | Partial (rekey + post-grace; not under loss) |
| **N15** | Path migrate + loss | Mid-session | Reliable | Survive on new path | Integ | Partial (migrate only) |

## Known gap (blocks true OOO / multi-FRAG loss E2E)

L1 Reliable `ReplayWindow` **accepts and delivers out of order** to the mux, while L3 Reliable
requires **contiguous `channel_seq`** (`out of order seq` / `out of order frag seq`). If an earlier
ADP datagram is lost or delayed and a later one is delivered first, L3 rejects the later frame;
L1 has already ACKed it, so it will not retransmit — the channel strands.

Consequences until L1 resequences Reliable payloads before `OnMessage` (or L3 buffers OOO seqs):

- Integration reorder cases use **hold + `FlushReorder` (FIFO)** with `window > burst` (no random permute).
- Multi-FRAG **Bulk under `DropNext` / loss** is **not** asserted E2E yet.
- True UDP permute + Bulk loss+reorder remain **blocked** (track as N4b / N5b / N13b when fixed).

L3 `MessageReassembly::AssemblesOutOfOrder` still covers FRAG *index* reordering once frames are in seq order.

## Priority fill order

1. **N4 / N5 / N6** — knobs already on `MemoryDatagramIo` (**done**, hold+flush / dup).
2. **N13** — small Reliable combo (**done**).
3. **L1 in-order Reliable delivery** — unlocks Bulk-under-loss and true permute E2E.
4. **N2 soak** — promote B1-style loss into seeded nightly/soak gtest.
5. **N14 / N15** — after steady-state N\* is green.

## Case template

```text
Given: associated link, open channel (ControlJson | Bulk)
Fault: DropNext(N) | ConfigureReorder(window, seed) | ConfigureDup(rate, seed) | Inject*
Traffic: send M messages / one large FRAG
Drive: FlushReorder; ClearFaultInjection; AdvanceMs(rtx) × K
Assert:
  - received == sent (Survive)  OR  no false Connected / no crash (Reject)
```

## Harness notes

- Apply fault injection **after** associate / channel open (handshake needs a clean path).
- Prefer **fixed seeds** in CI; log seed on failure for soak runs.
- For reorder integration tests, keep `window > outbound burst` so release is FIFO via `FlushReorder`
  (avoids the L1→L3 OOO gap above).
- After the fault burst, call `ClearFaultInjection` so retransmits are not stranded in a reorder window.
- Do not re-test HMAC bit-flip at integration if L1 already owns that invariant.

## Layout

| Area | Location |
|------|----------|
| L1 unit | `src/L1/tests/` |
| L2 unit | `src/L2/tests/` |
| L3 unit | `src/L3/tests/` |
| Integration | `tests/integration/amp_integration_test.cpp` |
| Harness | `tests/support/amp_integration_harness.h` |
| Perf under loss | `tests/perf/` + [PERF_CASES.md](PERF_CASES.md) B1 |
