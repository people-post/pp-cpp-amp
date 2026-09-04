#include "amp/L1/Connection.h"

#include "amp/L1/Endpoint.h"
#include "amp/L1/WireCodec.h"

#include <algorithm>
#include <cstring>

namespace pp::adp {

Connection::Connection(Endpoint& endpoint, OpenParams params)
    : endpoint_(&endpoint), id_(params.id), binder_(params.key), peer_(params.peer),
      params_(std::move(params)), rx_be_(params_.replay_window), rx_rel_(params_.replay_window) {}

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
      // Mint from clock + pointer entropy (tests can set explicit ids).
      const int64_t now = endpoint.GetClock().NowMs();
      for (size_t i = 0; i < 8; ++i) {
        params.id.bytes[i] = static_cast<uint8_t>((now >> (i * 8)) & 0xff);
      }
      const auto ent = reinterpret_cast<uintptr_t>(&endpoint) ^ static_cast<uintptr_t>(now * 2654435761u);
      for (size_t i = 0; i < 8; ++i) {
        params.id.bytes[8 + i] = static_cast<uint8_t>((ent >> (i * 8)) & 0xff);
      }
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

void Connection::UpgradeBinder(PeerKey key) { binder_.SetKey(key); }

bool Connection::LooksAlive(int64_t now_ms) const {
  if (closed_ || last_auth_rx_ms_ == 0) {
    return false;
  }
  return (now_ms - last_auth_rx_ms_) <= kAliveTimeoutMs;
}

Connection::Roe<void> Connection::SendKeepalive(const int64_t now_ms) {
  if (closed_) {
    return Failure::Of(Err::Closed, "adp: keepalive on closed connection");
  }
  if (peer_.port == 0) {
    return Failure::Of(Err::WireError, "adp: keepalive without peer endpoint");
  }
  return SendPacket(PacketType::Keepalive, 0, {}, now_ms);
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
  if (peer_.port == 0 && peer_.addr[0] == 0 && peer_.addr[1] == 0 && peer_.addr[2] == 0 &&
      peer_.addr[3] == 0) {
    SetPeerEndpoint(from);
    return;
  }
  if (from != peer_) {
    // Authenticated packet from a new path — migrate (NAT remap / handoff).
    SetPeerEndpoint(from);
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
  auto err = SendPacketAsFailure(PacketType::DataReliable, o.seq, payload, now);
  if (!err) {
    return err.error();
  }
  outstanding_.push_back(std::move(o));
  return {};
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
    (void)SendPacket(PacketType::DataReliable, o.seq, o.payload, now_ms);
  }
  // Drop permanently failed from front.
  while (!outstanding_.empty() && outstanding_.front().attempts >= params_.max_rtx &&
         now_ms >= outstanding_.front().next_rtx_ms) {
    outstanding_.pop_front();
  }
}

void Connection::HandleDatagram(const IpEndpoint& from, std::span<const uint8_t> datagram,
                                int64_t now_ms) {
  if (!binder_.Verify(datagram)) {
    return;
  }
  auto decoded = WireCodec::Decode(datagram);
  if (!decoded) {
    return;
  }
  HandleAuthenticated(*decoded, from, now_ms);
}

void Connection::HandleAuthenticated(const WirePacket& pkt, const IpEndpoint& from, int64_t now_ms) {
  if (!AcceptSkew(pkt.timestamp_ms, now_ms)) {
    return;
  }
  last_auth_rx_ms_ = now_ms;
  MaybeLearnPath(from);

  switch (pkt.type) {
  case PacketType::Ack: {
    outstanding_.erase(std::remove_if(outstanding_.begin(), outstanding_.end(),
                                      [&](const Outstanding& o) { return o.seq == pkt.seq; }),
                       outstanding_.end());
    break;
  }
  case PacketType::Close: {
    peer_closed_ = true;
    closed_ = true;
    endpoint_->Unregister(id_);
    break;
  }
  case PacketType::Keepalive: {
    break;
  }
  case PacketType::DataBestEffort: {
    if (!rx_be_.Accept(pkt.seq)) {
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
    const bool fresh = rx_rel_.Accept(pkt.seq);
    // Always ACK so sender can stop rtx even on dup.
    (void)SendPacket(PacketType::Ack, pkt.seq, {}, now_ms);
    if (!fresh) {
      break;
    }
    if (on_message_) {
      Message m;
      m.assoc = id_;
      m.seq = pkt.seq;
      m.qos = QosClass::Reliable;
      m.payload = pkt.payload;
      on_message_(m);
    }
    break;
  }
  }
}

} // namespace pp::adp
