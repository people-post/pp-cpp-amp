# AMP Channel — L3 normative contract

> Moved from pp-browser `docs/contracts/AMP-CHANNEL.md` at [3e407e3](https://github.com/people-post/pp-browser/blob/3e407e342320719794edd3025c0c7a632bb5f13a/docs/contracts/AMP-CHANNEL.md) (pp-browser [CR025](https://github.com/people-post/pp-browser/blob/main/projects/capability-repos/DECISIONS.md#cr025--documentation-lives-with-what-it-describes-pp-specs-public-holds-shared-contracts-vectors-and-cross-repo-programs)). The protocols that run on channels (the protocol-id map, the call-media bundle, the circuit tunnel's requests) are pp-browser's and pp-cpp-mesh's and stayed in pp-browser (§ Protocols on channels). Links back into pp-browser point at its `main`.

**Status:** Foundation spec (2026-08-30). Normative for amp L3 (`include/amp/L3/`, `src/L3/`; `lib/amp/L3/` in pp-browser before the extraction).
**Stack:** [STACK.md](../STACK.md) · L2 [AMP-SESSION.md](AMP-SESSION.md) · L1 [ADP.md](ADP.md)  
**Version axes:** the ADP wire version (a packet's first byte), per-channel `protocol_id`

## Role

L3 **AMP Channel** replaces Yamux + app `StreamFrameIo` framing:

- Multiplex logical conversations on one **Session**
- Map channel class → ADP QoS ([STACK.md § QoS](../STACK.md#qos-mapping-fixed-at-channel-open))
- Fragment/reassemble large L4 messages (up to app max, e.g. 256 KiB)
- Port `StreamIoPolicy` semantics (drop oldest, read_once, timeouts)

L2 AEAD wraps **L3 frame bytes**. L4 payloads inside L3 DATA frames are unchanged ([WIRE_SCHEMAS.md](https://github.com/people-post/pp-browser/blob/main/docs/contracts/WIRE_SCHEMAS.md)).

## Channel identifiers

| Id | Reserved use |
|----|----------------|
| **0** | Capability / identify plane ([A016](../DECISIONS.md#a016--channel-0--capability--identify-plane)) |
| **1 … 0xFFFF_FFFE** | Dynamic; allocated by opener |
| **0xFFFF_FFFF** | Illegal |

`protocol_id` is a UTF-8 string (e.g. `/pp/rpc/chat/1.0.0`) carried in OPEN.

L4 ids name **conversation kinds**, not product features — complete set and “when to mint a new id” gate: [L4_PROTOCOL_KINDS.md](https://github.com/people-post/pp-browser/blob/main/docs/contracts/L4_PROTOCOL_KINDS.md) ([A028](https://github.com/people-post/pp-browser/blob/main/projects/adp/DECISIONS.md#a028--l4-protocol-kinds--seven-conversation-shapes)).

## Channel classes

| Class | ADP QoS | Duplex | Typical `protocol_id` |
|-------|---------|--------|------------------------|
| **Transactional** | Reliable | half; `read_once` | `/pp/rpc/chat/1.0.0`, `/pp/rpc/history/1.0.0` |
| **Control** | Reliable | half or full | `/pp-mesh/reach/1.0.0`, ch0 |
| **Bulk** | Reliable | half | `/pp/blob/1.0.0` |
| **Realtime** | BestEffort | full | Opus/media relay media frames |
| **RealtimeControl** | Reliable | full | call-media hello/teardown on same call |

Class is fixed at OPEN and selects ADP QoS for the channel lifetime.

## L3 frame types

All multi-byte integers **little-endian** unless noted.

### Common header

9 B, sealed inside the packet (wire v2, [A029](../DECISIONS.md#a029--a-smaller-packet-one-sealed-header-derived-nonces-no-second-tag) H1): a path observer sees neither channel nor seq. The packet's first byte carries the wire version; the frame has none.

| Field | Size | Notes |
|-------|------|-------|
| `frame_type` | 1 | see below |
| `channel_id` | 4 | u32 |
| `channel_seq` | 4 | per-channel, per-direction; Reliable channels only |

### Frame types

| Value | Name | Payload |
|-------|------|---------|
| 0 | `Open` | `protocol_id` LenUtf8, `channel_class` u8, `flags` u16 |
| 1 | `OpenAck` | `result` u8 (0=ok) |
| 2 | `Data` | L4 bytes or fragment (see § Fragmentation) |
| 3 | `Close` | optional reason LenUtf8 |
| 4 | `Reset` | error code u32 |
| 5 | `Frag` | fragmentation meta + chunk (Reliable bulk/large txn) |

**LenUtf8:** `u32` LE byte count + UTF-8 (same profile as [WIRE_SCHEMAS § pp Binary Wire Profile](https://github.com/people-post/pp-browser/blob/main/docs/contracts/WIRE_SCHEMAS.md#pp-binary-wire-profile-d088) but LE length for L3).

After the link opens the packet, the parser dispatches on `frame_type` and enforces per-channel ordering for Reliable.

## Channel lifecycle

```text
        OPEN ──────────────► OPEN_ACK (ok)
          │                      │
          ▼                      ▼
       DATA ◄────────────────► DATA
          │                      │
    CLOSE / RESET            CLOSE / RESET
          │                      │
          ▼                      ▼
       terminal               terminal
```

| Event | Scope |
|-------|--------|
| **RESET** | This channel only; drop queued outbound |
| **CLOSE** | Graceful; Reliable may drain |
| **Session fail** | All channels on peer |

### Failure propagation

Matches [STACK.md § Failure propagation](../STACK.md#failure-propagation).

## Channel 0 — capability plane

Opened immediately after Session `Established`.

**Dialer (outbound link) opens channel 0** and sends a capability payload; **responder OpenAcks and replies** with its own payload on the same channel ([A016](../DECISIONS.md#a016--channel-0--capability--identify-plane)). Wired from `PeerLinkManager::OnLinkEstablished`.

**Payload (Transactional / Control DATA)**, leading byte `4` (`2` is session control; `1`, the payload before it said what the node runs, and `3`, that alone, are retired and refused):

- `local_peer_id`
- `listen_multiaddrs[]`
- `protocols[]` supported (includes circuit / media-relay protocol ids when hosted)
- what the node runs ([N032](https://github.com/people-post/pp-browser/blob/main/projects/p2p-mesh/DECISIONS.md#n032--node-versions-claims-only-exclude-verification-promotes)): `amp_wire` (u8), `node_version` (len-prefixed UTF-8), `features[]` (string list). pp-node (and a desktop in the node role) fills `node_version` with its build's version; an app in the client role sends it empty. `RemoteCapability(peer_id)` reads a connected peer's.

A decoder ignores bytes past the fields it knows (a later version's). Until 2026-11-12 the node's claims went as a second message (leading byte `3`) so a peer before them could drop it; with no bridges pre-release they ride the payload.

Channel 0 may stay open (long-lived control) or close after exchange — implementation policy; default **long-lived, Reliable Control**.

Replaces libp2p Identify stream on the AMP path.

## Fragmentation (L3)

**A link's frame budget** decides when a message fragments: the largest frame its transport carries in one packet (`ChannelMux::SetMaxFrameBytes`). A direct ADP link carries one sealed packet's payload, **1203 B** (a DATA frame up to 1194 B of message, fragments of 1178 B). A carrier-backed (nested) link carries what rides one DATA frame of its outer link whole, less the inner packet's lane wrapper, header and tag: **1168 B** (1143 B per fragment), so the outer link never fragments an inner packet again. L3 fragments larger L4 messages:

| Field (Frag payload) | Size |
|----------------------|------|
| `msg_id` | 8 |
| `frag_index` | 2 |
| `frag_count` | 2 |
| `total_len` | 4 |
| `chunk` | remainder |

- **Reliable** channels: reassemble in order; dup `msg_id`+index dropped; incomplete assembly times out (default **30 s**).
- **BestEffort** (optional, video): may skip missing frags; out of scope for v1 control paths.

Max reassembled size defaults match today’s stream caps:

| Policy | Default max |
|--------|-------------|
| Control JSON | 256 KiB |
| Bulk (blob) | per `AmpChannelLimits::kMaxBulkFrameBytes` |
| Call media | per `AmpChannelLimits` / product policy |

## Channel policies (port StreamIoPolicy)

Core factories: pp-cpp-amp `amp/L3/ChannelPolicy.h` (Amp owns Bulk class + size). Product L4 factories: pp-cpp-mesh `mesh/l4/shared/ProductChannelPolicies.h` (product-only extras + `read_once` / timeouts). See [pp-cpp-amp OWNERSHIP](../OWNERSHIP.md).

| Factory | Class | `read_once` | Read timeout | Home |
|---------|-------|-------------|--------------|------|
| `CallMediaChannelPolicy` | Realtime | no | off | core |
| `CallMediaControlChannelPolicy` | RealtimeControl | no | off | product |
| `MediaRelayDataChannelPolicy` | Realtime | no | off | product |
| `MediaRelayClientChannelPolicy` | RealtimeControl | no | off | product |
| `CircuitTunnelChannelPolicy` | Control | no | off | product |
| `CircuitCarrierChannelPolicy` | Realtime | no | off | core |
| `ControlJsonChannelPolicy` | Control | yes | 8 s (callers pass their deadline) | core |
| `MakeBulkChannelPolicy` / `BulkChannelPolicy()` | Bulk | no | off | **core** |
| `BulkChannelPolicy(bool)` (wrap + timeout) | Bulk | configurable | 8 s (client: remaining operation time; inbound: 15 s) | product |

No policy queues or drops outbound (pp-cpp-amp v6.0.0): a session sends each payload to its mux at once; a reliable frame waits there for window space, a best-effort one goes to the link. The drop-oldest caps (64 call-media, 2 hop, 6 client, 1 control) never filled and are gone.

**Read timeout is enforced** (pp-cpp-amp v2.15.0+): a channel with nothing *inbound*
for that long is reset (`read-timeout` to the local side, RESET to the peer); the clock
starts at open / bind and restarts on every inbound DATA or FRAG, not on sends. So:

- **Long-lived legs have it off** — call control and media, media-relay attach and hop,
  circuit tunnels and carriers. Each is legitimately silent on one side for long
  stretches (standby-path heartbeat 10 s, a publisher gets nothing back, reservations,
  one-way splices); their coordinators own handshake deadlines and liveness.
- **Request/response channels keep it**, sized to the slowest legitimate reply: the
  inbound side of a request answered by a worker (`InboundReplyPolicy`) waits for the
  whole job — dial-back serving waits for its sequential target walk (4 × 15 s + 5 s);
  an inbound blob fetch drains its reply outbound-only after `Close`, so it gets the
  client's 15 s operation deadline.

Checked on the mux tick (`MeshPump` io thread), so it fires up to one tick late.

**State tied to a channel is released in its closed callback.** A channel's end — the peer's
CLOSE / RESET, a read timeout, the link dropping, a failed write, a local close — reaches L4
**only** through the closed callback (never as a frame error), and peers usually just close
instead of sending a goodbye message. Anything kept per channel (a relay participant, a tunnel,
an in-flight request, a placeholder) must be released there or by a deadline. Since pp-cpp-amp
v2.16.0 a closed session also drops its handlers, so a handler may hold the session (and
since v2.16.1 it forgets its mux, so it may outlive its link); and dial
keys registered for one job are dropped with `UnregisterEndpoint` when it ends
([pp-cpp-amp STACK § Lifetimes](../STACK.md)).
The closed callback can run while the component's own lock is held (a write that fails inside
`EnqueueOutbound`): post the release to the io strand rather than locking inline.

## Protocols on channels

What runs on a channel is the embedder's, not this library's ([OWNERSHIP](../OWNERSHIP.md)). pp-browser's [AMP-CHANNEL](https://github.com/people-post/pp-browser/blob/main/docs/contracts/AMP-CHANNEL.md) keeps them: the map of `protocol_id`s to their L4 shapes ([§ L4 mapping](https://github.com/people-post/pp-browser/blob/main/docs/contracts/AMP-CHANNEL.md#l4-mapping-unchanged-payloads)), the app's call-media bundle with its path migration, failover and end of a call ([§ Call-media bundle](https://github.com/people-post/pp-browser/blob/main/docs/contracts/AMP-CHANNEL.md#call-media-bundle)), and pp-cpp-mesh's circuit tunnel requests ([§ Circuit tunnel (v1)](https://github.com/people-post/pp-browser/blob/main/docs/contracts/AMP-CHANNEL.md#circuit-tunnel-v1)). The kinds of conversation and the gate for a new `protocol_id`: pp-browser [L4_PROTOCOL_KINDS](https://github.com/people-post/pp-browser/blob/main/docs/contracts/L4_PROTOCOL_KINDS.md).

## Nested Session carrier ([A024](../DECISIONS.md#a024--amp-call-media-over-circuit--nested-session))

For Amp **call-media** over circuit, the bridged channel is **not** the product L4 pipe. Outer `target_protocol` is Amp `kAmpCircuitCarrierProtocolId` (`/pp-amp/circuit-carrier/1.0.0`). After splice:

1. A and B run **inner MSH** over the carrier (`PeerLink` carrier mode; non-chunked `AmpAdpCarrier` MSH/sealed frames as ChannelSession DATA).
2. Inner `Session` + `ChannelMux` becomes a normal PeerLink; `OpenChannel` opens A021 control+media on that mux.
3. Outer policy: `CircuitCarrierChannelPolicy` (BestEffort Realtime) so media FRAG bursts are not stalled by ADP `reliable_window`.

**Carrier payloads** (one per carrier DATA message, first byte the kind):

| Kind | Layout | Use |
|------|--------|-----|
| `0` Msh / `2` MshChunk | `[kind][msh type][body]` | inner handshake: a message that fits one frame as `Msh`, a larger one as `MshChunk` pieces (first send and resends alike) |
| `1` Sealed / `5` SealedReliable | `[kind][number u32 LE][frame ciphertext][tag 16]` | inner data, sealed by the inner session ([AMP-SESSION.md § Sealing packets](AMP-SESSION.md#sealing-packets)): spaces CarrierBestEffort / CarrierReliable, `[kind][number]` the associated data |
| `3` LaneData | `[kind][lane seq u32 LE][inner payload]` | the reliable lane around a SealedReliable packet |
| `4` LaneAck | `[kind][cumulative u32][selective u64]` | lane acks (every nested link speaks the lane: no probe, no negotiation) |

Each inner space keeps a replay window (best effort 128, reliable 1024): a relay between the ends could replay an inner packet, and the window turns it away. An inner frame is at most the nested link's budget (§ Fragmentation), so a sealed inner packet rides one outer DATA frame whole.

A media-relay hop reachable only through a circuit is reached the same way: a nested Session, on which the hop's control channel and its data lane open as on a direct link (pp-cpp-mesh v7.0.0; the single adopted tunnel channel of D9 5c is gone). See [CALL_MEDIA_CIRCUIT.md](https://github.com/people-post/pp-browser/blob/main/projects/adp/CALL_MEDIA_CIRCUIT.md).

## Three objects (do not collapse)

| Object | Type |
|--------|------|
| **ChannelSession** | L3 pipe — replaces `DuplexFrameSession` |
| **PeerLinkManager** | peer link — replaces dial + warm in `PeerSessionManager` |
| **Domain session** | LiveCall, thread store, relay participant |

## Ownership hierarchy ([A027](../DECISIONS.md#a027--parent-only-destroy-l3l4-ownership-hierarchy))

Mesh specialization of the repo-wide **parent-only destroy** rule ([OWNERSHIP.md](https://github.com/people-post/pp-browser/blob/main/docs/architecture/OWNERSHIP.md)). Only the **parent** may destroy a child. Children may **request** close; they must not drop the durable owner mid-dispatch.

```text
PeerLinkManager                 // owns PeerLink by key; elects/drops links (A026)
  └── PeerLink                  // owns ChannelMux
        └── ChannelMux          // owns channel records + handler slots
              └── ChannelSession  // durable owner = L4 slot (bundle / relay Session / client_)
                    └── on_frame_ / on_closed_   // non-owning; signal only
```

| Rule | Meaning |
|------|---------|
| **Single durable owner** | One clear `shared_ptr`/`unique_ptr` slot owns each `ChannelSession`. Extra `shared_ptr`s are **dispatch pins** only (Bind holds a lock for the current mux callback). |
| **Request ≠ destroy** | `CloseQuiet`, `return false` from `on_frame_`, and `on_closed` reasons are requests. Parent decides TearDown / erase / unbind. |
| **No destroy-from-callback** | Frame/terminal callbacks must not erase the owning map entry or `reset()` the owning slot in a way that can drop the last durable ref before the callback returns. Prefer flag / deferred drop after dispatch (or next io tick). |
| **Parent unbinds** | `ReleaseHandlers` / `SetDataHandler({})` is a parent step after close. `~ChannelSession` unbind is a safety net only. |
| **No use-after-erase** | Copy fields out, then `erase(id)` — never touch the erased object (`AdoptClientChannel` pattern). |
| **Callbacks non-owning** | Bind captures `weak_ptr` (or parent id + lookup), not a strong cycle into the L4 parent. |
| **PeerLink parent-only** | Manager drops loser links ([A026](../DECISIONS.md#a026--one-session-per-peerid-under-dual-dial-mesh-election)). L4 must not keep a long-lived raw `PeerLink*`; resolve via `peer_key`. |

**Mux:** copy handlers before invoke so parent unbind mid-callback does not destroy the running `std::function`. Mux never owns `ChannelSession`.

**Transitional:** some L4 paths still TearDown inside `on_frame_`; Bind’s dispatch pin makes that safe. Target shape remains signal → parent drop after dispatch.

## Threading

`ChannelSession` is **io-thread affine**. Heavy work posts to worker pool ([THREADING.md](https://github.com/people-post/pp-browser/blob/main/docs/architecture/THREADING.md)).

## Testing requirements

| Suite | Coverage |
|-------|----------|
| OPEN/ACK/CLOSE/RESET | single + parallel channels |
| QoS map | Reliable channel never sends BE ADP |
| Fragmentation | loss, reorder, dup, timeout |
| ch0 | capability round-trip |
| Policy | read_once, 8 s timeout; a best-effort session sends at once (no queue) |
| Isolate | RESET does not kill sibling channel |
| Ownership canaries | DualDial call-media (`CallMediaLeg*`); media-relay adopt + `Subscribe` after attach (`LocalHopFanoutRoundTrip`) |

## Related ADRs

[A014](../DECISIONS.md#a014--one-association-per-peer-pair) · [A016](../DECISIONS.md#a016--channel-0--capability--identify-plane) · [A018](../DECISIONS.md#a018--fragmentation-at-l3-not-l1) · [A019](../DECISIONS.md#a019--circuit-relay--channel-tunnel) · [A020](https://github.com/people-post/pp-browser/blob/main/projects/adp/DECISIONS.md#a020--single-transport-entry-per-protocol) · [A026](../DECISIONS.md#a026--one-session-per-peerid-under-dual-dial-mesh-election) · [A027](../DECISIONS.md#a027--parent-only-destroy-l3l4-ownership-hierarchy)
