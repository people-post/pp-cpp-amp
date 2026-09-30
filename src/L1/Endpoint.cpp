#include "amp/L1/Endpoint.h"

#include "amp/L1/HmacBinder.h"
#include "amp/L1/WireCodec.h"

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

void Endpoint::Unregister(const AssocId& id) { conns_.erase(id); }

Roe<void> Endpoint::SendRaw(const IpEndpoint& peer, std::span<const uint8_t> datagram) {
  auto sent = io_->SendTo(peer, datagram);
  if (sent) {
    tx_datagrams_.fetch_add(1, std::memory_order_relaxed);
    tx_bytes_.fetch_add(datagram.size(), std::memory_order_relaxed);
  }
  return sent;
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
  for (size_t n = 0; n < budget; ++n) {
    auto got = io_->RecvFrom();
    if (!got) {
      break;
    }
    if (!*got) {
      break;
    }
    rx_datagrams_.fetch_add(1, std::memory_order_relaxed);
    rx_bytes_.fetch_add((*got)->second.size(), std::memory_order_relaxed);
    HandleDatagram((*got)->first, (*got)->second);
  }
}

void Endpoint::Tick() {
  const int64_t now = clock_->NowMs();
  std::vector<std::shared_ptr<Connection>> snap;
  snap.reserve(conns_.size());
  for (auto& [_, c] : conns_) {
    snap.push_back(c);
  }
  for (auto& c : snap) {
    c->Tick(now);
  }
}

void Endpoint::HandleDatagram(const IpEndpoint& from, std::span<const uint8_t> datagram) {
  if (datagram.size() < kHeaderBytes + kHmacBytes) {
    NoteRejected();
    return;
  }
  AssocId id{};
  std::memcpy(id.bytes.data(), datagram.data() + 2, kAssocIdBytes);

  if (auto conn = Find(id)) {
    conn->HandleDatagram(from, datagram, clock_->NowMs());
    return;
  }
  if (!accept_enabled_ || !accept_key_) {
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
  const int64_t delta =
      static_cast<int64_t>(static_cast<int32_t>(now_trunc - decoded->timestamp_ms));
  if (delta > kDefaultSkewMs || delta < -kDefaultSkewMs) {
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

} // namespace pp::adp
