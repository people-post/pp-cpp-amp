# STACK

Amp layer map and product entry points.

## Layers

| Layer | Headers | Role |
|-------|---------|------|
| L1 ADP | `amp/L1/` | UDP association, Reliable/BestEffort, HMAC binder |
| L2 MSH | `amp/L2/` | Session handshake, AEAD, rekey, session control |
| L3 mux | `amp/L3/` | Channel OPEN/CLOSE, FRAG, Bulk credit, capability codec |
| Link | `amp/link/` | Dial book, PeerLink, MeshRuntime / MeshPump |

Dependencies: `pp-cpp-common` + `pp-cpp-crypto` only. No product L4.

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
- `SetRefuseUnhandledOpens(true)`: inbound opens for protocols without a handler are refused
  (OpenAck `kOpenAckNoHandler`) — the opener's `WhenChannelOpen` fails at once instead of seeing an
  open channel whose requests vanish. Off by default (raw muxes / tests bind by channel id).
  `WhenChannelOpen` also fails as soon as a connected link reports the channel Closed.
  A channel opened with `OpenChannelOnLink` is awaited with `WhenChannelOpenOnLink` (polls that
  link's mux, not whatever the key resolves to; fails at once when the link is gone).
- Nested carrier: `EstablishNestedOverCarrier`, `EnableNestedCarrierAccept`
- Drive: `Start` / `Stop`, `Drive` / `Pump` / `Tick`, `PostToIo`, `WithIoLock`

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
- **Multiaddrs may name a host.** Besides `/ip4|ip6/<ip>/udp/<port>/adp/1.0.0/p2p/<id>`, a
  dial candidate may be `/dns4|dns6|dns/<host>/udp/<port>/adp/1.0.0/p2p/<id>` (`dns4` asks for an
  IPv4 address, `dns6` IPv6, `dns` either; `/dnsaddr` is not supported). The name is resolved when
  the candidate is dialed, on a thread of its own (`HostResolver`) so the strand never blocks; dials
  under the key wait on that one lookup. A name that does not resolve fails the candidate like a
  failed dial (next candidate, then backoff). The address is kept for `host_resolution_ttl` and
  dropped when a dial to it fails, so a host that moved is looked up again. A bare
  `PeerLinkManager` has no resolver (`SetHostResolver`): there, a hostname candidate fails.

`PeerLinkManager::Links()` remains for Amp-internal tests and gradual migration. New product code should not add `FindLink` call sites.

## Ownership

See [OWNERSHIP.md](OWNERSHIP.md). Keepalive: [KEEPALIVE.md](KEEPALIVE.md). Fault matrix: [FAULT_CASES.md](FAULT_CASES.md).
