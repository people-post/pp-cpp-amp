# AMP Session — L2 normative contract

> Moved from pp-browser `docs/contracts/AMP-SESSION.md` at [3e407e3](https://github.com/people-post/pp-browser/blob/3e407e342320719794edd3025c0c7a632bb5f13a/docs/contracts/AMP-SESSION.md) (pp-browser [CR025](https://github.com/people-post/pp-browser/blob/main/projects/capability-repos/DECISIONS.md#cr025--documentation-lives-with-what-it-describes-pp-specs-public-holds-shared-contracts-vectors-and-cross-repo-programs)). Links back into pp-browser point at its `main`.

**Status:** Foundation spec (2026-08-30). Normative for amp L2 (`include/amp/L2/`, `src/L2/`; `lib/amp/L2/` in pp-browser before the extraction).
**Stack:** [STACK.md](../STACK.md) · L1 [ADP.md](ADP.md) · L3 [AMP-CHANNEL.md](AMP-CHANNEL.md)  
**Version axes:** `msh_version`, `session_epoch`

## Role

L2 **AMP Session** replaces Noise-on-TCP as the product mesh security layer:

- Authenticate remote **PeerId** (ML-DSA-65)
- Provide **forward secrecy** (ML-KEM-768 ephemeral exchange)
- **Encrypt all L3+ payloads** (session profile **full** only): the session seals each packet, L1 carries it ([ADP.md](ADP.md))
- Derive **K_assoc** for ADP L1 HMAC binding of handshake and control packets

L4 application E2E (chat `payload_b64`, call-media AEAD) is unchanged and complementary.

## Threat model

| Adversary | Protected by |
|-----------|--------------|
| Impersonate PeerId | ML-DSA identity binding in MSH Finished |
| Harvest-now-decrypt-later on session keys | ML-KEM ephemeral in MSH |
| Inject / alter L3+ payloads | Session AEAD; the packet's clear header is its associated data |
| Replay | Per-space replay windows on packet numbers, under keys fresh per session and epoch |
| Cross-channel confusion | The frame (channel id, seq) travels sealed |
| UDP inject without membership | Sealed packets: the AEAD tag; handshake and control packets: L1 HMAC (`K_assoc` once established) |

PQ bar aligns with [libp2p-pq-transport P001](https://github.com/people-post/pp-browser/blob/main/projects/_archive/libp2p-pq-transport/DECISIONS.md#p001--full-pq-threat-bar-transport-secrecy--peeridauth) — transport semantics move from Noise-on-TCP to **MSH-on-ADP**.

## Cryptographic profile (v1)

| Item | Choice |
|------|--------|
| Handshake | **MSH v1** — message-native ordered Reliable ADP messages |
| KEM | ML-KEM-768 (encaps/decaps on handshake tokens) |
| Identity | ML-DSA-65 device key; PeerId = multihash of marshalled pubkey (KeyType wire `4`) |
| Session AEAD | XChaCha20-Poly1305, the nonce derived from the packet number (§ Sealing packets) |
| KDF | HKDF-SHA256 from handshake transcript hash |

Semantic reference for KEM token order: [libp2p-pq-transport DESIGN](https://github.com/people-post/pp-browser/blob/main/projects/_archive/libp2p-pq-transport/DESIGN.md) (XXkem message order); MSH encodes the same semantics as **discrete messages**, not a byte pipe.

## MSH v1 transcript (outline)

Messages are sent on ADP **Reliable** before Session is established. Pre-session datagrams use a reserved **pre-session assoc** or cleartext handshake assoc id per implementation — must be documented in code with matching tests.

| Step | Direction | Message | Purpose |
|------|-----------|---------|---------|
| 1 | I → R | `ClientHello` | `msh_version`, client ephemeral ML-KEM pk, nonce |
| 2 | R → I | `ServerHello` | server ephemeral ML-KEM pk, nonce |
| 3 | R → I | `ServerPayload` | KEM ct(s), ML-DSA identity + static binding signature |
| 4 | I → R | `ClientPayload` | KEM ct(s), ML-DSA identity + static binding signature |
| 5 | both | `Finished` | MAC over full transcript |

**Identity binding payload** (unchanged semantics from P006):

```text
"noise-libp2p-static-key:" || static_mlkem_public_key (1184 B)
```

Signed with device ML-DSA-65 private key.

**Finished** covers all prior messages in order; mismatch → fail closed.

Exact field sizes and endianness are fixed in implementation + KAT tests (`pp_browser_amp_session_test`).

## Key derivation

From transcript hash `T = SHA-256(client_hello || … || finished)`:

| Label | Length | Use |
|-------|--------|-----|
| `pp-amp-k-assoc-v1` | 32 B | ADP L1 HMAC (`K_assoc`) |
| `pp-amp-k-session-v1` | 32 B | L2 AEAD (`K_session`) |
| `pp-amp-k-client-v1` | 32 B | AEAD key initiator → responder |
| `pp-amp-k-server-v1` | 32 B | AEAD key responder → initiator |

**Never** use the same key material for HMAC and AEAD.

On **rekey**, increment `session_epoch` and derive new **directional** keys (`k_send` / `k_recv`) with label `pp-amp-k-*-v1|epoch:<n>`. **`k_assoc` is stable** across rekey (L1 binder unchanged).

## Session record

| Field | Notes |
|-------|-------|
| `local_peer_id` / `remote_peer_id` | ML-DSA PeerIds |
| `session_epoch` | Starts at 1; bumps on rekey |
| `k_assoc` | Feeds ADP `Connection` |
| `k_send` / `k_recv` | Directional AEAD keys |
| `state` | `Handshaking` → `Established` → `Rekeying` → `Closed` |

## Sealing packets

The session seals and opens its link's packets ([A029](../DECISIONS.md#a029--a-smaller-packet-one-sealed-header-derived-nonces-no-second-tag) H1). L1 (and a carrier-backed link, for its inner packets) hands it the packet's clear header, packet space and number:

```text
nonce (24 B) = direction (u8) || packet space (u8) || session_epoch (u32 LE) || packet number (u32 LE) || 14 zero bytes
sealed       = XChaCha20-Poly1305(key = k_send, nonce, aad = the clear header, plaintext = the L3 frame)
```

| Space | Value | Numbered by |
|-------|-------|-------------|
| BestEffort | 0 | L1, best-effort data |
| Reliable | 1 | L1, reliable data |
| CarrierBestEffort | 2 | a carrier-backed link, best-effort inner packets |
| CarrierReliable | 3 | a carrier-backed link, reliable inner packets |

The nonce is never sent. It is unique per key because a (space, number) seals one plaintext: a number is taken once, and a resend seals the same bytes again (the same ciphertext, which leaks nothing). `k_send` and `k_recv` are already per direction and per epoch; both are in the nonce anyway. **Open** verifies the tag (header included) and decrypts; failure → drop the packet (count it); nothing reaches L3. In a rekey's grace window a packet that fails under the current key is tried under the previous one.

Fixed known answers: `PacketCryptoTest.KnownAnswer` (pp-cpp-amp), cross-checked against libsodium.

## Rekey

Triggers (policy, configurable):

- Time bound (e.g. 24 h)
- Data volume bound
- Explicit `SessionRekey` control message on channel 0

**Wire (ch0 DATA, version `2` — capability payloads remain version `1`):**

| kind | Meaning |
|------|---------|
| `1` | `SessionRekeyRequest` + `u32` LE `target_epoch` (= local epoch + 1) |
| `2` | `SessionRekeyAck` + same `target_epoch` |

Responder sends **Ack at the current epoch**, then both sides call `ApplyRekey`. Receive path keeps the previous `k_recv` for **`kSessionRekeyGraceMs` (1000 ms)** so in-flight epoch-*N* ciphertext still opens. A reliable resend is resealed under the current key, so it never depends on the grace window.

**Invariant:** channels stay open across rekey; only keys and `session_epoch` change. After the grace window, stale epochs fail closed.

## Path migrate interaction

L1 `SetPeerEndpoint` does **not** reset Session. Session keys and `session_epoch` are keyed by `(local_peer_id, remote_peer_id, assoc_id)`, not UDP 4-tuple.

## Rate limiting (pre-decrypt)

Endpoint should drop obviously invalid pre-session garbage before expensive ML-KEM work. Per-source-IP and per-assoc counters — implementation detail; required for production UDP.

## Session vs Association lifecycle

| Event | Session |
|-------|---------|
| L1 path migrate | unchanged |
| L1 assoc Close | Session → Closed |
| MSH fail | no Session; assoc reset |
| PeerId mismatch in Finished | fail closed |

## Testing requirements

| Suite | Coverage |
|-------|----------|
| KAT / vectors | Transcript hash, KDF labels, Finished MAC |
| 2-peer MemoryIo | Full handshake, seal/open round-trip |
| Rekey | epoch bump, grace window |
| Downgrade | Tampered Finished rejected |
| Migrate | Session survives `SetPeerEndpoint` |

## Related ADRs

[A013](../DECISIONS.md#a013--l2-full-session-only) · [A015](../DECISIONS.md#a015--k_assoc-and-k_session-from-msh-transcript) · [A017](https://github.com/people-post/pp-browser/blob/main/projects/adp/DECISIONS.md#a017--libp2p-shrink-retain-crypto--peerid-only)
