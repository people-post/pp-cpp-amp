# AMP integration test matrix (Track A)

> Moved from pp-browser `projects/adp/TEST_MATRIX.md` at [3e407e3](https://github.com/people-post/pp-browser/blob/3e407e342320719794edd3025c0c7a632bb5f13a/projects/adp/TEST_MATRIX.md) (pp-browser [CR025](https://github.com/people-post/pp-browser/blob/main/projects/capability-repos/DECISIONS.md#cr025--documentation-lives-with-what-it-describes-pp-specs-public-holds-shared-contracts-vectors-and-cross-repo-programs)). Links back into pp-browser point at its `main`.

**Tier:** B (local integration on `MemoryDatagramIo`)  
**Stack:** [STACK.md](../STACK.md) · [AMP-SESSION.md](../contracts/AMP-SESSION.md) · [AMP-CHANNEL.md](../contracts/AMP-CHANNEL.md)  
**Suite:** `pp_amp_integration_test` (`tests/integration/amp_integration_test.cpp`; in pp-browser, before the extraction, `pp_browser_amp_integration_test`)

Rekey receive path uses a **grace window ≤ 1 s** for the previous epoch ([AMP-SESSION.md § Rekey](../contracts/AMP-SESSION.md)).

## Failure propagation matrix

| ID | STACK / contract row | gtest | Status |
|----|----------------------|-------|--------|
| A-INT-01 | Channel RESET — siblings continue | `AmpIntegrationTest.ChannelResetSiblingSurvives` | done |
| A-INT-02 | L3 Reliable → ADP Reliable under loss | `AmpIntegrationTest.ReliableDataSurvivesLoss` | done |
| A-INT-03 | One session per PeerId (dual-dial) | `AmpIntegrationTest.DualDialChannelsWorkAfterElection` | done |
| A-INT-04 | MSH fail → no Session | `AmpIntegrationTest.MshFailureNoConnectedMux` | done |
| A-INT-05 | Association Close → Session ends; recovery | `AmpIntegrationTest.AssociationCloseAndRecovery` | done |
| A-INT-06 | L1 path migrate — Session unchanged | `AmpIntegrationTest.PathMigrateMidSession` | done |
| A-INT-07 | L3 FRAG through L2 + L1 | `AmpIntegrationTest.LargeFragRoundTripThroughStack` | done |
| A-INT-08 | Wire rekey + epoch grace | `AmpIntegrationTest.WireRekeyWithGraceWindow` | done |
| A-INT-09 | Post-grace stale epoch dropped | `AmpIntegrationTest.PostGraceStaleEpochDropped` | done |
| A-INT-10 | MSH chunk loss + ADP rtx (stretch) | — | deferred |

## Adversarial sender matrix (Track A-adv)

| ID | Threat | gtest | Status |
|----|--------|-------|--------|
| A-ADV-02 | `max_links` caps dial + inbound accept | `AmpIntegrationTest.AdversarialMaxLinksAdv02` | done |
| A-ADV-03 | Garbage MSH mid-handshake | `AmpIntegrationTest.AdversarialGarbageMshMidHandshakeAdv03` | done |
| A-ADV-04 | Handshake `dial_timeout` enforced | `AmpIntegrationTest.AdversarialDialTimeoutAdv04` | done |
| A-ADV-06 | Sealed AEAD garbage flood post-establish | `AmpIntegrationTest.AdversarialSealedGarbageFloodAdv06` | done |
| A-ADV-08 | FRAG partial assembly bomb | `AmpIntegrationTest.AdversarialFragPartialBombAdv08` | done |
| A-ADV-01 | Pre-decrypt rate limit | — | deferred |
| A-ADV-05 | OPEN flood | — | deferred |
| A-ADV-07 | Rekey spam | — | deferred |

## Session control wire (ch0)

| Field | Value |
|-------|-------|
| version | `2` (capability payloads use version `1`) |
| kind | `1` = RekeyRequest, `2` = RekeyAck |
| target_epoch | `u32` LE — must equal `local_epoch + 1` |
