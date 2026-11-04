# ADR: Stream transport (TCP, TLS, WebSocket, Bluetooth) for networks without UDP

**Status:** Proposed — design only; implementation not scheduled  
**Date:** 2026-10-02; revised 2026-11-03 for wire v2 (pp-cpp-amp v4.0.0) and Bluetooth  
**Builds on:** [ADR_LINK_PLANE.md](ADR_LINK_PLANE.md) (LinkId, `PeerPresence`, `MeshRuntime`), A024 (carrier links), A029 H1 (wire v2: packets sealed where they are numbered)

## Context

Amp links run on ADP over UDP. Some networks block UDP outright (many corporate,
hotel, campus and some mobile networks); some allow only HTTPS on 443, and some
only through an HTTP proxy or with TLS inspection. A peer on such a network cannot
reach the mesh at all today.

**Goal: getting through those networks.** Not security — MSH already authenticates
peers and encrypts with ML-KEM / ML-DSA and AEAD — and not hiding from a censor
that fingerprints traffic patterns.

A second case without UDP: two devices near each other with no network at all, over **Bluetooth**.

The layering separates what is UDP-specific (as of wire v2):

| Layer | UDP-specific? |
|-------|---------------|
| L1 ADP (`DatagramIo`, `Endpoint`, `Connection`) | Yes, by design: datagrams with its own reliability (ack, retransmit, replay window, liveness), an 8 B association id, no length field (UDP has it), and at most 1 232 B a datagram (`kMaxDatagramBytes`, a compile-time constant) |
| L2 MSH session | No. It holds the keys and seals or opens a packet given its space and number (`IPacketProtection`); whoever numbers packets calls it |
| L3 channel mux | No. It sends frames up to the link's budget (`ChannelMux::SetMaxFrameBytes`) and fragments past it |
| `PeerLink` | Two transports already: `TransportClass::Adp` and `::Carrier` (A024: MSH, then inner packets it numbers and seals itself, no ADP) |
| `PeerLinkManager` | One ADP `Endpoint` for dial / accept / clock / liveness |
| Multiaddr | Only `/ip4\|ip6\|dns4\|dns6\|dns/…/udp/<port>/adp/1.0.0/p2p/<id>` (names resolved at dial time). An address that does not parse is skipped (`DialBook`, `MeshRuntime`), so new forms are additive |
| `MeshRuntime` (product API) | No — dial keys and multiaddr strings |

## Decision (proposed)

### 1. A third link transport: `TransportClass::Stream`

MSH runs directly over a framed byte stream — no ADP underneath. The stream is
reliable and ordered, so neither ADP's reliability nor the carrier's reliable lane
(`CarrierLane`) is used. The carrier path is the template: it already runs the MSH
handshake as whole MSH frames (`MshAdpHandshake` without ADP chunking) and, since
wire v2, numbers and seals its own packets (`PeerLink::SealCarrierFrame`,
`DeliverSealedCarrierWire`).

- **Packets (wire v2):** the stream link does what the carrier link does.
  - It numbers its packets itself, one counter per direction, and seals each through
    the session's `IPacketProtection` with the clear header as associated data. The
    nonce comes from the space and the number, never sent.
  - It gets its own `PacketSpace` value (the enum has room). The space is part of the
    nonce, and a session lives on one link, so no two transports number under one key.
  - Its clear header is a type byte and the 4 B number: no association id (the socket
    says whose packet it is), no HMAC (the tag authenticates).
  - At the last number the link fails and redials, as an ADP association does.
  - A stream delivers in order once, so a number must be exactly one above the last.
    No replay window: anything else is an attack or a bug, and drops the link.
- **Framing:** each MSH message / sealed packet is one length-prefixed record on the
  stream (a varint, or 2 B with records capped at 64 KiB). ADP dropped its length
  field because UDP gives one; a stream does not.
- **Frame budget:** the link sets its mux's budget (`SetMaxFrameBytes`). With no path
  MTU to respect, a TCP link can take large frames (16 KiB, say), so a video frame
  goes in a few records instead of ~1 178 B fragments.
