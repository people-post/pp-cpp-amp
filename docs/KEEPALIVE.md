# ADP keepalive (NAT / association maintenance)

**Status:** v1 (2026-09-01)

## Purpose

UDP associations behind NAT lose router mappings after idle periods (often 30–120 s). Application-level liveness (`LooksAlive`, 5 s) evicts cold links quickly. **Keepalive** lets warm/hot links refresh NAT mappings without application traffic.

## Wire

| Field | Value |
|-------|-------|
| `PacketType` | `Keepalive` (4) |
| Payload | empty |
| seq | 0 |

Receiving a Keepalive updates `last_auth_rx_ms_` like any authenticated packet. No `on_message_` callback.

## Link policy

| Tier | API | Outbound interval (default) | Eviction |
|------|-----|----------------------------|----------|
| Cold | (default) | none | Evicted when `LooksAlive` false (~5 s idle) |
| Warm | `PeerLinkManager::MarkWarm` | 60 s | Skips `LooksAlive` eviction |
| Hot | `PeerLinkManager::MarkHot` | 20 s | Skips `LooksAlive` eviction |

**Sender rule:** only **outbound** connected links send scheduled keepalives. Public / inbound peers respond to normal traffic; they do not initiate keepalive timers.

Configure intervals via `PeerLinkConfig::keepalive_hot_interval` / `keepalive_warm_interval`.

## Integration

`PeerLinkManager::Tick()` calls `MaybeSendKeepalives()` after idle eviction. Drive via `MeshRuntime::Tick()` / `MeshPump::Tick()`.

Product layers (pp-browser) choose when to `MarkWarm` / `MarkHot` / `ClearWarm` on active peers.
