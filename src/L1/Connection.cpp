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

Connection::Connection(Endpoint& endpoint, OpenParams params)
    : endpoint_(&endpoint), id_(params.id), binder_(params.key), peer_(params.peer),
      params_(std::move(params)), rx_be_(params_.replay_window, /*slide_on_gap=*/true),
      // Must cover the whole send window: a seq the sender may legitimately
      // have in flight cannot be "too far".
      rx_rel_(std::max(params_.replay_window, params_.reliable_window), /*slide_on_gap=*/false) {
  authenticated_max_rtx_ = params_.max_rtx;
  if (params_.reduce_rtx_until_authenticated) {
    params_.max_rtx = std::min(params_.max_rtx, kPreAuthMaxRtx);
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

void Connection::UpgradeBinder(PeerKey key) {
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
    m.payload = std::move(it->second);
    rx_rel_hold_.erase(it);
    ++rx_rel_next_deliver_;
    on_message_(m);
  }
}

Connection::Roe<void> Connection::SendPacket(PacketType type, uint32_t seq, std::span<const uint8_t> payload,
                                 int64_t now_ms) {
  if (peer_.port == 0 && type != PacketType::Close) {
    // Allow send only if peer known (except we still encode close).
  }
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
  auto sealed = binder_.Seal(std::move(*encoded));
  if (!sealed) {
    return Failure::Of(Err::WireError, sealed.error().message);
  }
  auto sent = endpoint_->SendRaw(peer_, *sealed);
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
  if (payload.size() > kMaxPayload) {
    return Failure::Of(Err::PayloadTooLarge, "adp: payload too large");
  }
  const int64_t now = endpoint_->GetClock().NowMs();
  if (qos == QosClass::BestEffort) {
    if (tx_seq_be_ == 0xffffffffu) {
      return Failure::Of(Err::SeqWrap, "adp: seq wrap");
    }
    ++tx_seq_be_;
    return SendPacketAsFailure(PacketType::DataBestEffort, tx_seq_be_, payload, now);
  }
  if (outstanding_.size() >= params_.reliable_window) {
    return Failure::Of(Err::WindowFull, "adp: reliable window full");
  }
  if (tx_seq_rel_ == 0xffffffffu) {
    return Failure::Of(Err::SeqWrap, "adp: seq wrap");
  }
  ++tx_seq_rel_;
  Outstanding o;
  o.seq = tx_seq_rel_;
  o.payload.assign(payload.begin(), payload.end());
  o.next_rtx_ms = now + params_.rtx_interval_ms;
  o.attempts = 0;
  o.first_sent_ms = now;
  // The seq is taken: track the packet whatever this first send does. A
  // failed send is then just a lost packet that the retransmit timer resends;
  // dropping it would leave a seq gap the receiver waits on forever.
  outstanding_.push_back(std::move(o));
  endpoint_->NoteReliableSent();
  ++stats_.reliable_sent;
  auto sent = SendPacket(PacketType::DataReliable, tx_seq_rel_, payload, now);
  if (!sent && peer_unreachable_) {
    return sent.error();  // no route: the link layer tears this path down
  }
  return {};
}

void Connection::NoteRttSample(const int64_t rtt_ms) {
  endpoint_->NoteRttSample(rtt_ms);
  const int64_t sample = rtt_ms < 0 ? 0 : rtt_ms;
  ++stats_.rtt_samples;
  stats_.rtt_sum_ms += static_cast<uint64_t>(sample);
  stats_.srtt_ms = stats_.srtt_ms < 0 ? sample : stats_.srtt_ms + (sample - stats_.srtt_ms) / 8;
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
    o.next_rtx_ms = now_ms + params_.rtx_interval_ms;
    endpoint_->NoteRetransmit();
    ++stats_.retransmits;
    (void)SendPacket(PacketType::DataReliable, o.seq, o.payload, now_ms);
  }
  // Drop permanently failed from front.
  while (!outstanding_.empty() && outstanding_.front().attempts >= params_.max_rtx &&
         now_ms >= outstanding_.front().next_rtx_ms) {
    endpoint_->NoteReliableLost();
    ++stats_.reliable_lost;
    outstanding_.pop_front();
  }
}

void Connection::HandleDatagram(const IpEndpoint& from, std::span<const uint8_t> datagram,
                                int64_t now_ms) {
  if (!binder_.Verify(datagram)) {
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

void Connection::HandleAuthenticated(const WirePacket& pkt, const IpEndpoint& from, int64_t now_ms) {
  if (!AcceptSkew(pkt.timestamp_ms, now_ms)) {
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
    fresh = fresh_data = rx_be_.Accept(pkt.seq);
    break;
  case PacketType::DataReliable:
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
      const bool room = !on_message_ || rx_rel_hold_.size() < params_.reliable_window ||
                        pkt.seq == rx_rel_next_deliver_;
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
  if (!have_auth_rx_ts_ || static_cast<int32_t>(pkt.timestamp_ms - max_auth_rx_ts_) > 0) {
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
        NoteRttSample(now_ms - o.first_sent_ms);  // Karn: never a retransmitted one
        break;
      }
    }
    outstanding_.erase(std::remove_if(outstanding_.begin(), outstanding_.end(),
                                      [&](const Outstanding& o) { return o.seq == pkt.seq; }),
                       outstanding_.end());
    break;
  }
  case PacketType::Close: {
    // Pre-auth, the binder key is the well-known pre-session key: anyone can forge this HMAC.
    if (!authenticated_) {
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
  case PacketType::DataBestEffort: {
    if (!fresh_data) {
      break;
    }
    if (on_message_) {
      Message m;
      m.assoc = id_;
      m.seq = pkt.seq;
      m.qos = QosClass::BestEffort;
      m.payload = pkt.payload;
      on_message_(m);
    }
    break;
  }
  case PacketType::DataReliable: {
    if (ack_reliable) {
      (void)SendPacket(PacketType::Ack, pkt.seq, {}, now_ms);
    }
    if (!fresh_data) {
      break;
    }
    // The hold is capped to the reliable window (checked above): a forged
    // out-of-order seq per packet cannot grow it without bound.
    if (on_message_) {
      rx_rel_hold_[pkt.seq] = pkt.payload;
      DeliverReliableInOrder();
    }
    break;
  }
  }
}

} // namespace pp::adp
