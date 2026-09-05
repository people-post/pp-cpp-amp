# Amp ownership boundary

**Role:** Amp is the shared **transport stack** (L1 ADP, L2 MSH, L3 channel mux, link).  
It is the only mesh transport shared by pp-browser and pp-ledger. It is **not** a shared application or sync library.

## Owns (stay here)

| Concern | Notes |
|---------|--------|
| Association, session crypto, rekey | L1–L2 |
| Channel OPEN / CLOSE, `protocol_id`, QoS classes | L3 |
| FRAG + reassembly, Bulk credit / `WindowFull` | Large Reliable messages |
| Neutral policy factories | `ControlJsonChannelPolicy`, `CapabilityChannelPolicy`, `MakeBulkChannelPolicy` / `BulkChannelPolicy`, `CallMediaChannelPolicy`, `CircuitCarrierChannelPolicy` |
| Amp-owned plumbing ids | e.g. `/amp/circuit-carrier/1.0.0` |
| Link dial / warm / keepalive | `PeerLinkManager`, `AmpStack` |

## Does not own (product L4)

| Concern | Home |
|---------|------|
| Chat envelopes, history pull, payment promises | pp-browser **rpc** |
| Content-addressed blob fetch/push, DEK, CDN ladder | pp-browser **blob** |
| Piece / bitfield / have-want / multi-peer swarm | pp-browser (future **blob** ops) — not Amp |
| Call-media / media-relay session machines | pp-browser **realtime** |
| Punch / circuit tunnel product coordinators | pp-browser |
| `/pp-ledger/rpc/1.0.0` request types, tip/range sync, checkpoints | **pp-ledger** |
| `ledger_gateway` hop policy / MeshHost advertise | pp-browser / pp-node |

## Anti-duplication rules

1. **Share transport, not protocols** — Both products call Amp Bulk/FRAG; each keeps its own `protocol_id` and conversation SM.
2. **Prefer thin product glue twice** over a fat Amp L4 service (no Amp “TorrentService” / “LedgerSync”).
3. **Product channel policies** may wrap `MakeBulkChannelPolicy()` / zero-arg `ChatBlobChannelPolicy()` (e.g. `read_once`, timeouts). Do not re-copy Bulk class/size defaults in product headers.
4. **Ledger embed** links pp-ledger client types and supplies `ILedgerTransport` / Amp OPEN — do not reimplement ledger codecs in Amp or browser.
5. **Aliases** — `ChatBlobChannelPolicy` / `kMaxChatBlobFrameBytes` remain as deprecated names for `BulkChannelPolicy` / `kMaxBulkFrameBytes`.

## Related

- pp-browser: `docs/contracts/L4_PROTOCOL_KINDS.md`, `docs/contracts/AMP-CHANNEL.md`
- pp-ledger: `docs/amp-transport.md`, `docs/platform-integration.md`
