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
   `PostToIo` (never under association mutation). **Exclusive Drive:** only one
   driver may call `Pump`/`Tick`/`Drive` (MeshPump thread or the test harness acting
   as Amp). Nested `Drive` is refused. Teardown-class work uses `PostDeferred`
   (Abort / Close / DropLink / `on_done`); deadlines use `PostAfter` on the Amp clock.
   Waiters and L4 must not call `Tick`/`Drive` to make progress.
   **`BurstDial`** is the product API for parallel ephemeral ADP dials (punch sync
   windows): Amp-clock `PostAfter` deadline, `PostToIo` win poll, `PostDeferred`
   Abort + `on_done`. Ephemeral DialKeys `amp:burst:N:…`; win = Connected ADP PeerId.
5. **Index bind** (dial alias ↔ LinkId) replaces map-key rename (`RekeyLink` surgery).
   `LinkTable` owns `unique_ptr<PeerLink>` by `LinkId`; `BindDialKey` rewrites the
   dial index only. Session crypto rekey on ch0 remains separate.
6. **Capability / protocol observers** take `LinkHandle` (+ ids / payload), not
   `PeerLink&`.
7. **Internals split:** DialBook, LinkTable, DualDialElector are extracted; association /
   carrier / capability / keepalive remain on the PeerLinkManager façade until a second
   consumer needs them (no empty placeholder types).
8. **Strand mutex is non-recursive** once completions are deferred; affinity is
   “called only from MeshRuntime Drive/PostToIo/PostDeferred.”
9. **Link events** (`amp/link/LinkEvents.h`, `MeshRuntime::AddLinkEventListener`):
   `Connected` (after dual-dial adoption), `Dropped` (every `DropLink`, with a
   `LinkDropReason`, `was_connected`, last-RX age) and `PathChanged` (ADP remote
   endpoint migration, from `Connection::OnPathChange`). Posted via the completion
   poster — never on the link's own callback stack — and carry `LinkHandle` + ids,
   not `PeerLink&`. Amp stays logging-free; products log / react to events instead of
   polling `FindLink`. Every drop site must pass a reason (`ScheduleDropLink(key, reason)`).
   Products drop a link they know is stale with `RequestDropLink(dial key or PeerId)` (reason
   `requested`, scheduled on Tick) — never via `FindLink` + `Connection::Close`.

## Consequences

- Breaking Amp API for handlers / FindLink → bump major (`v2.x`).
- Browser L4 migrates off `PeerLinkManager&` and `FindLink`.
- Dogfood call-media circuit hop remains the regression oracle.
