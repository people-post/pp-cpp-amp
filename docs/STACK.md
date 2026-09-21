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
- Nested carrier: `EstablishNestedOverCarrier`, `EnableNestedCarrierAccept`
- Drive: `Start` / `Stop`, `Drive` / `Pump` / `Tick`, `PostToIo`, `WithIoLock`

`PeerLinkManager::Links()` remains for Amp-internal tests and gradual migration. New product code should not add `FindLink` call sites.

## Ownership

See [OWNERSHIP.md](OWNERSHIP.md). Keepalive: [KEEPALIVE.md](KEEPALIVE.md). Fault matrix: [FAULT_CASES.md](FAULT_CASES.md).
