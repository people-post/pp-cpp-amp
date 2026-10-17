# TUNING

Transport policy a deployment may tune, set on `PeerLinkConfig` (passed to
`AmpStack::Create` / `PeerLinkManager`). Defaults are today's constants, so an
untouched config behaves exactly as before. The surface is kept small on
purpose: every knob is a support cost. Values not listed here are fixed.

## Tunables

`PeerLinkConfig::adp` (`adp::AdpTuning`, applied to every link's ADP connection,
dialed or accepted):

| Field | Default | Meaning |
|-------|---------|---------|
| `reliable_window` | 128 | Unacked Reliable packets in flight per connection |
| `replay_window` | 128 | Reliable replay window (must cover the send window) |
| `rtx_interval_ms` | 50 | Retransmit interval |
| `max_rtx` | 20 | Retransmits before a packet is given up |
| `skew_ms` | 60 000 | Accepted clock difference on packet timestamps |
| `alive_timeout_ms` | 5 000 | A cold link (no keepalive cadence) silent this long is dead |

`PeerLinkConfig::mux` (`MuxTuning`, applied to every link's channel mux):

| Field | Default | Meaning |
|-------|---------|---------|
| `max_concurrent_channels` | 256 | Open channels per link; further OPENs are refused |
| `max_queued_bytes` | 32 MiB | Reliable bytes waiting for window space; sends beyond fail |
| `frag_assembly_timeout_ms` | 30 000 | A fragmented message not completed in time is dropped |

The existing `PeerLinkConfig` fields (dial timeouts, keepalive intervals, link
caps, `network_change_grace`) are tunable as before.
Links behave like TCP connections (pp-ledger load-resilience LR005):

| Field | Default | Meaning |
|-------|---------|---------|
| `max_links` | 48 | Links held at once (TCP: connections). Full: the longest-idle inbound link is closed to make room (connected, not warm, no channel open but the capability channel, nothing received for 1 s; its peer is told); failing that, the newcomer is refused at once — a `Close` its dialer takes as a refusal, like a reset |
| `max_pending_inbound` | 16 | Inbound handshakes in progress (TCP: the listen backlog). Full: the hello is dropped silently; the dialer resends it |
| `dial_failure_backoff_initial`, `dial_failure_backoff` | 1 s, 30 s | A key's failed dial backs off from the first, doubling with each failure in a row up to the second, ±20 % jitter; a success or a fresh address starts it over |

A dialer resends its handshake before it authenticates up to 6 times, the interval doubling from
`rtx_interval_ms` (50 ms … 3.2 s, ~6.3 s in all, like SYN retries), and takes the peer's
pre-authentication `Close` as a refusal. An accepted association still caps its pre-auth resends at
two and ignores a pre-auth `Close` (reflection, forgery): only the dialer, which chose its peer, trusts
it — and only someone who sees the association id on the path could forge it.

**Socket buffers** (`UdpSocketOptions`, passed to `OsUdpDatagramIo::Bind`; 0 = the OS default).
A node's one UDP socket carries every peer: when its receive buffer overflows, the kernel drops
datagrams of all peers alike — handshakes, keepalives, other peers' requests — which a few peers
sending large messages at once can cause (pp-ledger load-resilience M12: over a million datagrams
dropped in 8 s on Linux's default 208 KiB). A product sizing its socket asks for more; Linux grants
at most `net.core.rmem_max` / `wmem_max` unless the process may force it (CAP_NET_ADMIN), so hosts
raise those (`sysctl -w net.core.rmem_max=16777216 net.core.wmem_max=16777216`). A container
cannot: those sysctls exist only in the host's network namespace. There, give the binary
`CAP_NET_ADMIN` as permitted only (`setcap cap_net_admin+p`, and the container the capability,
e.g. compose `cap_add: [NET_ADMIN]`): `Bind` makes it effective for the buffer call alone and puts
the effective set back. (`+ep` keeps it effective throughout, and the binary will not start where
the capability is not granted.) Not with rootless containers, nor under `no-new-privileges`. `Stats()` reports
what was granted and, on Linux, the kernel's count of datagrams dropped for want of buffer
(`SO_RXQ_OVFL`; it rides on each received datagram, so it moves as datagrams arrive).

`host_resolution_ttl` (default 60 s) is how long a hostname multiaddr's resolved
address is reused before the name is looked up again (a failed dial drops it at
once).

## Rules (`ValidatePeerLinkConfig`)

`AmpStack::Create` refuses a config that breaks these; call the validator
yourself when constructing `PeerLinkManager` directly.

- `replay_window >= reliable_window` — otherwise a packet the peer may have in
  flight is "too far" after a loss and delivery stalls (fixed in v2.13.2).
- `reliable_window`, `rtx_interval_ms`, `max_rtx`, `skew_ms`,
  `max_concurrent_channels` > 0.
- `alive_timeout_ms >= 2 × rtx_interval_ms` — a live link survives a couple of
  retransmit rounds of silence.
- `frag_assembly_timeout_ms >= rtx_interval_ms × max_rtx` — a fragment may need
  the whole retransmit budget.
- `max_queued_bytes` holds at least one control message (64 KiB).

## Between peers

Tunables are local; peers need not match. A peer sending a larger window than
our replay window costs resends, not a stall (since v2.13.2 a packet we cannot
keep is not ACKed). Keep `skew_ms` and `alive_timeout_ms` comparable across a
deployment, or one side may judge the other dead or out of time first.

## Not tunable

Wire-format facts peers must agree on: key / nonce / MAC sizes, ADP header and
`kMaxPayload`, the FRAG chunk size, `kMshVersion`. Per-class message caps
(`AmpChannelLimits`) are part of each channel policy, not deployment tuning;
so is `ChannelPolicy::read_timeout` (enforced since v2.15.0: a channel with
nothing inbound for that long is reset), which each protocol sets for its own
request/response timing.
