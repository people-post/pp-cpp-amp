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
10. **Link hygiene** (pp-browser call-path-resilience k1): no link lingers.
   - A carrier-backed link whose carrier closed is dropped (`CarrierClosed`) in any phase but
     Handshaking / Dialing (those keep their deferred drop); an inbound link whose handshake
     fails is dropped (`HandshakeFailed`).
   - Drops target **a link, by `LinkHandle`**, never "whatever holds the dial key": completions
     (dial / nested / inbound), aborts, the dual-dial loser and Tick's timeouts / evictions all
     know the link. An ADP link and a nested link may share a dial key (A024 — circuit reach
     nests under the target PeerId while the direct dial is aborted); the key index names the
     latest, the other stays live by id, and dropping it leaves the key's index alone. Only
     product requests (`RequestDropLink`) resolve a key.
   - Waiters belong to a link, too: `EnsureAssociation` joins (or aborts) only the ADP dial under
     its key, `EstablishNestedOverCarrier` only a nested handshake under its key (waiters per
     `LinkId`). Neither waits on the other — a cold ADP dial to a NAT'd peer runs its full dial
     timeout while the carrier path answers at once.
   - Only a **fresh** packet refreshes liveness or moves the ADP path (A003): data once its replay
     window accepts the seq; seq-0 control packets (ack / close / keepalive) only when their wire
     timestamp is the newest seen (serial arithmetic). A replayed packet from a new address
     cannot redirect an association.
   - A send the OS rejects as host / network unreachable or down (`kDatagramSendUnreachable`)
     marks the association `PeerUnreachable`; Tick drops the link at once (`TransportFailed`).
   - Snapshots and events carry `LinkPathKind` (Direct / Punched / Carrier — Punched when the
     link came up through `BurstDial`, either side); snapshots also carry the live `remote`
     endpoint and `last_rx_age_ms`.
11. **Reliable lane on nested links** (A024 follow-on, pp-browser call-path-resilience k1). A relay
   splices the circuit carrier as best-effort bytes, so ADP's per-hop reliability ends at the relay
   and a nested link's Reliable-class mux frames (call control, chat, the call-media hello) had no
   retransmission: one lost or reordered frame wedged the channel (`out of order seq`) for good.
   - `CarrierLane` numbers each Reliable-class frame end to end (`LaneData` = [seq u32][Sealed
     wire]) and keeps it until acked; the receiver releases frames in order, drops duplicates and
     answers every data frame with `LaneAck` (cumulative u32 + 64 selective bits). Resend on an
     adaptive RTO (RFC 6298 estimator, Karn, 200 ms – 3 s, doubling), or at once after three
     selective acks pass a frame. Window 256 frames (mux transport credits; when full, the mux queues
     Reliable frames until acks free room, as for ADP).
   - A frame unacked after 10 transmissions (~20 s) means the end-to-end path is dead while the
     carrier may look fine (relay stopped splicing, far leg gone): Tick drops the nested link
     (`ConnectionDead`).
   - BestEffort frames (media) are unchanged: plain `Sealed`, no lane.
   - Negotiation without a wire version: each end sends a lane probe (a `LaneAck` acking nothing)
     before every handshake message and once at Connected; a peer that has sent any lane frame
     speaks the lane. Older builds drop unknown carrier kinds (handshake and connected paths
     alike), never probe, and keep getting plain `Sealed` frames. Lane frames received while
     still handshaking are acked and delivered once the mux exists.
   - Relays need no change (they never parse the carrier).

## Consequences

- Breaking Amp API for handlers / FindLink → bump major (`v2.x`).
- Browser L4 migrates off `PeerLinkManager&` and `FindLink`.
- Dogfood call-media circuit hop remains the regression oracle.
