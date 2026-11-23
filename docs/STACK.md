# STACK

Amp's big picture and layer map: the four layers and the objects they hold, how failures travel, one association per peer, QoS, security, the capability channel, fragmentation, circuits, addressing; then the entry points an embedder uses and the lifetimes it can rely on. Normative detail: [ADP](contracts/ADP.md) (L1), [AMP-SESSION](contracts/AMP-SESSION.md) (L2), [AMP-CHANNEL](contracts/AMP-CHANNEL.md) (L3); why: [DECISIONS.md](DECISIONS.md).

> The big picture (§ Names to § Non-goals, and § Related docs) moved from pp-browser `projects/adp/STACK.md` at [3e407e3](https://github.com/people-post/pp-browser/blob/3e407e342320719794edd3025c0c7a632bb5f13a/projects/adp/STACK.md) (pp-browser [CR025](https://github.com/people-post/pp-browser/blob/main/projects/capability-repos/DECISIONS.md#cr025--documentation-lives-with-what-it-describes-pp-specs-public-holds-shared-contracts-vectors-and-cross-repo-programs)) and is merged here with this file's layer map and product entry. It is the stack as designed in D0 (2026-08-30); *Since D0* / *Now* notes say where it has changed since. Links back into pp-browser point at its `main`.

**Status:** Foundation spec (2026-08-30)  
**Owner:** Hongwei + agents  
**Related:** [ADP.md](contracts/ADP.md) (L1), [AMP-SESSION.md](contracts/AMP-SESSION.md) (L2), [AMP-CHANNEL.md](contracts/AMP-CHANNEL.md) (L3), [L4_PROTOCOL_KINDS.md](https://github.com/people-post/pp-browser/blob/main/docs/contracts/L4_PROTOCOL_KINDS.md) (L4 kinds / [A028](https://github.com/people-post/pp-browser/blob/main/projects/adp/DECISIONS.md#a028--l4-protocol-kinds--seven-conversation-shapes)), [DECISIONS.md](DECISIONS.md) (the transport's; the app's stay in pp-browser)

## Names

| Name | Scope |
|------|--------|
| **ADP** | L1 wire only — Association Datagram Protocol (`src/lib/amp/L1/`, `pp_base_adp`) |
| **AMP** | Full four-layer peer mesh stack built on ADP — **A**ssociation **M**esh **P**rotocol |

When migrating off TCP/QUIC/Yamux, say **AMP** or **ADP stack**. Reserve **ADP** for the UDP datagram layer.

## North star

Replace the product peer underlay (**TCP + Noise + Yamux**) with **UDP + AMP (L1–L3)**. Application payloads ([WIRE_SCHEMAS.md](https://github.com/people-post/pp-browser/blob/main/docs/contracts/WIRE_SCHEMAS.md)) stay at L4 unchanged.

Libp2p shrinks to retained crypto/identity utilities; `Host::newStream` and stream transports retire from the product path ([A017](https://github.com/people-post/pp-browser/blob/main/projects/adp/DECISIONS.md#a017--libp2p-shrink-retain-crypto--peerid-only)).

## Four layers

```text
┌─────────────────────────────────────────────────────────┐
│  L4  Application protocols (kinds: identify / discover /│
│      reach / circuit / rpc / blob / realtime — A028)    │
│      /pp/* /pp-mesh/* + WIRE_SCHEMAS payloads    │
└─────────────────────────┬───────────────────────────────┘
                          │ messages ≤ app max (e.g. 256 KiB)
┌─────────────────────────▼───────────────────────────────┐
│  L3  AMP Channel                                        │
│      mux, OPEN/CLOSE/RESET, fragmentation, QoS policy   │
│      channel 0 = capabilities / identify                │
└─────────────────────────┬───────────────────────────────┘
                          │ AEAD plaintext (L2-full)
┌─────────────────────────▼───────────────────────────────┐
│  L2  AMP Session (FULL)                                 │
│      MSH handshake, PeerId bind, session AEAD, rekey    │
│      derives K_assoc → L1 HMAC                          │
└─────────────────────────┬───────────────────────────────┘
                          │ ADP payloads (≤ 1200 B per datagram)
┌─────────────────────────▼───────────────────────────────┐
│  L1  ADP Wire v1                                        │
│      HMAC bind, BestEffort + Reliable, path migrate     │
└─────────────────────────┬───────────────────────────────┘
                          │ UDP
                    PeerLinkManager
              (dial, warm, backoff, address book)
```

**Horizontal (not a layer):** `PeerLinkManager` — who to dial, warm/cold policy, circuit escalation, preferred multiaddrs. Evolves from today’s `PeerSessionManager`.

*Since D0:* the diagram is the stack as designed (wire v1). Since v4.0.0 L1, the L2 envelope and the L3 frame header are one packet sealed once: a datagram is at most 1232 B, a sealed packet carries no HMAC and `K_assoc` keys only the handshake and control packets' HMAC (§ [Wire v2](#wire-v2-since-v400), [A029](DECISIONS.md#a029--a-smaller-packet-one-sealed-header-derived-nonces-no-second-tag)). The layers' headers and roles now: § [Layers](#layers).

## Layers

| Layer | Headers | Role |
|-------|---------|------|
| L1 ADP | `amp/L1/` | UDP association, Reliable/BestEffort, sealed data packets (through `IPacketProtection`), HMAC for the handshake and control packets |
| L2 MSH | `amp/L2/` | Session handshake, the packet AEAD (nonce from the packet number), rekey, session control |
| L3 mux | `amp/L3/` | Channel OPEN/CLOSE, FRAG to the link's frame budget, Bulk credit, capability codec |
| Link | `amp/link/` | Dial book, PeerLink, MeshRuntime / MeshPump |

Dependencies: `pp-cpp-common` + `pp-cpp-crypto` only. No product L4.

### Wire v2 (since v4.0.0)

One packet, sealed once. A data packet is the version/type byte, the 8 B association id and the packet number in
the clear, then the frame sealed with the session's keys: XChaCha20-Poly1305, the nonce derived from direction, packet
space, key epoch and packet number (never sent), the clear header as associated data — 29 B around the frame. The frame
(type, channel id, channel seq, fragment fields) travels sealed. Handshake and control packets (ack, close, keepalive)
keep a sequence number, a timestamp and an HMAC (33 B). The largest datagram is 1232 B, IPv6's smallest MTU less its
headers; a link's mux fragments to what one packet carries (`ChannelMux::SetMaxFrameBytes`). A carrier-backed link
seals its inner packets the same way, `[kind][number]` in the clear, each space with its own replay window. Peers on
wire v1 are refused. Design: [A029](DECISIONS.md#a029--a-smaller-packet-one-sealed-header-derived-nonces-no-second-tag).

### Release compatibility: readers tolerate, refusals say why

Additive on the wire in both directions with v6.0.0 (pp-browser `projects/release-compatibility/INVENTORY.md`
T9, U4, U5, U6); no version byte changed.

- **L3 frames (T9).** OPEN, OpenAck, CLOSE and RESET ignore bytes past the fields they know (new fields append). A
  frame type the reader does not know gets a RESET of its channel with code `ChannelMux::kResetCodeUnknownFrameType`
  (a bare RESET when the reader has no such channel; dropped on channel 0, the link's own). A new frame type must not
  use the channel seq, and must be optional or used only once the peer's offer says it is understood: a v6.0.0
  reader drops it silently. The RESET code reaches the terminal reason: the default code (1, every reset so far)
  still reads `"peer_reset"`; others read `"peer_reset: unknown-frame-type"` or `"peer_reset: code N"` — match with
  `ChannelMux::PeerResetCode`. Amp's own codes are from `0x1'0000`; the others are the product's.
- **MSH (T9, U4).** A message type the handshake does not know fails it at once
  (`MshAdpHandshake::kErrUnknownMessageType`, `HandshakeFailed`) instead of waiting for the timeout. A hello of
  another MSH version fails as `kErrUnsupportedVersion`, reported as `VersionMismatch` (not `HandshakeFailed`) on
  either side.
- **The ADP Close says why (U4).** Its payload, empty in v6.0.0 and ignored by a v6.0.0 reader, carries an
  `adp::CloseReason`: `TableFull` (refused, or an idle link evicted for room), `Admission`, `HandshakeFailed`,
  `UnsupportedMshVersion` (followed by `[count][versions]`, the ones the closer speaks) and `Shutdown`; a close no
  reason fits sends none. A dialer maps each (`Connection::PeerClose`, `PeerLink::FailRefused`): table full,
  admission and shutdown stay `TransportFailed`, a handshake failure is `HandshakeFailed`, an unsupported MSH
  version `VersionMismatch`; no reason (a v6.0.0 peer) reads as before. `LinkEvent::peer_close_reason` carries it.
- **Who must update (U5).** A `VersionMismatch` — the peer's version negotiation (wire), its hello or its Close (MSH)
  — sets `LinkEvent::version_mismatch` (`VersionMismatchInfo`: layer, ours, theirs, `VersionSkew` `PeerNewer` /
  `PeerOlder` / `Unknown`) on the `DialFailed` and `Dropped` events, and the error text says which side updates.
- **Channel 0's offer (U6).** Leading byte 4 is frozen; fields append. An offer that does not decode is recorded
  (`PeerLinkManager::UnreadableCapability`, `MeshRuntime::UnreadableCapability`: "unreadable offer, byte N"), and
  the responder still answers with its own offer.

## Object hierarchy

Carry forward the three-object discipline from [LIBP2P_STREAMS.md](https://github.com/people-post/pp-browser/blob/main/docs/architecture/LIBP2P_STREAMS.md) (pipe vs peer link vs domain session):

| Object | Question | Lifetime | Notes |
|--------|----------|----------|-------|
| **Endpoint** | UDP I/O + L1 demux? | Process | One or more per host; `Endpoint::Pump()` |
| **Association** (L1) | Which `assoc_id` + UDP path? | Until L1 Close / fatal error | Path migrate via `SetPeerEndpoint` |
| **Session** (L2) | Which **PeerId** + crypto epoch? | Until session fail or explicit close | Survives path migrate |
| **Channel** (L3) | Which **protocol / conversation**? | Until RESET or CLOSE | Siblings independent |
| **Domain session** (L4) | What product work? | Call, thread, relay hop | Never on the wire |

### Failure propagation

| Event | Effect |
|-------|--------|
| **Channel RESET** | That channel only; session + siblings continue |
| **Channel CLOSE** | Graceful end; drain or abort per policy |
| **Session rekey** | Channels stay open; new `session_epoch` |
| **Path migrate** (L1) | Session + channels unchanged |
| **Session fail** | All channels on that peer torn down |
| **Association Close** | Session ends |
| **Endpoint stop** | Everything on that socket |

Domain state (call roster, SQLite, jitter buffers) lives on L4 consumers — not on Channel/Session objects.

**Error escalation** follows the same hierarchy: each owner wraps its immediate child's
`CodedFailure` (code remap + `AppendFrom` on `message`). See
[CODED_FAILURE.md](https://github.com/people-post/pp-browser/blob/main/docs/contracts/CODED_FAILURE.md); link-level code table in
[AMP-LINK-ERRORS.md](https://github.com/people-post/pp-browser/blob/main/docs/contracts/AMP-LINK-ERRORS.md).

## One association per peer pair (default)

**One long-lived Association + one Session per remote PeerId**, many Channels on top ([A026](DECISIONS.md#a026--one-session-per-peerid-under-dual-dial-mesh-election)).

| Anti-pattern | Why avoid |
|--------------|-----------|
| New assoc per chat message | Handshake + NAT churn |
| Separate assocs for media vs control | Double punch / double bind |
| Assoc per call | Breaks warm-peer policy |
| Keep both dual-dial links Connected | Two muxes → L4 TearDown / dangling `ChannelSession` |

**Dual-dial:** both sides may dial briefly; mesh **elects** one Connected link (higher base58 PeerId’s outbound wins when both initiated; else keep already-Connected), then drops the loser **on Tick** (never `DropLink` the existing winner’s rival mid-`OnLinkEstablished` — UAF). Call-media OPEN glare stays on that single mux ([A021](https://github.com/people-post/pp-browser/blob/main/projects/adp/DECISIONS.md#a021--call-media--channel-bundle-on-meshruntime)).

**Ownership:** only the parent destroys a child (manager → link/mux → `ChannelSession` → callbacks signal only) — repo-wide [OWNERSHIP.md](https://github.com/people-post/pp-browser/blob/main/docs/architecture/OWNERSHIP.md), mesh [A027](DECISIONS.md#a027--parent-only-destroy-l3l4-ownership-hierarchy), [AMP-CHANNEL § Ownership](contracts/AMP-CHANNEL.md#ownership-hierarchy-a027).

Media vs control separation uses **channels + QoS**, not separate UDP associations.

## QoS mapping (fixed at channel OPEN)

| L3 channel class | ADP QoS | Examples |
|------------------|---------|----------|
| Transactional, Control, Bulk | **Reliable** | chat, history, blob, dial-back, call hello/teardown |
| Realtime media | **BestEffort** | Opus, relay media frames |
| Realtime signaling on a call | **Reliable** | same call may use both classes on different channels |

**Rule:** L3 Reliable channels send only on ADP Reliable. Realtime media sends only on ADP BestEffort. Do not stack reliable byte streams on top of unreliable ADP for control.

Port today’s `StreamIoPolicy` concepts (`CallMediaIoPolicy`, `ControlJsonIoPolicy`, …) to **channel policies** at L3.

## L2-full security (summary)

- **MSH** (Message Session Handshake): PQ peer authentication + forward secrecy — message-native transcript; semantic reference [libp2p-pq-transport](https://github.com/people-post/pp-browser/blob/main/projects/_archive/libp2p-pq-transport/DESIGN.md) (ML-KEM-768 + ML-DSA-65).
- **Session profile:** **full only** on the product path ([A013](DECISIONS.md#a013--l2-full-session-only)).
- **Key separation** ([A015](DECISIONS.md#a015--k_assoc-and-k_session-from-msh-transcript)):
  - `K_assoc` → ADP L1 HMAC only
  - `K_session` → L2 AEAD only (never reuse labels)
- **AEAD AAD:** `session_epoch || channel_id || channel_seq || direction`
  *Now* (wire v2): the clear packet header is the associated data and the nonce derives from the packet number; channel id and seq travel sealed ([AMP-SESSION § Sealing packets](contracts/AMP-SESSION.md#sealing-packets)).
- L4 E2E (chat body, call-media AEAD) remains; L2 hides metadata (envelope JSON, Identify addrs, relay control).

Transitional call Opus path (`CallMediaAdpPath`, A008–A011) used TCP-hello `K_assoc`; **delete when AMP call-media ships** ([A015](DECISIONS.md#a015--k_assoc-and-k_session-from-msh-transcript)).

## Channel 0 — capability plane

Immediately after Session completes, open **well-known channel 0** ([A016](DECISIONS.md#a016--channel-0--capability--identify-plane)):

- PeerId confirmation, supported L4 protocol ids, listen multiaddrs
- Relay/circuit capability flags
- Replaces libp2p Identify + partial peerstore on the product path

Dynamic protocols use `OPEN` with `protocol_id` (same `/pp/*/1.0.0` and `/pp-mesh/*/1.0.0` strings where applicable).

## Fragmentation

**L1 ADP payload cap stays 1200 B.** Multi-datagram **L3 messages** for L4 frames up to app max ([A018](DECISIONS.md#a018--fragmentation-at-l3-not-l1)) — analogous to today’s `u64` length-prefixed stream frames, but message-native.

*Now:* the limit is the datagram's, 1232 B, and each link fragments to its own frame budget (`ChannelMux::SetMaxFrameBytes`; [AMP-CHANNEL § Fragmentation (L3)](contracts/AMP-CHANNEL.md#fragmentation-l3)).

## Circuit / media-relay (big-picture)

**Channel tunnel model** ([A019](DECISIONS.md#a019--circuit-relay--channel-tunnel)):

```text
A ──Session──► R ──Session──► B
     tunnel_ch_A              tunnel_ch_B
          └──── R forwards L3 frames opaque ────┘
```

- **v1 (shipped D7a / [A022](https://github.com/people-post/pp-browser/blob/main/projects/adp/DECISIONS.md#a022--circuit-tunnel--non-blocking-coordinator-on-meshruntime)):** `CircuitTunnelCoordinator` splices opaque **L4 DATA** on A↔R and R↔B Sessions via `ChannelBridge` — message-oriented, non-blocking (not a sync `RequestBridge` port)
- **Future:** nested A↔B Session so relay sees only L2 ciphertext (full [A019](DECISIONS.md#a019--circuit-relay--channel-tunnel) blind)
- Target `protocol_id` in bridge JSON (replaces stream-bridge target protocol)

*Now:* the nested A↔B Session is built ([A024](DECISIONS.md#a024--amp-call-media-over-circuit--nested-session), [AMP-CHANNEL § Nested Session carrier](contracts/AMP-CHANNEL.md#nested-session-carrier-a024)); the circuit and media-relay coordinators are pp-cpp-mesh's.

## Addressing

```text
/ip4/<host>/udp/<port>/adp/1.0.0/p2p/<PeerId>
```

Listen: one UDP socket per host (`Endpoint` demuxes many associations). Dial: create association toward `(ip, port)`; MSH binds PeerId.

*Now:* an address names its wire version, `/adp/2/`, and may name a host (`/dns4|dns6|dns/…`); the versionless `/adp/1.0.0/` is refused ([ADP § Addresses](contracts/ADP.md#addresses), § [Lifetimes](#lifetimes-product-code-can-rely-on)).

## Product entry

**`MeshRuntime`** is the sole product-facing composer (ADR_LINK_PLANE):

- Dial / channel: `EnsureAssociation`, `OpenChannel`, `WhenChannelOpenIn`, `BindChannel`
- Snapshots: `SnapshotByDialKey`, `SnapshotByPeerId`, `IsReachable`, `IsConnected`
- Peer lookup vs dial state: a dial key names one dial slot, but a peer can have several links
  (ADP + relay carrier, A024). `ResolveConnectedLink(key)` picks the link traffic uses — the key's
  own link when Connected, else the best Connected link to the key's peer (ADP, then carrier).
  `OpenChannel`, `WhenChannelOpen` (pinned to that link at registration), `BindChannel`,
  `IsConnected` and `GetLinkSnapshot` resolve through it, so a dial in flight under the key never
  hides a live link. Dial state (`EnsureAssociation`, drops, backoff) stays on the key's own link.
- Handlers: `SetProtocolHandler`, `SetCapabilityHandler` (no `PeerLink&`)
- Node info (N032): `SetAdvertisedNodeInfo` (node version, features) rides the capability offer with amp's wire version (channel 0's leading byte 4; the retired 1 and 3 are refused); `RemoteCapability(peer_id)` reads a connected peer's
- Version negotiation (N032): a datagram of another wire version is answered (rate-limited, never larger than what it answers) with `[0xF0][16 B echo][count][versions]`; a dialer still handshaking fails at once as `VersionMismatch` (`LinkDropReason::VersionMismatch`, "version-mismatch")
- `SetRefuseUnhandledOpens(true)`: inbound opens for protocols without a handler are refused
  (OpenAck `kOpenAckNoHandler`) — the opener's `WhenChannelOpen` fails at once instead of seeing an
  open channel whose requests vanish. Off by default (raw muxes / tests bind by channel id).
  `WhenChannelOpen` also fails as soon as a connected link reports the channel Closed.
  A channel opened with `OpenChannelOnLink` is awaited with `WhenChannelOpenOnLink` (polls that
  link's mux, not whatever the key resolves to; fails at once when the link is gone).
- Nested carrier: `EstablishNestedOverCarrier`, `EnableNestedCarrierAccept`
- Drive: `Start` / `Stop`, `Drive` / `Pump` / `Tick`, `PostToIo`, `WithIoLock`. **One driver:** the
  thread that runs the drive loop calls `ClaimDriver()` as it starts; from then on a drive from any
  other thread returns at once, doing nothing (`RefusedDrives()` counts them — a second driver is a
  bug). `OnDriverThread()` is true on that thread (unclaimed: on the thread inside a drive): where io
  work and handlers run, so nothing there may wait for posted io work. `Stop()` releases the claim.
  Unclaimed runtimes are driven by any thread, one at a time (harnesses driving by hand).
  `IsDriving()` may be read from any thread.
- **When to drive.** Each `Drive()` returns a `DriveResult`: datagrams read, whether it worked
  (posted or deferred work, timers, a change another thread made on the lane) and `next_due` — how
  soon the next turn has something to do: posted work (now), the earliest timer, the earliest
  resend (`Endpoint::NextResendDueMs`), queued link work and channel-open deadlines, the link walk's
  deadline and the io ticks'. The walk (`PeerLinkManager::Tick(sweep)`) reports the earliest
  time-bound step on any link (`SweepDueMs`: a handshake timeout, keepalive, liveness window end,
  network-change probe or grace, FRAG or read timeout, nested-carrier resend). While the lane is
  active the walk and the io ticks still run every housekeeping interval (50 ms), and once after
  the activity; idle, only when due — never more often than the interval. `NextDueIn()` asks from
  any thread. A driver waits until `next_due` (capped) or a datagram (`Endpoint::WaitReadable`).
  `SetWakeHandler(fn)` ends that wait early: it is called, under the io lock, when a thread other
  than the one driving lets go of the lane having made something due before the planned wake (a
  post, a sooner timer, a reliable send's resend), or while the driver waits longer than the
  interval (any change: a dial or a mark is then looked at within the interval, as when drivers
  polled). The lane's lock (`IoLaneMutex`) reports each outermost release for this. Io ticks:
  `AddIoDueTick(fn)` runs `fn` when it asked (its result; `kIoTickIdle`: only after activity);
  `AddIoTick(fn)` keeps the fixed interval (an idle driver then wakes that often).
- Receive errors: `Endpoint::Pump` returns the datagrams it handled; a receive that fails (not
  "nothing ready") is counted (`EndpointStats::rx_errors`) and handed to
  `SetReceiveErrorObserver` — the owner checks the socket (pp-cpp-mesh reopens a dead one). Before,
  they were dropped.
- `OsUdpDatagramIo::WaitReadable` never says "ready" for ever: a socket the poll reports hung up or
  invalid (POLLHUP / POLLNVAL — a shutdown, iOS's defunct socket, an fd closed under it) leaves the
  poll and reads fail until `Reopen`; one polled readable that reads nothing twice running (an error
  pending that reading does not clear) leaves the poll for 50 ms, doubling to 1 s, and the read says
  so once; a wake pipe that hangs up is replaced; a failed poll waits out its timeout. Transient
  receive errors (a signal, an earlier send's ICMP error; Windows' `WSAECONNRESET`, also switched off
  with `SIO_UDP_CONNRESET`) are not errors.
- PeerId: `amp/link/PeerId.h` — links name peers by the mesh PeerId of their ML-DSA-65 identity key
  (pp-cpp-crypto's `PeerIdFromMlDsaPublicKey`, the one derivation every embedder uses): set
  `PeerLinkConfig::peer_id_from_identity = PeerIdFromIdentityKey`. `PeerIdFromMultiaddr` reads the
  last `/p2p/` component.
- Io executor: `IoExecutor()` is the io lane as a `pp::SerialExecutor` (pp-cpp-common), for code that
  runs on the lane without driving it — a library's control logic (pp-cpp-mesh), a guest's servers
  (pp-ledger) — and that tests may swap for a `pp::ManualExecutor`. `Post` is `PostToIo`; `PostAfter`
  / `CancelTimer` are Amp-clock timers; `RunsTasksOnCurrentThread` is `OnDriverThread()` or inside
  `RunAndWait`. `RunAndWait` runs the task on the caller under the io lock (`WithIoLock`): serialized
  with everything on the lane, with no wait for a drive turn, so a thread that drives by hand cannot
  wait on itself; the task must not destroy the runtime. The executor may outlive its runtime: posts
  and timers are then dropped and `RunAndWait` runs on the caller (the destructor waits for a call
  in progress on another thread). Like every post, work queued when the runtime stops is dropped.

### Lifetimes product code can rely on

- **A closed `ChannelSession` drops its handlers.** However it closes — `Close`, `CloseQuiet`,
  `Reset`, the peer's CLOSE / RESET, a read timeout, the link dropping, a failed write — the
  closed callback gets its notice (not for `CloseQuiet`), then both the frame handler and the
  closed callback are released. A handler may therefore capture the session (or a holder of it)
  without keeping it alive forever. The end of a channel is reported **only** to the closed
  callback, never as a frame error: state tied to a channel must be released there.
- **Stopping a stack is a goodbye.** `MeshRuntime::Stop` (and so `AmpStack::Stop`) first calls
  `PeerLinkManager::CloseAllLinks`: every link is dropped (reason `local-shutdown`) and each ADP peer
  gets a Close, so it evicts our link at once instead of after its alive timeout. Link events and
  completions those drops post are discarded with the stopped runtime's queues. A link whose peer
  already closed holds no `max_links` slot even before Tick evicts it.
- **Dial keys live until unregistered.** `RegisterEndpoint(s)` keeps a key's endpoints and
  backoff for the process's lifetime; keys made for one job (a probe, a punch target, a bridge
  target) should be dropped with `UnregisterEndpoint` when the job ends. `BurstDial` drops its own.
- **A full node answers like a TCP server.** A full link table evicts its longest-idle inbound
  link, else refuses the newcomer at once (`Err::TransportFailed`, "refused by the peer"); a full
  handshake backlog drops the hello silently and the dialer resends it on a doubling interval; a
  failed dial backs off from 1 s, doubling to 30 s, jittered ([TUNING.md](TUNING.md)).
- **A key's candidates are tried in turn.** With another candidate still to try, one that has
  not answered within `dial_attempt_timeout` gives way to the next; one that answered is the peer,
  only slow, and keeps the whole `dial_timeout` (a distant node, a post-quantum handshake). A
  nested handshake over a carrier is not a candidate: it always keeps the whole `dial_timeout`, even
  when the peer's record has untried addresses (two long legs through a relay need more than the
  short budget, pp-browser #502).
- **Multiaddrs may name a host.** Besides `/ip4|ip6/<ip>/udp/<port>/adp/<wire>/p2p/<id>`, a
  dial candidate may be `/dns4|dns6|dns/<host>/udp/<port>/adp/<wire>/p2p/<id>` (`dns4` asks for an
  IPv4 address, `dns6` IPv6, `dns` either; `/dnsaddr` is not supported). The name is resolved when
  the candidate is dialed, on a thread of its own (`HostResolver`) so the strand never blocks; dials
  under the key wait on that one lookup. A name that does not resolve fails the candidate like a
  failed dial (next candidate, then backoff). The address is kept for `host_resolution_ttl` and
  dropped when a dial to it fails, so a host that moved is looked up again. A bare
  `PeerLinkManager` has no resolver (`SetHostResolver`): there, a hostname candidate fails.

`PeerLinkManager::Links()` remains for Amp-internal tests and gradual migration. New product code should not add `FindLink` call sites.

## Threading — MeshPump

| Thread | Work |
|--------|------|
| **MeshPump (MeshHost-owned)** | `MeshRuntime::Drive` ~5ms → Endpoint drain → Session decrypt → Channel demux → L4 dispatch (fast) |
| **MeshControlPool (MeshHost-owned, 1–2)** | Blocking Connect / `IoPumpUntil` facades; may call `MeshHost::Tick` while waiting |
| **WorkerPool (shared)** | SQLite history serve, Brief HTTP, heavy decrypt/parse — **not** Amp dial waits |

L3 channel objects are **pump/control affine** (same rule as former io-thread affinity). See [THREADING.md](https://github.com/people-post/pp-browser/blob/main/docs/architecture/THREADING.md).

**Product pump:** Amp has no async reactor. `MeshHost::Start` owns a joinable `MeshPumpThread` that calls `Tick()` → `MeshRuntime::Drive()`. `AttachAmpStack` (tests) installs `MeshControlPool` only and leaves Tick to the harness — VirtualClock is not continuous-pump safe. `Drive` is mutex-serialized so MeshControl waiters’ nested `Tick` and the pump do not race PeerLink/Mux.

**Known debt:** many L4 facades still use sync `IoPumpUntil` on MeshControlPool. Prefer A022-style non-blocking callbacks (`StartBridge` + completion) when touching a path; async `Connect(cb)` for call-media is deferred ([SESSION_MACHINES](https://github.com/people-post/pp-browser/blob/main/projects/p2p-av-calls/SESSION_MACHINES.md)).

*Now:* `MeshHost` is pp-cpp-mesh's; who drives the runtime, and when: § [Product entry](#product-entry) (one driver, `DriveResult::next_due`).

## Code layout (planned)

*History:* the layout planned in pp-browser before the extraction. This repo's: [README § Layout](../README.md#layout).

```text
src/lib/amp/L1/          → L1 (exists; no libp2p)
src/lib/amp/L2/ → L2 MSH, Session, rekey
src/lib/amp/L3/ → L3 mux, frag, ChannelSession
src/lib/mesh/          → PeerLinkManager, reachability; libp2p glue shrinks
```

Acyclic: `crypto` → `adp` → `mesh` → `p2p` → `people`. `mesh` must not link libp2p ([A017](https://github.com/people-post/pp-browser/blob/main/projects/adp/DECISIONS.md#a017--libp2p-shrink-retain-crypto--peerid-only)).

## Transition

*History:* pp-browser's move onto Amp, done (its D9 and D10, [PHASES](https://github.com/people-post/pp-browser/blob/main/projects/adp/PHASES.md)).

| Phase | Milestone |
|-------|-----------|
| **A** | AMP stack parallel — `MemoryDatagramIo` + unit tests only |
| **B** | L4 protocols port one-by-one (chat → history → call → relay) |
| **C** | Delete TCP/Yamux/Noise wire path; libp2p Host gone from product |

No scattered `if (amp) … else (libp2p) …` in features — single `ChannelSession` entry per protocol ([A020](https://github.com/people-post/pp-browser/blob/main/projects/adp/DECISIONS.md#a020--single-transport-entry-per-protocol)).

## Non-goals (v1)

- Traffic padding / traffic analysis resistance
- Kademlia DHT as dial requirement (contacts-first mesh)
- QUIC compatibility
- Per-channel lite/full session profiles (session is full only)

## Ownership

See [OWNERSHIP.md](OWNERSHIP.md). Keepalive: [KEEPALIVE.md](KEEPALIVE.md). Fault matrix: [FAULT_CASES.md](FAULT_CASES.md).

## Related docs

| Doc | What |
|-----|------|
| [DESIGN.md](DESIGN.md) | L1 detail |
| [AMP-SESSION.md](contracts/AMP-SESSION.md) | L2 normative |
| [AMP-CHANNEL.md](contracts/AMP-CHANNEL.md) | L3 normative |
| [PHASES.md](https://github.com/people-post/pp-browser/blob/main/projects/adp/PHASES.md) | Implementation checklist (pp-browser's D0–D10, done) |
| [LIBP2P_STREAMS.md](https://github.com/people-post/pp-browser/blob/main/docs/architecture/LIBP2P_STREAMS.md) | **Legacy** stream stack (until Phase C) |
| [OWNERSHIP.md](OWNERSHIP.md) | What Amp owns, and what is the embedder's L4 |
| [DECISIONS.md](DECISIONS.md) | Why the stack is as it is |
| [testing/TEST_MATRIX.md](testing/TEST_MATRIX.md) | Test matrices: across layers (Tier B), and per layer from there |
| [STATUS.md](STATUS.md) | What is left to do |
