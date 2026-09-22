#include "amp/link/MeshRuntime.h"

namespace pp::amp {

MeshRuntime::MeshRuntime(adp::Endpoint& endpoint, MshIdentity local_identity, std::string local_peer_id,
                         PeerLinkConfig config)
    : endpoint_(endpoint),
      links_(endpoint, std::move(local_identity), std::move(local_peer_id), std::move(config), io_mu_),
      pump_(endpoint, links_) {
  links_.SetCompletionPoster([this](std::function<void()> fn) { PostToIo(std::move(fn)); });
}

void MeshRuntime::Start() {
  std::lock_guard lock(io_mu_);
  started_ = true;
}

void MeshRuntime::Stop() {
  std::lock_guard lock(io_mu_);
  started_ = false;
  io_queue_.clear();
  io_ticks_.clear();
}

void MeshRuntime::PumpLocked() {
  if (pumping_) {
    pump_.Pump();
    return;
  }
  pumping_ = true;
  std::vector<IoTickId> ids;
  ids.reserve(io_ticks_.size());
  for (const auto& entry : io_ticks_) {
    ids.push_back(entry.id);
  }
  for (const IoTickId id : ids) {
    for (const auto& entry : io_ticks_) {
      if (entry.id == id && entry.tick) {
        entry.tick();
        break;
      }
    }
  }
  DrainPostedIoLocked();
  pump_.Pump();
  pumping_ = false;
}

void MeshRuntime::TickLocked() { pump_.Tick(); }

void MeshRuntime::DrainPostedIoLocked() {
  for (size_t budget = 0; budget < 32 && !io_queue_.empty(); ++budget) {
    auto task = std::move(io_queue_.front());
    io_queue_.pop_front();
    if (task) {
      task();
    }
  }
}

void MeshRuntime::Pump() {
  std::lock_guard lock(io_mu_);
  PumpLocked();
}

void MeshRuntime::Tick() {
  std::lock_guard lock(io_mu_);
  TickLocked();
  // Dial timeout / WhenChannelOpen completions are PostToIo'd from Tick; drain so a
  // single Pump→Tick cycle (harness AdvanceMs / Drive) observes waiter callbacks.
  DrainPostedIoLocked();
}

void MeshRuntime::Drive() {
  std::lock_guard lock(io_mu_);
  PumpLocked();
  TickLocked();
  DrainPostedIoLocked();
}

void MeshRuntime::PostToIo(IoTask task) {
  if (!task) {
    return;
  }
  std::lock_guard lock(io_mu_);
  io_queue_.push_back(std::move(task));
}

MeshRuntime::IoTickId MeshRuntime::AddIoTick(IoTask tick) {
  if (!tick) {
    return 0;
  }
  std::lock_guard lock(io_mu_);
  const IoTickId id = next_io_tick_id_++;
  if (next_io_tick_id_ == 0) {
    next_io_tick_id_ = 1;
  }
  io_ticks_.push_back(IoTickEntry{id, std::move(tick)});
  return id;
}

void MeshRuntime::RemoveIoTick(const IoTickId id) {
  if (id == 0) {
    return;
  }
  std::lock_guard lock(io_mu_);
  for (auto it = io_ticks_.begin(); it != io_ticks_.end(); ++it) {
    if (it->id == id) {
      io_ticks_.erase(it);
      return;
    }
  }
}

void MeshRuntime::EnsureAssociation(const DialKey& peer_key, PeerLinkManager::LinkCb on_complete) {
  links_.EnsureAssociation(peer_key, std::move(on_complete));
}

void MeshRuntime::OpenChannel(const DialKey& peer_key, const std::string& protocol_id, ChannelPolicy policy,
                              PeerLinkManager::ChannelCb on_complete) {
  links_.OpenChannel(peer_key, protocol_id, std::move(policy), std::move(on_complete));
}

void MeshRuntime::WhenChannelOpen(const DialKey& peer_key, uint32_t channel_id, int64_t deadline_ms_abs,
                                  std::function<void(bool ok)> done) {
  links_.WhenChannelOpen(peer_key, channel_id, deadline_ms_abs, std::move(done));
}

void MeshRuntime::WhenChannelOpenIn(const DialKey& peer_key, uint32_t channel_id,
                                    std::chrono::milliseconds remaining,
                                    std::function<void(bool ok)> done) {
  links_.WhenChannelOpenIn(peer_key, channel_id, remaining, std::move(done));
}

std::shared_ptr<ChannelSession> MeshRuntime::BindChannel(const DialKey& peer_key, uint32_t channel_id,
                                                         ChannelPolicy policy,
                                                         ChannelSession::FrameHandler on_frame,
                                                         ChannelSession::ClosedCallback on_closed) {
  return links_.BindChannel(peer_key, channel_id, std::move(policy), std::move(on_frame),
                            std::move(on_closed));
}

LinkSnapshotEx MeshRuntime::SnapshotByDialKey(const DialKey& key) const {
  return links_.GetSnapshotByDialKey(key);
}

LinkSnapshotEx MeshRuntime::SnapshotByPeerId(const std::string& peer_id, TransportClass prefer) const {
  return links_.GetSnapshotByPeerId(peer_id, prefer);
}

PeerLinkSnapshot MeshRuntime::GetLinkSnapshot(const DialKey& peer_key) const {
  return links_.GetLinkSnapshot(peer_key);
}

bool MeshRuntime::IsConnected(const DialKey& peer_key) const {
  return links_.IsConnected(peer_key);
}

bool MeshRuntime::IsConnectedToPeerId(const std::string& peer_id) const {
  return links_.IsConnectedToPeerId(peer_id);
}

bool MeshRuntime::IsReachable(const std::string& peer_id) const {
  return links_.IsReachable(peer_id);
}

Roe<void> MeshRuntime::RegisterEndpoint(const DialKey& peer_key, const std::string& multiaddr) {
  return links_.RegisterEndpoint(peer_key, multiaddr);
}

Roe<void> MeshRuntime::RegisterEndpoints(const DialKey& peer_key,
                                         const std::vector<std::string>& multiaddrs) {
  return links_.RegisterEndpoints(peer_key, multiaddrs);
}

std::optional<std::string> MeshRuntime::PreferredMultiaddr(const std::string& peer_id) const {
  return links_.PreferredMultiaddr(peer_id);
}

void MeshRuntime::SetProtocolHandler(const std::string& protocol_id, PeerLinkManager::ProtocolHandler handler) {
  links_.SetProtocolHandler(protocol_id, std::move(handler));
}

void MeshRuntime::RemoveProtocolHandler(const std::string& protocol_id) {
  links_.RemoveProtocolHandler(protocol_id);
}

void MeshRuntime::SetCapabilityHandler(PeerLinkManager::CapabilityHandler handler) {
  links_.SetCapabilityHandler(std::move(handler));
}

void MeshRuntime::SetLocalListenMultiaddrs(std::vector<std::string> multiaddrs) {
  links_.SetLocalListenMultiaddrs(std::move(multiaddrs));
}

void MeshRuntime::SetAdvertisedProtocols(std::vector<std::string> protocols) {
  links_.SetAdvertisedProtocols(std::move(protocols));
}

void MeshRuntime::MarkWarm(const DialKey& peer_key) { links_.MarkWarm(peer_key); }

void MeshRuntime::MarkHot(const DialKey& peer_key) { links_.MarkHot(peer_key); }

void MeshRuntime::ClearWarm(const DialKey& peer_key) { links_.ClearWarm(peer_key); }

void MeshRuntime::ClearDialBackoff(const DialKey& peer_key) { links_.ClearDialBackoff(peer_key); }

void MeshRuntime::AbortInflightDial(const DialKey& peer_key) { links_.AbortInflightDial(peer_key); }

void MeshRuntime::EstablishNestedOverCarrier(const DialKey& peer_key, std::shared_ptr<ChannelSession> carrier,
                                             bool initiator, PeerLinkManager::LinkCb on_complete) {
  links_.EstablishNestedOverCarrier(peer_key, std::move(carrier), initiator, std::move(on_complete));
}

void MeshRuntime::EnableNestedCarrierAccept(bool enable, std::string protocol_id) {
  links_.EnableNestedCarrierAccept(enable, std::move(protocol_id));
}

} // namespace pp::amp
