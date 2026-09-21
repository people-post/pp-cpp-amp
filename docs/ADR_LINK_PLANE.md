# ADR: Amp link plane (LinkId + strand-only API)

**Status:** Accepted  
**Date:** 2026-09-21  
**Amends:** A024 (carrier coexistence), A026 (dual-dial), A027 (parent-only destroy)

## Context

`PeerLinkManager` mixed dial book, association, dual-dial election, nested carriers, ch0,
keepalive, and protocol fanout behind an overloaded `peer_key` string map and raw
`PeerLink*` exports. Shared `MeshRuntime::io_mu_` closed map races but not pointer
lifetime, sync-callback reentrancy, or PeerId vs dial-alias confusion.

## Decision

1. **`LinkId` is the primary key** of the live link table. `DialKey` (product association
   key) and `PeerId` are secondary indexes.
2. **`PeerPresence`** per PeerId holds at most one Connected ADP `LinkId` (A026) and at
   most one Connected carrier `LinkId` (A024). Carrier never satisfies ADP
   `EnsureAssociation`.
3. **No product `PeerLink*` / `&`.** Product uses `LinkHandle`, snapshots,
   `WhenChannelOpen`, `BindChannel`, and protocol handlers that receive
   `(LinkHandle, remote_peer_id, channel_id)`.
4. **`MeshRuntime` is the sole product entry** for link ops. Completions run via
   `PostToIo` (never under association mutation). Nested product `Drive` is forbidden
   when MeshPump runs.
5. **Index bind** (dial alias ↔ LinkId) replaces map-key rename (`RekeyLink` surgery).
   Session crypto rekey on ch0 remains separate.
6. **Internals split:** DialBook, LinkTable, AssociationController, DualDialElector,
   CarrierInstaller, CapabilityPlane, KeepalivePolicy behind a thin façade.
7. **Strand mutex is non-recursive** once completions are deferred; affinity is
   “called only from MeshRuntime Drive/PostToIo.”

## Consequences

- Breaking Amp API for handlers / FindLink → bump major (`v2.x`).
- Browser L4 migrates off `PeerLinkManager&` and `FindLink`.
- Dogfood call-media circuit hop remains the regression oracle.
