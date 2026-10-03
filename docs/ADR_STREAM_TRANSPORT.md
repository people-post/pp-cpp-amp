# ADR: Stream transport (TCP, TLS, WebSocket) for restrictive networks

**Status:** Proposed — design only; implementation not scheduled  
**Date:** 2026-10-02  
**Builds on:** [ADR_LINK_PLANE.md](ADR_LINK_PLANE.md) (LinkId, `PeerPresence`, `MeshRuntime`), A024 (carrier links)

## Context

Amp links run on ADP over UDP. Some networks block UDP outright (many corporate,
hotel, campus and some mobile networks); some allow only HTTPS on 443, and some
only through an HTTP proxy or with TLS inspection. A peer on such a network cannot
reach the mesh at all today.

**Goal: getting through those networks.** Not security — MSH already authenticates
peers and encrypts with ML-KEM / ML-DSA and AEAD — and not hiding from a censor
that fingerprints traffic patterns.

Today's layering already separates what is UDP-specific:

| Layer | UDP-specific? |
|-------|---------------|
| L1 ADP (`DatagramIo`, `Endpoint`, `Connection`) | Yes, by design: datagrams with its own reliability (ack, retransmit, replay window, liveness) |
| L2 MSH session, L3 channel mux | No — they send and receive sealed frames |
| `PeerLink` | Two transports already: `TransportClass::Adp` and `::Carrier` (A024: MSH and sealed frames over a channel, no ADP) |
| `PeerLinkManager` | One ADP `Endpoint` for dial / accept / clock / liveness |
| Multiaddr | Only `/ip4\|ip6/…/udp/<port>/adp/1.0.0/p2p/<id>` |
| `MeshRuntime` (product API) | No — dial keys and multiaddr strings |

## Decision (proposed)

### 1. A third link transport: `TransportClass::Stream`

MSH runs directly over a framed byte stream — no ADP underneath. The stream is
reliable and ordered, so neither ADP's reliability nor the carrier's reliable lane
(`CarrierLane`) is used. The carrier path is the template: it already runs the MSH
handshake and sealed frames as whole frames over a non-ADP transport.

- **Framing:** each MSH message / sealed frame is one length-prefixed record on the stream.
- **Backpressure:** a byte cap on the socket's pending send queue feeds the mux's
  `TransportCredits`, as the ADP window does today.
- **Liveness:** no ADP liveness on a stream. A socket error or close drops the link
  at once; a peer that goes silent is caught by the link keepalive tiers
  ([KEEPALIVE.md](KEEPALIVE.md)) sent as control frames on the stream.
- **I/O:** non-blocking sockets driven by `MeshPump` (Exclusive Drive, ADR_LINK_PLANE
  §4): listen, accept, connect with a deadline on the Amp clock.

L2, L3, `ChannelMux`, channel policies and every product L4 are unchanged.

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

1. Multiaddr tokens: `amp/1.0.0` for streams vs reusing `adp/1.0.0`; `/wss` vs `/tls/ws`.
2. Fallback head start and how "this network blocks UDP" is remembered.
3. Calls over a stream: allow, or prefer a relay over UDP when one is reachable.
4. TLS dependency: BoringSSL in Amp behind an interface, vs injected by the product.
5. Whether plain TCP (no TLS) is worth shipping on its own, or only as the base of TLS.