- **Backpressure:** a byte cap on the socket's pending send queue feeds the mux's
  `TransportCredits`, as the ADP window does today.
- **Liveness:** no ADP liveness on a stream. A socket error or close drops the link
  at once; a peer that goes silent is caught by the link keepalive tiers
  ([KEEPALIVE.md](KEEPALIVE.md)) sent as control frames on the stream.
- **I/O:** non-blocking sockets driven by `MeshPump` (Exclusive Drive, ADR_LINK_PLANE
  §4): listen, accept, connect with a deadline on the Amp clock.

L2, L3, `ChannelMux`, channel policies and every product L4 are unchanged.

### 1a. Bluetooth is a stream link too

Bluetooth's links are reliable and ordered at the link layer, so they take the stream
link's design (§1), not ADP's retransmits:

| Bluetooth | Shape | Framing |
|-----------|-------|---------|
| Classic RFCOMM (Android `BluetoothSocket`; on iOS only for MFi accessories) | Byte stream | Length-prefixed records, as TCP |
| BLE L2CAP connection-oriented channel (Android 10+ `BluetoothSocket`, iOS `CBL2CAPChannel`'s streams) | Byte stream on both | Length-prefixed records, as TCP |
| BLE GATT, write without response + notify (the fallback where L2CAP channels are missing) | Messages of ATT MTU − 3 (negotiated, typically 185–512 B), in order while the sender keeps to the platform's flow control (iOS `canSendWriteWithoutResponse`, Android's write callbacks) | One record per message; the budget is the MTU less the packet's wrapping |

- **Small budgets.** A BLE link's frame budget is a few hundred bytes. The mux refuses a
  budget below `ChannelMux::kMinFrameBytes` (64 B) and a message needing more than
  65 535 fragments (`kMaxFragmentsPerMessage`), so a small link can neither underflow a
  fragment's size nor wrap its 16-bit count.
- **What fits.** About 30 B of each BLE frame is wrapping (5 B header, 16 B tag, 9 B
  frame header): 12 % of a 244 B frame. A 40 ms Opus packet in a media frame fits one.
  Video mostly does not fit Bluetooth's rate; product policy decides (§ Limits).
- **Discovery and addresses** are product work: a Bluetooth peer has no IP. Dial keys
  are PeerIds already; the address names the radio's peer (`/ble/<id>/amp/…`, open).

### 2. Layers on the stream, in order of the networks they pass

| Stack | Passes | Notes |
|-------|--------|-------|
| TCP | Networks that block UDP but not TCP | Simplest; rarely the only thing open |
| TCP + TLS (443) | Networks allowing only HTTPS | The main target |
| TCP + TLS + WebSocket (443) | HTTP-proxy-only networks; TLS-inspecting proxies that require real HTTP | WebSocket upgrade, then binary messages carry the records |

TLS is a wrapper for middleboxes only:

