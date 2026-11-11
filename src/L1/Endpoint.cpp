#include "amp/L1/Endpoint.h"

#include "amp/L1/HmacBinder.h"
#include "amp/L1/PacketProtection.h"
#include "amp/L1/WireCodec.h"

#include <algorithm>
#include <cstring>

namespace pp::adp {

Endpoint::Endpoint(std::shared_ptr<DatagramIo> io, std::shared_ptr<Clock> clock)
    : io_(std::move(io)), clock_(std::move(clock)) {}

Roe<std::shared_ptr<Connection>> Endpoint::Open(OpenParams params) {
  auto opened = Connection::Open(*this, std::move(params));
  if (!opened) {
    return Error(opened.error().message);
  }
  auto conn = *opened;
  if (conns_.count(conn->Id()) != 0) {
    return Error("adp: assoc already open");
  }
  conns_.emplace(conn->Id(), conn);
  return conn;
}

Roe<std::shared_ptr<Connection>> Endpoint::AcceptOrCreate(const AssocId& id, const PeerKey& key,
                                                          const IpEndpoint& peer) {
  if (auto it = conns_.find(id); it != conns_.end()) {
    return it->second;
  }
  OpenParams p;
  p.ApplyTuning(tuning_);
  p.key = key;
  p.id = id;
  p.mint_id = false;
  p.peer = peer;
  p.reduce_rtx_until_authenticated = true;
  return Open(std::move(p));
}

std::shared_ptr<Connection> Endpoint::Find(const AssocId& id) const {
  auto it = conns_.find(id);
  if (it == conns_.end()) {
    return nullptr;
  }
  return it->second;
}

void Endpoint::Unregister(const AssocId& id) {
  conns_.erase(id);
}

Roe<void> Endpoint::SendRaw(const IpEndpoint& peer, std::span<const uint8_t> datagram, const AssocId* assoc) {
  if (corked_ && assoc) {
    CorkedSend send;
    send.peer = peer;
    send.assoc = *assoc;
    send.offset = cork_bytes_.size();
    send.size = datagram.size();
    cork_bytes_.insert(cork_bytes_.end(), datagram.begin(), datagram.end());
    corked_sends_.push_back(send);
    tx_datagrams_.fetch_add(1, std::memory_order_relaxed);
    tx_bytes_.fetch_add(datagram.size(), std::memory_order_relaxed);
    if (corked_sends_.size() >= kMaxCorkedSends) {
      FlushSends();
      corked_ = true;
    }
    return {};
  }
  auto sent = io_->SendTo(peer, datagram);
  if (sent) {
    tx_datagrams_.fetch_add(1, std::memory_order_relaxed);
    tx_bytes_.fetch_add(datagram.size(), std::memory_order_relaxed);
  }
  return sent;
}

namespace {

bool SamePeer(const IpEndpoint& a, const IpEndpoint& b) {
  return a.family == b.family && a.port == b.port && a.addr == b.addr;
}

bool PeerBefore(const IpEndpoint& a, const IpEndpoint& b) {
  if (a.family != b.family) {
    return a.family < b.family;
  }
  if (a.port != b.port) {
    return a.port < b.port;
  }
  return a.addr < b.addr;
}

/** UDP's largest payload, IPv4 (the smaller): a GSO run's bytes stay under it. */
constexpr size_t kMaxRunBytes = 65507;

} // namespace

void Endpoint::FlushSends() {
  corked_ = false;
  if (corked_sends_.empty()) {
    return;
  }
  // Grouped by peer; within a peer, in the order sent (a stable sort).
  std::vector<size_t> order(corked_sends_.size());
  for (size_t i = 0; i < order.size(); ++i) {
    order[i] = i;
  }
  std::stable_sort(order.begin(), order.end(), [this](const size_t a, const size_t b) {
    return PeerBefore(corked_sends_[a].peer, corked_sends_[b].peer);
  });
  size_t first = 0;
  while (first < order.size()) {
    size_t last = first + 1;
    while (last < order.size() && SamePeer(corked_sends_[order[last]].peer, corked_sends_[order[first]].peer)) {
      ++last;
    }
    SendRuns(order, first, last);
    first = last;
  }
  corked_sends_.clear();
  cork_bytes_.clear();
}

void Endpoint::SendRuns(const std::vector<size_t>& order, const size_t first, const size_t last) {
  std::vector<std::span<const uint8_t>> parts;
  size_t at = first;
  while (at < last) {
    // A run: datagrams of one size, the last of it may be shorter; at most kMaxSegments and kMaxRunBytes.
    const size_t segment = corked_sends_[order[at]].size;
    parts.clear();
    size_t bytes = 0;
    size_t end = at;
    while (end < last && parts.size() < DatagramIo::kMaxSegments) {
      const CorkedSend& send = corked_sends_[order[end]];
      if (send.size > segment || bytes + send.size > kMaxRunBytes) {
        break;
      }
      parts.emplace_back(cork_bytes_.data() + send.offset, send.size);
      bytes += send.size;
      ++end;
      if (send.size < segment) {
        break; // a shorter one ends the run
      }
    }
    const IpEndpoint& peer = corked_sends_[order[at]].peer;
    auto sent = parts.size() == 1 ? io_->SendTo(peer, parts.front()) : io_->SendSegments(peer, parts, segment);
    if (!sent && IsUnreachableSendError(sent.error())) {
      for (size_t i = at; i < end; ++i) {
        if (const auto& assoc = corked_sends_[order[i]].assoc) {
          if (auto conn = Find(*assoc)) {
            conn->MarkPeerUnreachable();
          }
        }
      }
    }
    at = end;
  }
}

EndpointStats Endpoint::Stats() const {
  EndpointStats s;
  s.tx_datagrams = tx_datagrams_.load(std::memory_order_relaxed);
  s.tx_bytes = tx_bytes_.load(std::memory_order_relaxed);
  s.rx_datagrams = rx_datagrams_.load(std::memory_order_relaxed);
  s.rx_bytes = rx_bytes_.load(std::memory_order_relaxed);
  s.rx_rejected = rx_rejected_.load(std::memory_order_relaxed);
  s.reliable_sent = reliable_sent_.load(std::memory_order_relaxed);
  s.retransmits = retransmits_.load(std::memory_order_relaxed);
  s.reliable_lost = reliable_lost_.load(std::memory_order_relaxed);
  s.rtt_samples = rtt_samples_.load(std::memory_order_relaxed);
  s.rtt_sum_ms = rtt_sum_ms_.load(std::memory_order_relaxed);
  return s;
}

void Endpoint::NoteRttSample(const int64_t rtt_ms) {
  const int64_t sample = rtt_ms < 0 ? 0 : rtt_ms;
  rtt_samples_.fetch_add(1, std::memory_order_relaxed);
  rtt_sum_ms_.fetch_add(static_cast<uint64_t>(sample), std::memory_order_relaxed);
  if (rtt_observer_) {
    rtt_observer_(sample);
  }
}

void Endpoint::Pump(const size_t budget) {
  // In batches where the IO can (recvmmsg), each datagram handled in the IO's buffer.
  (void)io_->RecvBatch(budget, [this](const IpEndpoint& from, const std::span<const uint8_t> datagram) {
    rx_datagrams_.fetch_add(1, std::memory_order_relaxed);
    rx_bytes_.fetch_add(datagram.size(), std::memory_order_relaxed);
    HandleDatagram(from, datagram);
  });
}

void Endpoint::Tick() {
  if (resend_pending_.empty()) {
    return;
  }
  const int64_t now = clock_->NowMs();
  std::vector<std::shared_ptr<Connection>> snap;
  snap.reserve(resend_pending_.size());
  for (auto it = resend_pending_.begin(); it != resend_pending_.end();) {
    auto conn = conns_.find(*it);
    if (conn == conns_.end() || !conn->second) {
      it = resend_pending_.erase(it); // gone
      continue;
    }
    snap.push_back(conn->second);
    ++it;
  }
  for (auto& c : snap) {
    c->Tick(now);
    if (!c->HasOutstanding()) {
      resend_pending_.erase(c->Id());
    }
  }
}

void Endpoint::HandleDatagram(const IpEndpoint& from, std::span<const uint8_t> datagram) {
  if (!datagram.empty() && (datagram[0] >> 4) != kWireVersion) {
    HandleForeignVersion(from, datagram);
    return;
  }
  if (datagram.size() < kSealedHeaderBytes + kPacketTagBytes || datagram.size() > kMaxDatagramBytes) {
    NoteRejected();
    return;
  }
  const AssocId id = WireCodec::PeekAssoc(datagram);

  if (auto conn = Find(id)) {
    conn->HandleDatagram(from, datagram, clock_->NowMs());
    return;
  }
  // A new association starts only from a plain (HMAC'd) handshake packet: a sealed one names keys nobody here has.
  auto type = WireCodec::PeekType(datagram);
  if (!accept_enabled_ || !accept_key_ || !type || IsSealed(*type)) {
    NoteRejected();
    return;
  }
  HmacBinder binder(*accept_key_);
  if (!binder.Verify(datagram)) {
    NoteRejected();
    return;
  }
  auto decoded = WireCodec::Decode(datagram);
  if (!decoded) {
    NoteRejected();
    return;
  }
  const bool is_new = Find(id) == nullptr;
  if (is_new && conns_.size() >= max_accepted_conns_) {
    return;
  }
  // Skew check before creating an association.
  const int64_t now = clock_->NowMs();
  const uint32_t now_trunc = static_cast<uint32_t>(static_cast<uint64_t>(now) & 0xffffffffull);
  const int64_t delta = static_cast<int64_t>(static_cast<int32_t>(now_trunc - decoded->timestamp_ms));
  if (delta > tuning_.skew_ms || delta < -tuning_.skew_ms) {
    return;
  }
  auto accepted = AcceptOrCreate(id, *accept_key_, from);
  if (!accepted) {
    return;
  }
  if (is_new && accept_handler_) {
    accept_handler_(*accepted);
  }
  (*accepted)->HandleAuthenticated(*decoded, from, now);
}

void Endpoint::HandleForeignVersion(const IpEndpoint& from, std::span<const uint8_t> datagram) {
  NoteRejected();
  if ((datagram[0] >> 4) == kVersionNegotiationNibble) {
    auto negotiation = WireCodec::DecodeVersionNegotiation(datagram);
    if (!negotiation) {
      return;
    }
    // A reply that lists our version is not about our version (garbled, or forged): ignore it.
    if (std::find(negotiation->versions.begin(), negotiation->versions.end(), kWireVersion) !=
        negotiation->versions.end()) {
      return;
    }
    // Our own dial's packets begin with our layout: the echo names the association, as PeekAssoc reads it.
    if (auto conn = Find(WireCodec::PeekAssoc(negotiation->echo))) {
      conn->HandleVersionNegotiation(negotiation->versions);
    }
    return;
  }
  // Another version's packet: tell its sender which one we speak, if we take dials at all.
  if (accept_enabled_) {
    ReplyVersionNegotiation(from, datagram);
  }
}

void Endpoint::ReplyVersionNegotiation(const IpEndpoint& from, std::span<const uint8_t> datagram) {
  static constexpr uint8_t kSpoken[] = {kWireVersion};
  const auto reply = WireCodec::EncodeVersionNegotiation(datagram, kSpoken);
  if (reply.size() > datagram.size()) {
    return; // never amplify: a reply is no larger than what it answers
  }
  const int64_t now = clock_->NowMs();
  if (now - negotiation_window_ms_ >= 1000) {
    negotiation_window_ms_ = now;
    negotiation_sent_in_window_ = 0;
  }
  if (negotiation_sent_in_window_ >= kVersionNegotiationPerSecond) {
    return;
  }
  ++negotiation_sent_in_window_;
  (void)SendRaw(from, reply);
}

} // namespace pp::adp
