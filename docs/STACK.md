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
- Handlers: `SetProtocolHandler`, `SetCapabilityHandler` (no `PeerLink&`)
- `SetRefuseUnhandledOpens(true)`: inbound opens for protocols without a handler are refused
  (OpenAck `kOpenAckNoHandler`) — the opener's `WhenChannelOpen` fails at once instead of seeing an
  open channel whose requests vanish. Off by default (raw muxes / tests bind by channel id).
  `WhenChannelOpen` also fails as soon as a connected link reports the channel Closed.
  A channel opened with `OpenChannelOnLink` is awaited with `WhenChannelOpenOnLink` (polls that
  link's mux, not whatever the key resolves to; fails at once when the link is gone).
- Nested carrier: `EstablishNestedOverCarrier`, `EnableNestedCarrierAccept`
- Drive: `Start` / `Stop`, `Drive` / `Pump` / `Tick`, `PostToIo`, `WithIoLock`

`PeerLinkManager::Links()` remains for Amp-internal tests and gradual migration. New product code should not add `FindLink` call sites.

## Ownership

See [OWNERSHIP.md](OWNERSHIP.md). Keepalive: [KEEPALIVE.md](KEEPALIVE.md). Fault matrix: [FAULT_CASES.md](FAULT_CASES.md).