- **Look like ordinary HTTPS:** nodes have DNS names and real certificates (e.g.
  ACME / Let's Encrypt); clients dial by name with SNI; ALPN `h2` / `http/1.1`.
- **Never trusted for identity.** The client accepts any certificate — MSH
  authenticates the peer inside. A TLS-inspecting proxy that substitutes its own
  certificate therefore does not weaken anything.
- TLS library: BoringSSL (pp-browser already vendors it) or the platform stack,
  behind a small interface so Amp keeps `pp-cpp-common` + `pp-cpp-crypto` as its
  only required dependencies; the stream transport builds without TLS.

### 3. Multiaddr

The UDP form is unchanged. Proposed stream forms (libp2p-style protocol tokens):

```text
/ip4/<ip>/tcp/<port>/amp/1.0.0/p2p/<id>                     plain TCP
/dns4/<name>/tcp/443/tls/amp/1.0.0/p2p/<id>                 TLS, SNI = <name>
/dns4/<name>/tcp/443/tls/ws/amp/1.0.0/p2p/<id>              TLS + WebSocket
```

(`/ip6`, `/dns6`, `/dns` likewise.) `ParseAdpMultiaddr` grows into a parser that
returns the transport kind; dialing picks the transport from the multiaddr.

### 4. Presence and preference

`PeerPresence` (ADR_LINK_PLANE §2) gains a Stream slot: at most one Connected
Stream link per PeerId, beside ADP and carrier.

- A Stream link is a **direct** path: it satisfies `EnsureAssociation` (a carrier
  link still does not).
- Traffic prefers **ADP, then Stream, then carrier** (`ResolveConnectedLink`). When
  an ADP link comes up later, traffic moves to it; the Stream link idles and
  closes on the cold keepalive tier.
- Dual-dial election (A026) applies per transport class.

### 5. Fallback policy (dialer side)

Nodes listen on both UDP and TCP 443; only the dialer decides.

1. Dial ADP (UDP) first.
2. If no link is Connected within a short head start (proposed ~300 ms, tunable),
   also dial the peer's stream multiaddrs; keep whichever connects first.
3. Remember per local network (e.g. per interface / gateway) that UDP failed, and
   dial stream directly next time; retry UDP occasionally.

## Limits

- **Realtime media over a stream** suffers head-of-line blocking: one lost segment
  stalls everything behind it until TCP resends. Calls work, but stutter on lossy
  networks. Product policy may prefer ADP or a relay for calls when one exists.
- **No hole punching over TCP.** Punch, dial-back and UPnP stay UDP-only. A stream
  client reaches public nodes directly; two stream-only peers meet through a relay
  (circuit relay, unchanged).
- **Not hiding.** Traffic analysis can still identify Amp over TLS.

## Alternatives considered

| Alternative | Why not |
|-------------|---------|
| TCP as a `DatagramIo` under unchanged ADP | ADP's retransmits (50 ms) and 5 s liveness run on top of TCP's own: duplicate sends exactly when a path is congested, liveness fighting TCP backoff; one `Endpoint` per `PeerLinkManager` would force UDP and TCP into one IO with transport-tagged endpoints |
| Bluetooth GATT as a `DatagramIo` under ADP | The same double reliability (BLE's link layer acknowledges), and ADP's 1 232 B datagram is a constant, larger than any GATT message: ADP would need a per-IO size first |
| DTLS over UDP | Networks that block UDP block it regardless of content; networks allowing only UDP 443 expect QUIC, which DTLS is not; double encryption for no security gain |
| QUIC disguise | Real QUIC framing is a second transport stack — large, and TLS-over-TCP covers the same networks |

## Consequences

**Amp (estimate):** stream transport (TCP, I/O, framing, `PeerLink` stream ctor,
`PeerLinkManager` dial / accept / presence / drop reasons, link events with
`path=stream`, multiaddr) ~2–3 weeks with tests; TLS ~1 week; WebSocket ~1 week.

**Product (pp-browser, ~1 week):** `MeshHost` opens a TCP listener; nodes advertise
stream multiaddrs (listen-address ranking, address disclosure); fallback policy
wiring; metrics labels. L4 protocols and UI unchanged.

**Ops:** nodes open 443/tcp, get DNS names and certificates (Route 53 + ACME).

## Open questions

1. Where a Bluetooth link ranks against ADP and TCP stream links (§4), and whether
   calls may use one.
2. Whether mesh's own address records (directory, DHT, address disclosure) skip forms
   they cannot parse as amp does, or refuse the whole record — decides whether
   advertising stream / Bluetooth addresses needs a coordinated release.
3. Multiaddr tokens: `amp/1.0.0` for streams vs reusing `adp/1.0.0`; `/wss` vs `/tls/ws`.
4. Fallback head start and how "this network blocks UDP" is remembered.
5. Calls over a stream: allow, or prefer a relay over UDP when one is reachable.
6. TLS dependency: BoringSSL in Amp behind an interface, vs injected by the product.
7. Whether plain TCP (no TLS) is worth shipping on its own, or only as the base of TLS.
