# AMP link layer functional test matrix

> Moved from pp-browser `projects/adp/LINK_TEST_MATRIX.md` at [3e407e3](https://github.com/people-post/pp-browser/blob/3e407e342320719794edd3025c0c7a632bb5f13a/projects/adp/LINK_TEST_MATRIX.md) (pp-browser [CR025](https://github.com/people-post/pp-browser/blob/main/projects/capability-repos/DECISIONS.md#cr025--documentation-lives-with-what-it-describes-pp-specs-public-holds-shared-contracts-vectors-and-cross-repo-programs)). Links back into pp-browser point at its `main`.

**Tier:** A (link unit tests on `MemoryDatagramIo`; no Tier B adversarial matrix)  
**Contract:** [STACK.md](../STACK.md) · **Design:** [DECISIONS.md](../DECISIONS.md) (A025, A026)  
**Suite:** `pp_amp_link_test` (`src/link/tests/`; in pp-browser, before the extraction, `pp_browser_amp_link_test`)

Tier B cross-layer integration: [TEST_MATRIX.md](TEST_MATRIX.md) → `pp_amp_integration_test`.

## Multiaddr

| ID | Requirement | gtest | Status |
|----|-------------|-------|--------|
| LINK-A-01 | ADP multiaddr parse/format | `AdpMultiaddrTest.ParseAndFormatRoundTrip` | done |

## Association + channels

| ID | Requirement | gtest | Status |
|----|-------------|-------|--------|
| LINK-L-01 | EnsureAssociation over memory I/O | `MeshLinkTest.EnsureAssociationOverMemoryIo` | done |
| LINK-L-02 | OpenChannel DATA round-trip | `MeshLinkTest.OpenChannelDataRoundTrip` | done |
| LINK-L-03 | MeshRuntime pump drives assoc | `MeshRuntimeTest.PumpDrivesAssociationRoundTrip` | done |
| LINK-L-04 | Inbound link rekeys to registered alias | `MeshLinkTest.InboundLinkRekeysToRegisteredAlias` | done |

## Capability (ch0 over link)

| ID | Requirement | gtest | Status |
|----|-------------|-------|--------|
| LINK-C-01 | Capability exchange after assoc | `MeshLinkTest.CapabilityExchangeAfterAssociation` | done |
| LINK-C-02 | Ingest enables PeerId dial | `MeshLinkTest.CapabilityIngestEnablesPeerIdDial` | done |

## Dual-dial election

| ID | Requirement | gtest | Status |
|----|-------------|-------|--------|
| LINK-D-01 | One Connected link per PeerId | `MeshLinkTest.DualDialElectsOneConnectedLinkPerPeerId` | done |

## AmpStack

| ID | Requirement | gtest | Status |
|----|-------------|-------|--------|
| LINK-S-01 | Create + associate via stacks | `AmpStackTest.CreateAndAssociateViaStacks` | done |
