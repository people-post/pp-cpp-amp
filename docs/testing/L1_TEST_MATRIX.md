# ADP L1 functional test matrix

> Moved from pp-browser `projects/adp/L1_TEST_MATRIX.md` at [3e407e3](https://github.com/people-post/pp-browser/blob/3e407e342320719794edd3025c0c7a632bb5f13a/projects/adp/L1_TEST_MATRIX.md) (pp-browser [CR025](https://github.com/people-post/pp-browser/blob/main/projects/capability-repos/DECISIONS.md#cr025--documentation-lives-with-what-it-describes-pp-specs-public-holds-shared-contracts-vectors-and-cross-repo-programs)). Links back into pp-browser point at its `main`.

**Tier:** A (pure L1 on `MemoryDatagramIo` + `VirtualClock`; OsUdp smoke in harden suite)  
**Contract:** [docs/contracts/ADP.md](../contracts/ADP.md) · **Design:** [DESIGN.md](../DESIGN.md)  
**Suite:** `pp_amp_l1_test` (`src/L1/tests/`; in pp-browser, before the extraction, `pp_browser_adp_test`)

Cross-layer L1 checks live in [TEST_MATRIX.md](TEST_MATRIX.md) (`A-INT-02`, `A-INT-05`, `A-INT-06`; `A-INT-10` deferred).

## Wire + HMAC

| ID | Requirement | gtest | Status |
|----|-------------|-------|--------|
| L1-W-01 | LE header layout (28 B) | `AdpWireTest.GoldenHeaderLayoutLittleEndian` | done |
| L1-W-02 | Empty + max payload round-trip | `AdpWireTest.RoundTripEmptyPayload`, `RoundTripMaxPayload` | done |
| L1-W-03 | Encode rejects payload > 1200 | `AdpWireTest.RejectOversizePayload` | done |
| L1-W-04 | Decode rejects unknown version / truncated | `AdpWireTest.RejectUnknownVersion`, `RejectTruncated` | done |
| L1-W-05 | Decode rejects bad packet type (> Close) | `AdpWireTest.RejectBadPacketType` | done |
| L1-W-06 | Decode rejects payload_len mismatch | `AdpWireTest.RejectLengthMismatch` | done |
| L1-H-01 | HMAC seal/verify + tamper reject | `AdpWireTest.HmacAcceptsAndRejectsBitFlip`, `HmacWrongKey`, `HmacTagFlipRejected` | done |

## Threat model

| ID | Requirement | gtest | Status |
|----|-------------|-------|--------|
| L1-T-01 | Wrong HMAC inbound → no association | `AdpEdgeTest.AcceptWrongKeyDropsInbound` | done |
| L1-T-02 | Duplicate BE seq on wire → single delivery | `AdpEdgeTest.ReplayRejectOnWireBestEffort` | done |
| L1-T-03 | Duplicate Reliable seq → ACK only, one app delivery | `AdpEdgeTest.ReliableDupAckOnlyOnce` | done |
| L1-T-04 | Timestamp skew on accept path | `AdpConnTest.TimestampSkewReject` | done |

## Delivery

| ID | Requirement | gtest | Status |
|----|-------------|-------|--------|
| L1-D-01 | BestEffort deliver, no rtx on drop | `AdpConnTest.BestEffortDeliver`, `BestEffortNoRtxOnDrop` | done |
| L1-D-02 | Reliable rtx under loss + ACK stops rtx | `AdpReliableTest.DeliverUnderLoss`, `AckStopsRetransmit` | done |
| L1-D-03 | QoS isolation (separate seq spaces) | `AdpReliableTest.QosIsolation` | done |
| L1-D-04 | Reliable window full → send fails | `AdpEdgeTest.ReliableWindowFull` | done |
| L1-D-05 | Send payload > 1200 → fails | `AdpEdgeTest.PayloadTooLargeOnSend` | done |
| L1-D-06 | Give up after max_rtx (no crash) | `AdpReliableTest.GiveUpAfterMaxRtx` | done |

## Lifecycle + association

| ID | Requirement | gtest | Status |
|----|-------------|-------|--------|
| L1-L-01 | Local close + send-after-close fails | `AdpConnTest.SendAfterCloseFails`, `AdpReliableTest.ShutdownClose` | done |
| L1-L-02 | Remote Close → peer IsClosed | `AdpReliableTest.ShutdownClose` | done |
| L1-L-03 | LooksAlive after auth / timeout | `AdpEdgeTest.LooksAliveTimeout` | done |
| L1-L-04 | UpgradeBinder rejects old key | `AdpEdgeTest.UpgradeBinderRejectsOldKey` | done |
| L1-L-05 | mint_id assigns non-zero AssocId; unique across clock ticks | `AdpEdgeTest.MintAssocIdUnique` | done |
| L1-L-06 | Duplicate Open same AssocId → error | `AdpEdgeTest.DuplicateOpenSameAssocFails` | done |

## Endpoint / accept policy

| ID | Requirement | gtest | Status |
|----|-------------|-------|--------|
| L1-E-01 | Demux by assoc_id | `AdpConnTest.DemuxMultipleAssocs` | done |
| L1-E-02 | Accept disabled → inbound dropped | `AdpEdgeTest.AcceptDisabledDropsInbound` | done |
| L1-E-03 | Accept handler fires on new association | `AdpEdgeTest.AcceptHandlerFiresOnce` | done |
| L1-E-04 | AcceptOrCreate returns existing connection | `AdpEdgeTest.AcceptOrCreateReturnsExisting` | done |

## NAT / path

| ID | Requirement | gtest | Status |
|----|-------------|-------|--------|
| L1-P-01 | Server replies to observed source | `AdpConnTest.NatReplyUsesObservedAddr` | done |
| L1-P-02 | Path migrate + OnPathChange | `AdpConnTest.PathMigrate` | done |
| L1-P-03 | Path migrate mid-session (L2 unchanged) | `AmpIntegrationTest.PathMigrateMidSession` | done (Tier B) |

## I/O + hardening

| ID | Requirement | gtest | Status |
|----|-------------|-------|--------|
| L1-I-01 | MemoryDatagramIo deterministic loss | `AdpReliableTest.DeliverUnderLoss` | done |
| L1-I-02 | OsUdp loopback smoke | `AdpHardenTest.OsUdpLoopbackSmoke` | done |
| L1-I-03 | Mutilated datagrams never crash | `AdpHardenTest.PacketMutilatorNeverCrashes` | done |
| L1-I-04 | Multi-association stress | `AdpHardenTest.MultiConnectionStressMemory` | done |

## Out of scope (L1 functional matrix)

| Item | Reason |
|------|--------|
| Seq wrap at `0xffffffff` | No product path; would need test hooks |
| OsUdp bind/sendto errno paths | Platform I/O, not ADP semantics |
| Probabilistic `SetDropRate` | Deterministic `DropNext` covers loss semantics |
| MSH chunk loss + ADP rtx | Tier B `A-INT-10` (deferred) |
