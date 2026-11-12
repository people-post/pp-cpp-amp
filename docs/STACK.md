# STACK

Amp layer map and product entry points.

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
wire v1 are refused. Design: pp-browser `projects/adp/DECISIONS.md` A029.

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

## Ownership

See [OWNERSHIP.md](OWNERSHIP.md). Keepalive: [KEEPALIVE.md](KEEPALIVE.md). Fault matrix: [FAULT_CASES.md](FAULT_CASES.md).
