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
(`AmpChannelLimits`) are part of each channel policy, not deployment tuning.
