#include "amp/L1/Connection.h"

#include "amp/L1/Endpoint.h"
#include "amp/L1/WireCodec.h"

#include "crypto/SodiumUtil.h"

#include <sodium.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>

namespace pp::adp {
namespace {

PacketSpace SpaceOf(const PacketType type) {
  return type == PacketType::DataReliable ? PacketSpace::Reliable : PacketSpace::BestEffort;
}

} // namespace

Connection::Connection(Endpoint& endpoint, OpenParams params)
    : endpoint_(&endpoint),
      id_(params.id),
      binder_(params.key),
      peer_(params.peer),
      params_(std::move(params)),
      rx_be_(params_.replay_window, /*slide_on_gap=*/true),
      // Must cover the whole send window: a seq the sender may legitimately
      // have in flight cannot be "too far".
      rx_rel_(std::max(params_.replay_window, params_.reliable_window), /*slide_on_gap=*/false) {
  authenticated_max_rtx_ = params_.max_rtx;
  if (params_.reduce_rtx_until_authenticated) {
    params_.max_rtx = std::min(params_.max_rtx, params_.dialer_backoff ? kPreAuthDialMaxRtx : kPreAuthMaxRtx);
  } else {
    // Opted out of pre-auth hardening (explicit Open() outside PeerLinkManager, e.g. tests):
    // keep the old behavior of trusting Close / Keepalive from the first packet.
    authenticated_ = true;
  }
}

Connection::Roe<std::shared_ptr<Connection>> Connection::Open(Endpoint& endpoint, OpenParams params) {
  if (params.mint_id) {
    bool zero = true;
    for (uint8_t b : params.id.bytes) {
      if (b != 0) {
        zero = false;
        break;
      }
    }
    if (zero) {
      // CSPRNG, not clock/pointer/counter mixing: an id guessable from wall time let a
      // third party address a UDP datagram at someone else's live association.
      pp::EnsureSodiumInit();
      randombytes_buf(params.id.bytes.data(), params.id.bytes.size());
    }
  }
  auto conn = std::shared_ptr<Connection>(new Connection(endpoint, std::move(params)));
  return conn;
}

void Connection::Close() {
  if (closed_) {
    return;
  }
  const int64_t now = endpoint_->GetClock().NowMs();
  (void)SendPacket(PacketType::Close, 0, {}, now);
  closed_ = true;
  endpoint_->Unregister(id_);
}

void Connection::Abandon() {
  if (closed_) {
    return;
  }
  closed_ = true;
  endpoint_->Unregister(id_);
}

int64_t Connection::RetransmitIntervalMs(int doublings) const {
  if (authenticated_) {
    // Each resend doubles the wait (RFC 6298 §5.5), up to kMaxRtoMs.
    doublings = std::min(doublings, 8);
    return std::min(RtoMs() << doublings, std::max(kMaxRtoMs, params_.rtx_interval_ms));
  }
  if (!params_.dialer_backoff) {
    return params_.rtx_interval_ms;
  }
  return params_.rtx_interval_ms << std::min(doublings, 16); // a dialer's handshake: doubling, like SYN retries
}

int64_t Connection::RtoMs() const {
  if (stats_.srtt_ms < 0) {
    return params_.rtx_interval_ms;
  }
  const int64_t rto = stats_.srtt_ms + std::max(kRtoGranularityMs, 4 * stats_.rttvar_ms);
  return std::clamp(rto, params_.rtx_interval_ms, std::max(kMaxRtoMs, params_.rtx_interval_ms));
}

void Connection::SetPeerEndpoint(IpEndpoint peer) {
  if (peer_ == peer) {
    return;
  }
  const IpEndpoint from = peer_;
  peer_ = peer;
  if (on_path_change_) {
    on_path_change_(from, peer_);
  }
}

void Connection::SetProtection(std::shared_ptr<const IPacketProtection> protection) {
  protection_ = std::move(protection);
}

void Connection::UpgradeBinder(PeerKey key) {
  if (!pre_upgrade_key_) {
    pre_upgrade_key_ = binder_.Key();
  }
  binder_.SetKey(key);
  params_.max_rtx = authenticated_max_rtx_;
  authenticated_ = true;
}

bool Connection::LooksAlive(int64_t now_ms) const {
  if (closed_ || last_auth_rx_ms_ == 0) {
    return false;
  }
  return (now_ms - last_auth_rx_ms_) <= LivenessWindowMs();
}

int64_t Connection::LivenessWindowMs() const {
  const int64_t cadence = std::max(local_keepalive_interval_ms_, peer_keepalive_interval_ms_);
  return std::max(params_.alive_timeout_ms, cadence * kKeepaliveLivenessNumerator / kKeepaliveLivenessDenominator);
}

Connection::Roe<void> Connection::SendKeepalive(const int64_t now_ms, const uint32_t interval_ms) {
  local_keepalive_interval_ms_ = interval_ms;
  return SendKeepalivePacket(now_ms, interval_ms, kKeepaliveFlagEchoRequest);
}

Connection::Roe<void> Connection::StopKeepalive(const int64_t now_ms) {
  if (local_keepalive_interval_ms_ == 0) {
    return Roe<void>();
  }
  local_keepalive_interval_ms_ = 0;
  return SendKeepalivePacket(now_ms, 0, 0);
}

Connection::Roe<void> Connection::SendKeepalivePacket(const int64_t now_ms, const uint32_t interval_ms,
                                                      const uint8_t flags) {
  if (closed_) {
    return Failure::Of(Err::Closed, "adp: keepalive on closed connection");
  }
  if (peer_.port == 0) {
    return Failure::Of(Err::WireError, "adp: keepalive without peer endpoint");
  }
  const std::array<uint8_t, kKeepalivePayloadBytes> payload = {
      static_cast<uint8_t>(interval_ms >> 24), static_cast<uint8_t>(interval_ms >> 16),
      static_cast<uint8_t>(interval_ms >> 8), static_cast<uint8_t>(interval_ms), flags};
  return SendPacket(PacketType::Keepalive, 0, payload, now_ms);
}

Connection::Roe<void> Connection::SendProbe(const int64_t now_ms) {
  return SendKeepalivePacket(now_ms, local_keepalive_interval_ms_, kKeepaliveFlagEchoRequest);
}

void Connection::HandleKeepalive(const WirePacket& pkt, const int64_t now_ms) {
  if (pkt.payload.size() < kKeepalivePayloadBytes) {
    return;
  }
  const auto& p = pkt.payload;
  const uint32_t announced = (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
                             (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
  peer_keepalive_interval_ms_ = std::min(announced, kMaxPeerKeepaliveIntervalMs);
  if ((p[4] & kKeepaliveFlagEchoRequest) != 0) {
    // Echo carries our own cadence (0 if we keep none) and never requests an echo back.
    (void)SendKeepalivePacket(now_ms, local_keepalive_interval_ms_, 0);
  }
}

uint32_t Connection::TruncTs(int64_t now_ms) const {
  return static_cast<uint32_t>(static_cast<uint64_t>(now_ms) & 0xffffffffull);
}

bool Connection::AcceptSkew(uint32_t ts, int64_t now_ms) const {
  const uint32_t now_trunc = TruncTs(now_ms);
  const int64_t delta = static_cast<int64_t>(static_cast<int32_t>(now_trunc - ts));
  return delta <= params_.skew_ms && delta >= -params_.skew_ms;
}

void Connection::MaybeLearnPath(const IpEndpoint& from) {
  // Unspecified address (0.0.0.0 / ::) is never dialable regardless of port (A4 / B3).
  const size_t n = peer_.family == IpEndpoint::Family::V4 ? 4 : 16;
  bool unspecified = true;
  for (size_t i = 0; i < n; ++i) {
    if (peer_.addr[i] != 0) {
      unspecified = false;
      break;
    }
  }
  if (unspecified) {
    SetPeerEndpoint(from);
    return;
  }
  if (from != peer_) {
    // Authenticated packet from a new path — migrate (NAT remap / handoff).
    SetPeerEndpoint(from);
  }
}

void Connection::DeliverReliableInOrder() {
  if (!on_message_) {
    return;
  }
  while (true) {
    auto it = rx_rel_hold_.find(rx_rel_next_deliver_);
    if (it == rx_rel_hold_.end()) {
      break;
    }
    Message m;
    m.assoc = id_;
    m.seq = it->first;
    m.qos = QosClass::Reliable;
    m.sealed = it->second.sealed;
    m.payload = std::move(it->second.payload);
    rx_rel_hold_.erase(it);
    ++rx_rel_next_deliver_;
    on_message_(m);
  }
}

Connection::Roe<std::vector<uint8_t>> Connection::SealDatagram(const PacketType type, const uint32_t seq,
                                                               const std::span<const uint8_t> payload) const {
  if (!protection_) {
    return Failure::Of(Err::WireError, "adp: no session keys to seal with");
  }
  SealedHeader h;
  h.type = type;
  h.assoc = id_;
  h.number = seq;
  const auto header = WireCodec::EncodeSealedHeader(h);
  std::vector<uint8_t> out;
  out.reserve(header.size() + payload.size() + kPacketTagBytes);
  out.assign(header.begin(), header.end());
  if (auto sealed = protection_->SealPacket(SpaceOf(type), seq, header, payload, out); !sealed) {
    return Failure::Of(Err::WireError, sealed.error().message);
  }
  return out;
}

Connection::Roe<WirePacket> Connection::OpenDatagram(const std::span<const uint8_t> datagram,
                                                     const int64_t now_ms) const {
  if (!protection_) {
    return Failure::Of(Err::WireError, "adp: sealed packet before session keys");
  }
  auto header = WireCodec::DecodeSealedHeader(datagram);
  if (!header) {
    return Failure::Of(Err::WireError, header.error().message);
  }
  auto opened = protection_->OpenPacket(SpaceOf(header->type), header->number, datagram.first(kSealedHeaderBytes),
                                        datagram.subspan(kSealedHeaderBytes), now_ms);
  if (!opened) {
    return Failure::Of(Err::WireError, opened.error().message);
  }
  WirePacket pkt;
  pkt.type = header->type;
  pkt.assoc = header->assoc;
  pkt.seq = header->number;
  pkt.payload = std::move(*opened);
  return pkt;
}

Connection::Roe<void> Connection::SendPacket(PacketType type, uint32_t seq, std::span<const uint8_t> payload,
                                             int64_t now_ms, const PeerKey* key) {
  std::vector<uint8_t> datagram;
  if (IsSealed(type)) {
    auto sealed = SealDatagram(type, seq, payload);
    if (!sealed) {
      return sealed.error();
    }
    datagram = std::move(*sealed);
  } else {
    WirePacket pkt;
    pkt.version = kWireVersion;
    pkt.type = type;
    pkt.assoc = id_;
    pkt.seq = seq;
    pkt.timestamp_ms = TruncTs(now_ms);
    pkt.payload.assign(payload.begin(), payload.end());
    auto encoded = WireCodec::Encode(pkt);
    if (!encoded) {
      return Failure::Of(Err::WireError, encoded.error().message);
    }
    auto maced = key ? HmacBinder(*key).Seal(std::move(*encoded)) : binder_.Seal(std::move(*encoded));
    if (!maced) {
      return Failure::Of(Err::WireError, maced.error().message);
    }
    datagram = std::move(*maced);
  }
  auto sent = endpoint_->SendRaw(peer_, datagram);
  if (!sent) {
    // Checked here, for every send (data, retransmit, ack, keepalive), whether or not the caller
    // looks at the result.
    if (IsUnreachableSendError(sent.error())) {
      peer_unreachable_ = true;
      return Failure::Of(Err::Unreachable, sent.error().message);
    }
    return Failure::Of(Err::WireError, sent.error().message);
  }
  return {};
}

Connection::Roe<void> Connection::SendPacketAsFailure(const PacketType type, const uint32_t seq,
                                                      const std::span<const uint8_t> payload, const int64_t now_ms) {
  auto sent = SendPacket(type, seq, payload, now_ms);
  if (!sent) {
    return Failure::Of(Err::WireError, sent.error().message);
  }
  return {};
}

Connection::Roe<void> Connection::Send(QosClass qos, std::span<const uint8_t> payload) {
  if (closed_) {
    return Failure::Of(Err::Closed, "adp: send on closed connection");
  }
  // A sealed packet's header and tag are smaller than an HMAC packet's: it carries a little more.
  if (payload.size() > (Sealing() ? kMaxSealedPayload : kMaxPayload)) {
    return Failure::Of(Err::PayloadTooLarge, "adp: payload too large");
  }
  const int64_t now = endpoint_->GetClock().NowMs();
  if (qos == QosClass::BestEffort) {
    if (tx_seq_be_ == 0xffffffffu) {
      return Failure::Of(Err::SeqWrap, "adp: seq wrap");
    }
    ++tx_seq_be_;
    return SendPacketAsFailure(Sealing() ? PacketType::DataBestEffort : PacketType::PlainBestEffort, tx_seq_be_,
                               payload, now);
  }
  if (outstanding_.size() >= params_.reliable_window) {
    return Failure::Of(Err::WindowFull, "adp: reliable window full");
  }
  if (tx_seq_rel_ == 0xffffffffu) {
    return Failure::Of(Err::SeqWrap, "adp: seq wrap");
  }
  ++tx_seq_rel_;
  Outstanding o;
  o.type = Sealing() ? PacketType::DataReliable : PacketType::PlainReliable;
  o.before_upgrade = !pre_upgrade_key_;
  o.seq = tx_seq_rel_;
  o.payload.assign(payload.begin(), payload.end());
  // A new packet starts from the backed-off timeout (RFC 6298 §5.7): before a clean round trip, a path slower
  // than rtx_interval_ms would otherwise have every packet resent before its ack, and no ack would ever give a
  // sample (Karn).
  o.base_backoff = authenticated_ ? rto_backoff_ : 0;
  o.next_rtx_ms = now + RetransmitIntervalMs(o.base_backoff);
  o.attempts = 0;
  o.first_sent_ms = now;
  // The seq is taken: track the packet whatever this first send does. A
  // failed send is then just a lost packet that the retransmit timer resends;
  // dropping it would leave a seq gap the receiver waits on forever.
  outstanding_.push_back(std::move(o));
  endpoint_->NoteReliableSent();
  ++stats_.reliable_sent;
  auto sent = SendPacket(outstanding_.back().type, tx_seq_rel_, payload, now);
  if (!sent && peer_unreachable_) {
    return sent.error(); // no route: the link layer tears this path down
  }
  return {};
}

void Connection::NoteRttSample(const int64_t rtt_ms) {
  endpoint_->NoteRttSample(rtt_ms);
  const int64_t sample = rtt_ms < 0 ? 0 : rtt_ms;
  ++stats_.rtt_samples;
  rto_backoff_ = 0; // a clean round trip: the timeout comes from it again
  stats_.rtt_sum_ms += static_cast<uint64_t>(sample);
  if (stats_.srtt_ms < 0) {
    stats_.srtt_ms = sample;
    stats_.rttvar_ms = sample / 2;
  } else {
    const int64_t error = sample > stats_.srtt_ms ? sample - stats_.srtt_ms : stats_.srtt_ms - sample;
    stats_.rttvar_ms += (error - stats_.rttvar_ms) / 4;
    stats_.srtt_ms += (sample - stats_.srtt_ms) / 8;
  }
}

void Connection::Tick(int64_t now_ms) {
  if (closed_) {
    return;
  }
  for (auto& o : outstanding_) {
    if (now_ms < o.next_rtx_ms) {
      continue;
    }
    if (o.attempts >= params_.max_rtx) {
      continue;
    }
    ++o.attempts;
    const int doublings = o.base_backoff + o.attempts;
    if (authenticated_) {
      rto_backoff_ = std::max(rto_backoff_, std::min(doublings, 8));
    }
    o.next_rtx_ms = now_ms + RetransmitIntervalMs(doublings);
    endpoint_->NoteRetransmit();
    ++stats_.retransmits;
    // Resealed under the current key: the same bytes under the same (space, number), so the same ciphertext unless
    // a rekey came between. A plain (handshake) packet from before the upgrade goes under the key it first went out
    // with: the peer of a handshake that finished here first still checks the pre-session key, and a resend under
    // the new one never got through (the dial timed out).
    const PeerKey* key = o.before_upgrade && pre_upgrade_key_ && !IsSealed(o.type) ? &*pre_upgrade_key_ : nullptr;
    (void)SendPacket(o.type, o.seq, o.payload, now_ms, key);
  }
  // Drop permanently failed from front.
  while (!outstanding_.empty() && outstanding_.front().attempts >= params_.max_rtx &&
         now_ms >= outstanding_.front().next_rtx_ms) {
    endpoint_->NoteReliableLost();
    ++stats_.reliable_lost;
    outstanding_.pop_front();
  }
}

void Connection::HandleDatagram(const IpEndpoint& from, std::span<const uint8_t> datagram, int64_t now_ms) {
  auto type = WireCodec::PeekType(datagram);
  if (!type) {
    endpoint_->NoteRejected();
    return;
  }
  if (IsSealed(*type)) {
    // Opened before anything trusts it: the replay window, liveness and the path move only on a packet that opened.
    auto opened = OpenDatagram(datagram, now_ms);
    if (!opened) {
      endpoint_->NoteRejected();
      return;
    }
    HandleAuthenticated(*opened, from, now_ms);
    return;
  }
  if (!binder_.Verify(datagram)) {
    if (pre_upgrade_key_ && HmacBinder(*pre_upgrade_key_).Verify(datagram)) {
      if (auto decoded = WireCodec::Decode(datagram)) {
        HandlePreUpgradeKeyPacket(*decoded, now_ms);
        return;
      }
    }
    endpoint_->NoteRejected();
    return;
  }
  auto decoded = WireCodec::Decode(datagram);
  if (!decoded) {
    endpoint_->NoteRejected();
    return;
  }
  HandleAuthenticated(*decoded, from, now_ms);
}

bool Connection::RefusePlainAfterKeys(const WirePacket& pkt) {
  if (!protection_ || IsSealed(pkt.type)) {
    return false;
  }
  if (pkt.type == PacketType::PlainBestEffort) {
    return true;
  }
  if (pkt.type == PacketType::PlainReliable) {
    // The handshake's last packets may still be resent after both ends have keys: ack a duplicate so its sender
    // stops, take nothing new in the clear.
    if (rx_rel_.Classify(pkt.seq) == ReplayWindow::Verdict::Duplicate) {
      (void)SendPacket(PacketType::Ack, pkt.seq, {}, endpoint_->GetClock().NowMs());
    }
    return true;
  }
  return false;
}

void Connection::HandlePreUpgradeKeyPacket(const WirePacket& pkt, const int64_t now_ms) {
  if (!AcceptSkew(pkt.timestamp_ms, now_ms)) {
    return;
  }
  if (pkt.type == PacketType::Ack) {
    // Only for a packet we sent under that key: a forged ack (anyone may hold the pre-session key) can stop a
    // handshake resend at most, never data's.
    outstanding_.erase(std::remove_if(outstanding_.begin(), outstanding_.end(),
                                      [&](const Outstanding& o) { return o.before_upgrade && o.seq == pkt.seq; }),
                       outstanding_.end());
    return;
  }
  if (pkt.type == PacketType::PlainReliable && rx_rel_.Classify(pkt.seq) == ReplayWindow::Verdict::Duplicate) {
    // The peer's handshake packet again (our ack was lost or went out under the new key): ack it under the key it
    // came with, which is the one the peer checks acks of it with.
    (void)SendPacket(PacketType::Ack, pkt.seq, {}, now_ms, &*pre_upgrade_key_);
  }
}

void Connection::HandleAuthenticated(const WirePacket& pkt, const IpEndpoint& from, int64_t now_ms) {
  const bool sealed = IsSealed(pkt.type);
  // A sealed packet has no timestamp: under a session key its replay window is its freshness.
  if (!sealed && !AcceptSkew(pkt.timestamp_ms, now_ms)) {
    return;
  }
  if (RefusePlainAfterKeys(pkt)) {
    return;
  }
  // Only a fresh packet proves the peer is alive and may move the path (A003): data by its replay
  // window, seq-0 control packets (ack / close / keepalive) by being newer than anything
  // authenticated so far. A replayed packet from a new address must not redirect the
  // association; a real rebind's packets are the newest ones.
  bool fresh_data = false;
  bool fresh = false;
  bool ack_reliable = false;
  switch (pkt.type) {
    case PacketType::DataBestEffort:
    case PacketType::PlainBestEffort:
      fresh = fresh_data = rx_be_.Accept(pkt.seq);
      break;
    case PacketType::DataReliable:
    case PacketType::PlainReliable:
      // Record (and later ACK) only a packet we keep: one ACKed but dropped is
      // never resent and stalls in-order delivery for good. Duplicates are ACKed
      // so the sender stops resending; too far ahead, or no room in the hold,
      // stays unACKed and is resent.
      switch (rx_rel_.Classify(pkt.seq)) {
        case ReplayWindow::Verdict::Duplicate:
          ack_reliable = true;
          break;
        case ReplayWindow::Verdict::TooFar:
          break;
        case ReplayWindow::Verdict::Fresh: {
          const bool room =
              !on_message_ || rx_rel_hold_.size() < params_.reliable_window || pkt.seq == rx_rel_next_deliver_;
          if (room) {
            fresh = fresh_data = rx_rel_.Accept(pkt.seq);
            ack_reliable = fresh_data;
          }
          break;
        }
      }
      break;
    default:
      // Serial arithmetic on the 32-bit wire timestamp (it wraps, as in AcceptSkew).
      fresh = !have_auth_rx_ts_ || static_cast<int32_t>(pkt.timestamp_ms - max_auth_rx_ts_) > 0;
      break;
  }
  if (!sealed && (!have_auth_rx_ts_ || static_cast<int32_t>(pkt.timestamp_ms - max_auth_rx_ts_) > 0)) {
    max_auth_rx_ts_ = pkt.timestamp_ms;
    have_auth_rx_ts_ = true;
  }
  if (fresh) {
    last_auth_rx_ms_ = now_ms;
    MaybeLearnPath(from);
  }

  switch (pkt.type) {
    case PacketType::Ack: {
      for (const Outstanding& o : outstanding_) {
        if (o.seq == pkt.seq && o.attempts == 0) {
          NoteRttSample(now_ms - o.first_sent_ms); // Karn: never a retransmitted one
          break;
        }
      }
      outstanding_.erase(std::remove_if(outstanding_.begin(), outstanding_.end(),
                                        [&](const Outstanding& o) { return o.seq == pkt.seq; }),
                         outstanding_.end());
      break;
    }
    case PacketType::Close: {
      // Pre-auth, the binder key is the well-known pre-session key: anyone who sees the association id
      // can forge this. A dialer still takes it as a refusal (dialer_backoff): like a TCP reset, only an
      // on-path observer could forge it, and it could drop the handshake as well.
      if (!authenticated_ && !params_.dialer_backoff) {
        break;
      }
      peer_closed_ = true;
      closed_ = true;
      endpoint_->Unregister(id_);
      break;
    }
    case PacketType::Keepalive: {
      if (!authenticated_) {
        break;
      }
      HandleKeepalive(pkt, now_ms);
      break;
    }
    case PacketType::DataBestEffort:
    case PacketType::PlainBestEffort: {
      if (!fresh_data) {
        break;
      }
      if (on_message_) {
        Message m;
        m.assoc = id_;
        m.seq = pkt.seq;
        m.qos = QosClass::BestEffort;
        m.sealed = sealed;
        m.payload = pkt.payload;
        on_message_(m);
      }
      break;
    }
    case PacketType::DataReliable:
    case PacketType::PlainReliable: {
      if (ack_reliable) {
        (void)SendPacket(PacketType::Ack, pkt.seq, {}, now_ms);
      }
      if (!fresh_data) {
        break;
      }
      // The hold is capped to the reliable window (checked above): a forged
      // out-of-order seq per packet cannot grow it without bound.
      if (on_message_) {
        rx_rel_hold_[pkt.seq] = HeldReliable{sealed, pkt.payload};
        DeliverReliableInOrder();
      }
      break;
    }
  }
}

void Connection::HandleVersionNegotiation(const std::vector<uint8_t>& versions) {
  if (authenticated_ || !params_.dialer_backoff || closed_) {
    return;
  }
  peer_version_mismatch_ = true;
  peer_wire_versions_ = versions;
  closed_ = true;
  endpoint_->Unregister(id_);
}

} // namespace pp::adp
