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

- **Any** Connected ADP link with a warm/hot tier sends scheduled keepalives — inbound too.
- Warm/hot links are **not** exempt from eviction: with echoes, silence past the window means dead (reason `connection-dead` in `LinkEvent`). A closed Connection is evicted immediately (B25).
- `ClearWarm` sends a stop notice (cadence 0, no echo) so both ends return to the cold window.
- `PeerLinkManager::Tick` sends due keepalives **before** eviction, so a freshly warmed link announces its cadence before it is judged against the cold window.

Configure cadences via `PeerLinkConfig::keepalive_hot_interval` / `keepalive_warm_interval`; products choose tiers (pp-browser: call-path-resilience K008).

## Integration

Drive via `MeshRuntime::Tick()` / `MeshPump::Tick()`. Tests: `AdpConnTest.KeepaliveAnnouncesCadenceAndEchoes`, `AmpIntegrationTest.HotDialerKeepsColdInboundAlive`, `AmpIntegrationTest.HotLinkEvictedWhenPeerGoesSilent`.
