# ADP — Association Datagram Protocol

> Moved from pp-browser `docs/contracts/ADP.md` at [3e407e3](https://github.com/people-post/pp-browser/blob/3e407e342320719794edd3025c0c7a632bb5f13a/docs/contracts/ADP.md) (pp-browser [CR025](https://github.com/people-post/pp-browser/blob/main/projects/capability-repos/DECISIONS.md#cr025--documentation-lives-with-what-it-describes-pp-specs-public-holds-shared-contracts-vectors-and-cross-repo-programs)). Links back into pp-browser point at its `main`.

**Status:** Wire v2 (pp-cpp-amp v4.0.0, [A029](../DECISIONS.md#a029--a-smaller-packet-one-sealed-header-derived-nonces-no-second-tag) H1). Normative wire for amp L1. A peer on wire v1 is refused (version byte); there is no bridge.

**AMP stack (L2–L4):** [AMP-SESSION.md](AMP-SESSION.md), [AMP-CHANNEL.md](AMP-CHANNEL.md), [STACK.md](../STACK.md).

**Design and history:** [DESIGN.md](../DESIGN.md) (L1), [DECISIONS.md](../DECISIONS.md); the app's adoption: pp-browser [projects/adp/](https://github.com/people-post/pp-browser/tree/main/projects/adp/)

## Role

L1 message-oriented UDP **connection**: best-effort and reliable delivery, path migration, and one packet per datagram. Once the handshake gives the session its keys, L1 seals data packets through the session (`IPacketProtection`; it never holds the keys). Before that, and for control packets, an HMAC binds the sender. Asio-free.

## Wire v2 (little-endian)

The first byte is the version (high nibble, `2`) and the type (low nibble). The association id follows: 8 B, chosen by the dialer (CSPRNG); the responder refuses an id already in use.

**Sealed data** (`DataBestEffort=0`, `DataReliable=1`), once the session has keys:

| Field | Size |
|-------|------|
| version / type | 1 |
| assoc_id | 8 |
| packet number | 4 (per space × direction; starts at 1) |
| ciphertext | N (the L3 frame, [AMP-CHANNEL.md](AMP-CHANNEL.md)) |
| tag | 16 |

29 B around the frame. The clear header is the AEAD's associated data and the packet number is its nonce ([AMP-SESSION.md § Sealing packets](AMP-SESSION.md#sealing-packets)): no timestamp, no length field (UDP has it), no HMAC. A sealed packet is opened before anything trusts it: the replay window, liveness and a path move follow only a packet that opened. A sealed packet for an unknown association id is dropped; a new association starts only from a plain handshake packet.

**HMAC packets** (`Ack=2`, `Close=3`, `Keepalive=4`, `PlainBestEffort=5`, `PlainReliable=6`):

| Field | Size |
|-------|------|
| version / type | 1 |
| assoc_id | 8 |
| seq | 4 (data: per space × direction, starts at 1; Ack: the acked seq; else 0) |
| timestamp_ms | 4 (truncated epoch ms; ±60 s skew) |
| payload | N |
| hmac | 16 (HMAC-SHA256 truncated, over the preceding bytes) |

33 B around the payload. Plain data is the handshake, under the well-known pre-session key; after the handshake the HMAC key is `K_assoc`. Once the session has keys, plain data is refused, except that a duplicate of the handshake's reliable packets is acked so its sender stops resending. Control packets have no replay window; a timestamp newer than any seen is their freshness.

**Version negotiation** ([N032](https://github.com/people-post/pp-browser/blob/main/projects/p2p-mesh/DECISIONS.md#n032--node-versions-claims-only-exclude-verification-promotes), pp-cpp-amp v4.1.0): the first-byte high nibble `0xF` is reserved for it in every wire version and is never a wire version. A node that takes dials answers a datagram of a version it does not speak with:

| Field | Size |
|-------|------|
| marker | 1 (`0xF0`) |
| echo | 16 (the first 16 B of the datagram it answers, zero padded) |
| count | 1 |
| versions | count × 1 (the wire versions it speaks) |

- **Bounded:** at most 16 replies a second per endpoint, never larger than the datagram answered, and a negotiation datagram is never answered.
- **Who takes it:** only a dialer still handshaking whose association id the echo names (the same place in every layout). It is unauthenticated, so it only means "stop dialing": the dial fails at once as `VersionMismatch` (link event reason `version-mismatch`) instead of timing out, and the mesh's reach record holds that peer down for its longest retry (30 min). A reply that lists our own version is ignored; an established association ignores it.

**Sizes:** a datagram is at most **1232 B**, IPv6's smallest MTU (1280) less the IPv6 and UDP headers, so it crosses any path whole. A sealed packet carries up to 1203 B, an HMAC packet up to 1199 B (`kMaxPayload`).

**Numbers:** best-effort and reliable packets are numbered apart (two spaces), so the space is part of the nonce. A resend goes as it first went (plain or sealed) under its own number, resealed under the current key: the same bytes unless a rekey came between. A number space that runs out (2³²) fails the association; the link redials with new keys.

## Addresses

`/ip4|ip6|dns|dns4|dns6/<host>/udp/<port>/adp/<wire>/p2p/<PeerId>` (pp-cpp-amp v5.0.0, [N032](https://github.com/people-post/pp-browser/blob/main/projects/p2p-mesh/DECISIONS.md#n032--node-versions-claims-only-exclude-verification-promotes) item 3). `<wire>` is the wire version the address speaks (`2`); a node writes its own. Wherever an address comes from (the directory, a contact, mDNS, a seed list, channel 0), a reader:

- **our version:** dials it;
- **another version, or none** (the versionless `/adp/1.0.0/` of addresses stored before item 3): refuses it at parse (`ParseAdpMultiaddr`), so it is skipped like an unparsable address and never dialed.

An mDNS announcement says its version too (TXT `wire=<N>` beside `peer=`); a browser takes only ours. A reader on pp-cpp-amp before v5.0.0 refuses `/adp/2/`.

## Delivery

- **BestEffort** — no retransmit
- **Reliable** — ACK + retransmit on injectable clock

## I/O

`DatagramIo` + `MemoryDatagramIo` (tests) + `OsUdpDatagramIo` (UDP sockets). No Asio dependency on `pp_amp_l1` (`pp_base_adp` in pp-browser before the extraction).
