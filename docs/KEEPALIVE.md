# ADP keepalive (NAT / association maintenance)

**Status:** v2 (2026-09-24) — cadence-aware liveness + echo + dead-peer eviction

## Purpose

UDP associations behind NAT lose router mappings after idle periods (cellular CGNAT can be 20–30 s, home routers 30–120 s). Links that must survive idle (relay reservations, standby paths, warm chat peers) keep a **keepalive cadence**; everything else is **cold** and is evicted quickly when silent.

## Liveness window

Each `adp::Connection` judges liveness against

```
window = max(kAliveTimeoutMs (5 s), 5/2 × max(local cadence, peer cadence))
```

- *local cadence* — the interval we announced in our last scheduled keepalive (0 after `StopKeepalive`);
- *peer cadence* — the interval the peer announced in its last keepalive (0 = none / stopped).

`LooksAlive(now)` = authenticated RX within `window`. The 5/2 factor tolerates ~2 lost keepalives.

v1 used a fixed 5 s window on the receiver whatever the sender's tier, so a warm/hot sender (60 s / 20 s) was evicted by its cold peer after 5 s (dogfood 2026-09-24: relay reservations dropped).

## Wire

| Field | Value |
|-------|-------|
| `PacketType` | `Keepalive` (4) |
| seq | 0 |
| Payload | `u32` BE sender cadence ms (0 = stopped) · `u8` flags |
| Flags | `0x01` echo request |

On receipt: record the peer cadence; if echo is requested, reply at once with a keepalive carrying our own cadence and **no** echo request (no ping-pong loops). The echo gives the sender RX (so silence means a dead peer / path) and refreshes NAT mappings in both directions.

## Link policy

| Tier | API | Cadence (Amp default) | Eviction |
|------|-----|-----------------------|----------|
| Cold | (default) / `ClearWarm` | none | Silent > 5 s (or peer cadence window) |
| Warm | `PeerLinkManager::MarkWarm` | `keepalive_warm_interval` (60 s) | Silent > window (dead-peer) |
| Hot | `PeerLinkManager::MarkHot` | `keepalive_hot_interval` (20 s) | Silent > window (dead-peer) |

A link keeps the **highest** of three sources (the first two since v3.5.0):

- **The product mark** — `MarkWarm` / `MarkHot` / `ClearWarm`, one slot per link (last write wins).
- **Its open channels** — `ChannelPolicy::keepalive`: while a channel asking for a tier is open (or opening) on the link, at either end that bound it, the link keeps at least that tier; it goes when the channel closes. A link kept up because of what rides it says so by the channel (a call's paths: pp-cpp-mesh `CallMediaControlChannelPolicy`, on every path of a call — call-path-resilience K008), so links shared by several features (chat warms a peer, a call rides the same link) never undo each other: a call ending leaves chat's warm mark, and `ClearWarm` leaves an open channel's tier.

- **The links it carries** — a nested (carrier-backed) link sends no keepalives of its own, so its tier (its mark and
  its channels) goes to the link whose mux carries it, recomputed every sweep (`PeerLink::CarriedKeepaliveTier`). A
  peer reached through a relay that chat warms keeps the relay link warm, a call through it keeps it hot. Before,
  the relay link fell to the cold window as soon as the call's media stopped (or the park on that relay lapsed),
  died 5 s later, and took the carrier with it, so the next call to the same peer started cold (pp-browser #577).

- **Any** Connected ADP link with a warm/hot tier sends scheduled keepalives — inbound too.
- Warm/hot links are **not** exempt from eviction: with echoes, silence past the window means dead (reason `connection-dead` in `LinkEvent`). A closed Connection is evicted immediately (B25).
- `ClearWarm` — and the last channel asking for a tier closing — sends a stop notice (cadence 0, no echo) so both ends return to the cold window, unless the other source still keeps the link up.
- `PeerLinkManager::Tick` sends due keepalives **before** eviction, so a freshly warmed link announces its cadence before it is judged against the cold window.

Configure cadences via `PeerLinkConfig::keepalive_hot_interval` / `keepalive_warm_interval`; products choose tiers (pp-browser: call-path-resilience K008).

## Network change

A link's liveness window can be long (5/2 × a 20 s hot cadence = 50 s), and a local network change — Wi-Fi ↔ cellular, new address, sleep / wake — can kill every path at once or leave them reachable only from the new address. The product reports it with `MeshRuntime::NotifyNetworkChanged()` (any thread; runs `PeerLinkManager::OnNetworkChanged()` on the IO lane):

- Every Connected ADP link is marked **suspect** and sent a **probe** at once: a keepalive at the link's current cadence (unchanged) with the echo flag. Sent from our new address, it also moves the peer's path to that address (the peer learns paths from fresh authenticated packets).
- An authenticated packet received after the change clears the suspicion. Otherwise the probe is resent every `network_change_grace / 4`, and after `PeerLinkConfig::network_change_grace` (2 s) the link is evicted with reason **`network-changed`** — instead of lingering for its liveness window.
- Dial backoffs are cleared: a peer that failed from the old network may be reachable from the new one.
- Nested (carrier-backed) links are not probed; they live and die with their carrier.

Tests: `MeshLinkTest.NetworkChangeEvictsASilentLinkWithinTheGrace`, `NetworkChangeKeepsALinkThatAnswers`, `NetworkChangeResendsALostProbe`.

## Verifying one link

A product that is about to rely on one link — a relay it will bridge through, a seed it parks on —
can ask `PeerLinkManager::VerifyLink(peer, heard_within, done)` instead of trusting a link that
may have been silent for most of its liveness window. Heard from within `heard_within`: `done(true)`
at once. Otherwise the link is probed exactly as after a network change; an authenticated packet
answers `done(true)`, silence for `network_change_grace` evicts it with reason **`unresponsive`**
and answers `done(false)` (as does any other drop meanwhile, or no Connected link to begin with).
Carrier-backed links answer `true` — their carrier lane judges them.

Tests: `MeshLinkTest.VerifyLinkEvictsASilentLinkAndSaysSo`, `VerifyLinkKeepsALinkThatAnswers`.

## Integration

Drive via `MeshRuntime::Tick()` / `MeshPump::Tick()`; an idle driver wakes for the next keepalive or window end, which the walk reports (`PeerLinkManager::SweepDueMs`, STACK.md § When to drive). Tests: `AdpConnTest.KeepaliveAnnouncesCadenceAndEchoes`, `AmpIntegrationTest.HotDialerKeepsColdInboundAlive`, `AmpIntegrationTest.HotLinkEvictedWhenPeerGoesSilent`.
