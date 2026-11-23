# Status

What is left to do in this library. Everything else the stack's design called for is built: the four layers, the link
plane, wire v2 ([A029](DECISIONS.md#a029--a-smaller-packet-one-sealed-header-derived-nonces-no-second-tag)), the nested
Session over a carrier ([A024](DECISIONS.md#a024--amp-call-media-over-circuit--nested-session)), and the test matrices
under [testing/](testing/TEST_MATRIX.md) except the rows below.

> Collected from pp-browser `projects/adp/CURRENT_STATE.md` (§ Next) and `PHASES.md` at
> [3e407e3](https://github.com/people-post/pp-browser/tree/3e407e342320719794edd3025c0c7a632bb5f13a/projects/adp), the
> deferred rows of the test matrices, and what [A029](DECISIONS.md#a029--a-smaller-packet-one-sealed-header-derived-nonces-no-second-tag)
> left open (pp-browser [CR025](https://github.com/people-post/pp-browser/blob/main/projects/capability-repos/DECISIONS.md#cr025--documentation-lives-with-what-it-describes-pp-specs-public-holds-shared-contracts-vectors-and-cross-repo-programs)).
> Only the library's own work is here; the app's adoption phases stay in pp-browser's
> [projects/adp](https://github.com/people-post/pp-browser/tree/main/projects/adp/).

## Open

| Item | What | From |
|------|------|------|
| **A-INT-10** (stretch) | An integration test of the handshake under loss: an MSH chunk lost and resent by ADP's reliable delivery. The fault matrix still starts its faults after the handshake ([FAULT_CASES](FAULT_CASES.md)) | [TEST_MATRIX](testing/TEST_MATRIX.md#failure-propagation-matrix); pp-browser CURRENT_STATE § Next 4 |
| **A-ADV-01** pre-decrypt rate limit | Drop obvious pre-session garbage before the ML-KEM work, with per-source-IP and per-association counters: "required for production UDP" ([AMP-SESSION § Rate limiting](contracts/AMP-SESSION.md#rate-limiting-pre-decrypt)) | [TEST_MATRIX](testing/TEST_MATRIX.md#adversarial-sender-matrix-track-a-adv) |
| **A-ADV-05** OPEN flood | A peer opening channels without end | [TEST_MATRIX](testing/TEST_MATRIX.md#adversarial-sender-matrix-track-a-adv) |
| **A-ADV-07** rekey spam | A peer asking for rekeys without end | [TEST_MATRIX](testing/TEST_MATRIX.md#adversarial-sender-matrix-track-a-adv) |

## Left open by a decision (not planned)

- **Key-phase bit** in the packet's type byte, to tell which key a packet uses during a rekey's grace window instead of
  trying the old key when the new one fails. Optional while no product path rekeys; not built
  ([A029](DECISIONS.md#a029--a-smaller-packet-one-sealed-header-derived-nonces-no-second-tag), *How H1 is built*).
- **The outer frame header of a circuit packet** (at most ~9 B of the 59 B around a 1:1 circuit's inner frame): the
  only part left to trim after H3 was withdrawn, "as a separate change if ever" (A029, *H3 withdrawn*).

## Not this library's

Open items in pp-browser's `projects/adp` that are the app's or pp-cpp-mesh's, listed so they are not looked for here:
listen policy and mDNS-only edge cases (D8 follow-on; LAN discovery is pp-cpp-mesh's), the org seed's UDP port, and
moving L4 teardown out of frame callbacks into the parent ([A027](DECISIONS.md#a027--parent-only-destroy-l3l4-ownership-hierarchy)
follow-on: call media, media relay and circuit are the app's and pp-cpp-mesh's code). See pp-browser
[CURRENT_STATE](https://github.com/people-post/pp-browser/blob/main/projects/adp/CURRENT_STATE.md).
