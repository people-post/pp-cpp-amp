# AMP L2 (MSH session) functional test matrix

> Moved from pp-browser `projects/adp/L2_TEST_MATRIX.md` at [3e407e3](https://github.com/people-post/pp-browser/blob/3e407e342320719794edd3025c0c7a632bb5f13a/projects/adp/L2_TEST_MATRIX.md) (pp-browser [CR025](https://github.com/people-post/pp-browser/blob/main/projects/capability-repos/DECISIONS.md#cr025--documentation-lives-with-what-it-describes-pp-specs-public-holds-shared-contracts-vectors-and-cross-repo-programs)). Links back into pp-browser point at its `main`.

**Tier:** A (pure L2; in-memory handshake + crypto unit tests)  
**Contract:** [docs/contracts/AMP-SESSION.md](../contracts/AMP-SESSION.md) · **Design:** [STACK.md](../STACK.md)  
**Suite:** `pp_amp_l2_test` (`src/L2/tests/`; in pp-browser, before the extraction, `pp_browser_amp_session_test`)

Cross-layer checks live in [TEST_MATRIX.md](TEST_MATRIX.md) (Tier B).

## MSH wire

| ID | Requirement | gtest | Status |
|----|-------------|-------|--------|
| L2-W-01 | Hello round-trip (client + server) | `MshWireTest.HelloRoundTripClientAndServer` | done |
| L2-W-02 | Payload round-trip (client + server) | `MshWireTest.PayloadRoundTripClientAndServer` | done |
| L2-W-03 | Finished round-trip | `MshWireTest.FinishedRoundTrip` | done |
| L2-W-04 | Wrong message type rejected | `MshWireTest.RejectWrongMessageType` | done |
| L2-W-05 | Truncated hello rejected | `MshWireTest.RejectTruncatedHello` | done |
| L2-W-06 | Trailing bytes rejected | `MshWireTest.RejectTrailingBytesOnHello` | done |
| L2-W-07 | Unsupported hello version rejected | `MshWireTest.RejectUnsupportedHelloVersion` | done |
| L2-W-08 | Encode rejects bad field sizes | `MshWireTest.RejectBadFieldSizesOnEncode` | done |
| L2-W-09 | Identity bind prefix + KEM key size | `MshWireTest.BuildIdentitySignMessageRequiresFullKemKey` | done |

## MSH threat

| ID | Requirement | gtest | Status |
|----|-------------|-------|--------|
| L2-H-01 | MSH run establishes complementary sessions | `MshHandshakeTest.RunEstablishesComplementarySessions` | done |
| L2-H-02 | Tampered finished fails (isolated MAC) | `MshHandshakeTest.TamperedFinishedFails` | done |
| L2-T-01 | Invalid identity signature rejected | `MshEdgeTest.InvalidIdentitySignatureRejected` | done |
| L2-T-02 | Transcript hash order-sensitive | `MshEdgeTest.TranscriptHashOrderMatters` | done |
| L2-T-03 | VerifyFinished rejects bit-flip | `MshEdgeTest.VerifyFinishedRejectsBitFlip` | done |
| L2-T-04 | VerifyFinished rejects wrong master IKM | `MshEdgeTest.VerifyFinishedRejectsWrongMasterIkm` | done |

## Session AEAD

| ID | Requirement | gtest | Status |
|----|-------------|-------|--------|
| L2-C-01 | Seal/open round-trip | `SessionCryptoTest.SealOpenRoundTrip` | done |
| L2-C-02 | Wrong channel_seq in AAD fails | `SessionCryptoTest.WrongChannelSeqFailsOpen` | done |
| L2-C-03 | Wrong channel_id fails | `SessionCryptoTest.WrongChannelIdFailsOpen` | done |
| L2-C-04 | Wrong epoch fails | `SessionCryptoTest.WrongEpochFailsOpen` | done |
| L2-C-05 | Wrong direction fails | `SessionCryptoTest.WrongDirectionFailsOpen` | done |
| L2-C-06 | Wrong key fails | `SessionCryptoTest.WrongKeyFailsOpen` | done |
| L2-C-07 | Tampered ciphertext fails | `SessionCryptoTest.TamperedCiphertextFailsOpen` | done |
| L2-C-08 | Sealed blob too short fails | `SessionCryptoTest.SealedTooShortFailsOpen` | done |
| L2-C-09 | AAD binds direction byte | `SessionCryptoTest.BuildAadBindsDirectionByte` | done |
| L2-C-10 | Rekey rotates send/recv keys | `SessionTest.RekeyRotatesSendRecv` | done |

## Key derivation

| ID | Requirement | gtest | Status |
|----|-------------|-------|--------|
| L2-K-01 | Transcript hash deterministic | `SessionKeysTest.TranscriptHashDeterministic` | done |
| L2-K-02 | Directional keys differ + cross | `SessionKeysTest.DeriveDirectionalKeysDiffer` | done |
| L2-K-03 | Rekey changes send/recv; k_assoc stable | `SessionKeysTest.RekeyChangesSendRecv` | done |
| L2-K-04 | Empty master IKM rejected | `SessionKeysTest.RejectEmptyMasterIkm` | done |
| L2-K-05 | Bad transcript hash size rejected | `SessionKeysTest.RejectBadTranscriptHashSize` | done |
| L2-K-06 | k_assoc ≠ directional keys | `SessionKeysTest.AssocKeyDiffersFromDirectionalKeys` | done |
| L2-K-07 | Golden transcript hash vector | `SessionKeysTest.GoldenTranscriptHash` | done |

## Session object

| ID | Requirement | gtest | Status |
|----|-------------|-------|--------|
| L2-S-01 | FromMaterial rejects bad key sizes | `SessionEdgeTest.FromMaterialRejectsBadKeySizes` | done |
| L2-S-02 | FromMaterial rejects empty master/transcript | `SessionEdgeTest.FromMaterialRejectsEmptyMasterOrTranscript` | done |
| L2-S-03 | AssocKey matches k_assoc | `SessionEdgeTest.AssocKeyMatchesMaterial` | done |
| L2-S-04 | Bidirectional seal/open after handshake | `SessionEdgeTest.BidirectionalSealOpenAfterHandshake` | done |
| L2-S-05 | ApplyRekey rejects wrong target epoch | `SessionEdgeTest.ApplyRekeyRejectsUnexpectedEpoch` | done |
| L2-S-06 | Rekey() clears grace state | `SessionEdgeTest.RekeyClearsGraceState` | done |
| L2-S-07 | Rekey() invalidates prior epoch traffic | `SessionEdgeTest.RekeyInvalidatesPriorEpochTraffic` | done |

## Session control wire (ch0)

| ID | Requirement | gtest | Status |
|----|-------------|-------|--------|
| L2-X-01 | Control request/ack round-trip | `SessionControlCodecTest.RoundTripRequestAndAck` | done |
| L2-X-02 | Distinguishes from capability v1 | `SessionControlCodecTest.DistinguishesFromCapabilityVersionOne` | done |
| L2-X-03 | Bad wire length rejected | `SessionControlCodecTest.RejectBadLength` | done |
| L2-X-04 | Bad version/kind rejected | `SessionControlCodecTest.RejectBadVersionAndKind` | done |
| L2-X-05 | LooksLike requires v2 + known kind | `SessionControlCodecTest.LooksLikeRequiresVersionTwoAndKnownKind` | done |

## Rekey grace

| ID | Requirement | gtest | Status |
|----|-------------|-------|--------|
| L2-R-01 | Previous epoch accepted within grace | `SessionRekeyGraceTest.ApplyRekeyAcceptsPreviousEpochWithinGrace` | done |
| L2-R-02 | Previous epoch rejected after grace | `SessionRekeyGraceTest.ApplyRekeyRejectsPreviousEpochAfterGrace` | done |

## Out of scope (L2 functional matrix)

| Item | Reason |
|------|--------|
| Path migrate + session unchanged | Tier B `A-INT-06` |
| MSH fail → no session | Tier B `A-INT-04` |
| Wire rekey over ch0 end-to-end | Tier B `A-INT-08`, `A-INT-09` |
| Pre-decrypt rate limit / garbage flood | Tier B `A-ADV-*` |
| Stepwise ADP Reliable MSH carriage | Link layer + integration |
| Full-handshake inject bad Finished mid-Run | Needs step API; wire-level coverage above |
