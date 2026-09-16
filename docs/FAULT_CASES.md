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
| **N4** | Reorder (permute) | Steady | Multi-packet Reliable | Survive, app-ordered | Integ | **landed** (`ReliableDataSurvivesReorder`) |
| **N5** | Reorder FRAG | Steady | Multi-frag Bulk | Survive | L3 unit + integ | **landed** (`AssemblesOutOfOrder`, `BulkFragSurvivesReorder`) |
| **N5b** | Drop next N | Steady | Multi-frag Bulk | Survive | Integ | **landed** (`BulkFragSurvivesLoss`) |
| **N6** | Dup rate | Steady | Reliable | Survive once | L1 + integ | **landed** (`DeliverUnderDup`, `ReliableDataSurvivesDup`) |
| **N7** | Replay / late seq | Steady | BE + Reliable | Reject duplicate | L1 | **landed** (replay / dup-ACK) |
| **N8** | Bit-flip / truncate wire | Any | Encoded ADP | Reject, no crash | L1 | **landed** (mutilator + HMAC) |
| **N9** | Tamper sealed / Finished | HS / post | MSH / AEAD | Reject | L2 | **landed** |
| **N10** | Garbage mid-HS | Handshake | Raw + MSH-shaped | Recover or dial fail clean | Integ | **landed** (Adv03) |
| **N11** | Sealed garbage flood | Post-assoc | Invalid AEAD | Stay up; good data works | Integ | **landed** (Adv06) |
| **N12** | Partial FRAG bomb | Post-open | Incomplete frags | Bound memory; sweep | Integ | **landed** (Adv08) |
| **N13** | Loss + reorder | Steady | Reliable + Bulk FRAG | Survive | Integ | **landed** (`ReliableAndBulkSurviveLossPlusReorder`) |
| **N14** | Loss during rekey | Grace window | Mixed epochs | Survive / drop stale | Integ | Partial (rekey + post-grace; not under loss) |
| **N15** | Path migrate + loss | Mid-session | Reliable | Survive on new path | Integ | Partial (migrate only) |

## L1 Reliable in-order delivery

`Connection` holds out-of-order Reliable payloads and delivers contiguous seqs only to `OnMessage`
(`DeliverReliableInOrder`). BestEffort still delivers immediately (may be OOO).

This unlocks multi-FRAG Bulk under loss and true UDP permute E2E (L3 still requires contiguous
`channel_seq`, which L1 now guarantees for Reliable).

## Priority fill order

1. **N4 / N5 / N6** — done.
2. **N13** — done (simultaneous loss + reorder).
3. **L1 in-order Reliable** — **done** (`DeliverInOrderAfterGap`, Bulk loss / permute E2E).
4. **N2 soak** — promote B1-style loss into seeded nightly/soak gtest.
5. **N14 / N15** — loss during rekey / path migrate + loss.

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
