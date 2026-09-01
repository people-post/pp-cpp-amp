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
  return io_->SendTo(peer, datagram);
}

void Endpoint::Pump() {
  for (;;) {
    auto got = io_->RecvFrom();
    if (!got) {
      break;
    }
    if (!*got) {
      break;
    }
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
    return;
  }
  AssocId id{};
  std::memcpy(id.bytes.data(), datagram.data() + 2, kAssocIdBytes);

  if (auto conn = Find(id)) {
    conn->HandleDatagram(from, datagram, clock_->NowMs());
    return;
  }
  if (!accept_enabled_ || !accept_key_) {
    return;
  }
  HmacBinder binder(*accept_key_);
  if (!binder.Verify(datagram)) {
    return;
  }
  auto decoded = WireCodec::Decode(datagram);
  if (!decoded) {
    return;
  }
  const bool is_new = Find(id) == nullptr;
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
