# ADP — design (L1 wire)

> Moved from pp-browser `projects/adp/DESIGN.md` at [3e407e3](https://github.com/people-post/pp-browser/blob/3e407e342320719794edd3025c0c7a632bb5f13a/projects/adp/DESIGN.md) (pp-browser [CR025](https://github.com/people-post/pp-browser/blob/main/projects/capability-repos/DECISIONS.md#cr025--documentation-lives-with-what-it-describes-pp-specs-public-holds-shared-contracts-vectors-and-cross-repo-programs)). Links back into pp-browser point at its `main`.

**Scope:** L1 only. Full stack: [STACK.md](STACK.md). L2/L3 contracts: [AMP-SESSION.md](contracts/AMP-SESSION.md), [AMP-CHANNEL.md](contracts/AMP-CHANNEL.md).

**Related ADRs:** [DECISIONS.md](DECISIONS.md). **Phases:** [PHASES.md](https://github.com/people-post/pp-browser/blob/main/projects/adp/PHASES.md).

## Threat model (L1 only)

| Adversary | Protected by |
|-----------|--------------|
| Inject / spoof datagrams on path | 16-byte HMAC over header+payload with `K_assoc` |
| Replay | Per-direction per-QoS seq + replay window |
| Stale / off-path delayed packets | Timestamp skew window |

Confidentiality and authenticity of **content** are L2+ (AMP Session AEAD, app E2E / call crypto). ADP L1 HMAC is sender-binding only ([A002](DECISIONS.md#a002--l1-hmac-binding-vs-l2-crypto)).

## Stack

```
L2 opaque payload (Noise / AEAD / RPC / Opus)
 └─ ADP Connection (message-oriented)
     ├─ BestEffort  — no retransmit
     └─ Reliable    — ACK + rtx (virtual clock)
 └─ Endpoint demux (many assocs / one DatagramIo)
 └─ DatagramIo (Memory | OsUdp)
```

## Wire v1 (little-endian)

*History: superseded by wire v2 (pp-cpp-amp v4.0.0, [A029](DECISIONS.md#a029--a-smaller-packet-one-sealed-header-derived-nonces-no-second-tag)); the wire now is [ADP.md](contracts/ADP.md).*

| Field | Size | Notes |
|-------|------|-------|
| `version` | 1 | `1` |
| `flags` | 1 | see packet types |
| `assoc_id` | 16 | connection id |
| `seq` | 4 | starts at 1; separate spaces per QoS × direction |
| `timestamp_ms` | 4 | truncated epoch ms |
| `payload_len` | 2 | payload bytes |
| `payload` | N | opaque; max **1200** |
| `hmac` | 16 | HMAC-SHA256 truncated; covers all preceding bytes |

**Packet types (`flags` low nibble):**

| Value | Name |
|-------|------|
| 0 | `DataBestEffort` |
| 1 | `DataReliable` |
| 2 | `Ack` (payload empty; `seq` = acked reliable seq) |
| 3 | `Close` |

Fixed header without HMAC = 28 bytes; max datagram = 28 + 1200 + 16 = 1244.

## API (namespace `pp::adp`)

- `Connection::Open` / `Close`
- `SetPeerEndpoint` / `PeerEndpoint` (path migrate)
- `Send(QosClass, span)` — `BestEffort` | `Reliable`
- `OnMessage` — whole messages only
- `LooksAlive(now)` — recent authenticated rx
- `Endpoint` — demux; host calls `Pump()`
- `DatagramIo` — `SendTo` / `RecvFrom`; `MemoryDatagramIo` / `OsUdpDatagramIo`
- `Clock` — injectable; tests `Advance`

## NAT

Client speaks first. Server/hop replies to the **observed** source address. Dual-NAT peer↔peer needs punch/relay (out of scope for foundation).

## I/O

**No Asio** in `pp_base_adp`. Correctness tests use `MemoryDatagramIo` + virtual clock. `OsUdpDatagramIo` is thin POSIX/Winsock for production + one loopback smoke suite.
