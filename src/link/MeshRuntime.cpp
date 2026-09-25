#include "amp/link/MeshRuntime.h"

#include "amp/link/AdpMultiaddr.h"

#include <atomic>
#include <utility>

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
  deferred_queue_.clear();
  timers_.clear();
  io_ticks_.clear();
}

bool MeshRuntime::BeginDriveLocked() {
  if (driving_) {
    // Nested Pump/Tick/Drive is forbidden — callers must Post/PostDeferred instead.
    return false;
  }
  driving_ = true;
  return true;
}

void MeshRuntime::EndDriveLocked() { driving_ = false; }

void MeshRuntime::PumpLocked() {
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

void MeshRuntime::DrainDeferredLocked() {
  // Drain all deferred posted this turn (teardown must not wait on a budget).
  while (!deferred_queue_.empty()) {
    auto batch = std::move(deferred_queue_);
    deferred_queue_.clear();
    for (auto& task : batch) {
      if (task) {
        task();
      }
    }
  }
}

void MeshRuntime::FireTimersLocked() {
  const int64_t now = endpoint_.GetClock().NowMs();
  std::vector<IoTask> due;
  for (auto it = timers_.begin(); it != timers_.end();) {
    if (it->deadline_ms <= now) {
      if (it->task) {
        due.push_back(std::move(it->task));
      }
      it = timers_.erase(it);
    } else {
      ++it;
    }
  }
  for (auto& task : due) {
    if (task) {
      task();
    }
  }
}

MeshRuntime::TimerId MeshRuntime::ArmTimerLocked(int64_t deadline_ms_abs, IoTask task) {
  if (!task) {
    return 0;
  }
  const TimerId id = next_timer_id_++;
  if (next_timer_id_ == 0) {
    next_timer_id_ = 1;
  }
  timers_.push_back(TimerEntry{id, deadline_ms_abs, std::move(task)});
  return id;
}

void MeshRuntime::Pump() {
  std::lock_guard lock(io_mu_);
  if (!BeginDriveLocked()) {
    return;
  }
  PumpLocked();
  DrainDeferredLocked();
  EndDriveLocked();
}

void MeshRuntime::Tick() {
  std::lock_guard lock(io_mu_);
  if (!BeginDriveLocked()) {
    return;
  }
  TickLocked();
  // Dial timeout / WhenChannelOpen completions are PostToIo'd from Tick; drain so a
  // single Pump→Tick cycle (harness AdvanceMs / Drive) observes waiter callbacks.
  DrainPostedIoLocked();
  DrainDeferredLocked();
  FireTimersLocked();
  DrainDeferredLocked();
  EndDriveLocked();
}

void MeshRuntime::Drive() {
  std::lock_guard lock(io_mu_);
  if (!BeginDriveLocked()) {
    return;
  }
  // Turn order: work ticks → UDP pump → mux Tick → work drain → deferred → timers → deferred.
  PumpLocked();
  TickLocked();
  DrainPostedIoLocked();
  DrainDeferredLocked();
  FireTimersLocked();
  DrainDeferredLocked();
  EndDriveLocked();
}

void MeshRuntime::PostToIo(IoTask task) {
  if (!task) {
    return;
  }
  std::lock_guard lock(io_mu_);
  io_queue_.push_back(std::move(task));
}

void MeshRuntime::PostDeferred(IoTask task) {
  if (!task) {
    return;
  }
  std::lock_guard lock(io_mu_);
  deferred_queue_.push_back(std::move(task));
}

MeshRuntime::TimerId MeshRuntime::PostAfter(std::chrono::milliseconds delay, IoTask task) {
  if (!task) {
    return 0;
  }
  std::lock_guard lock(io_mu_);
  const int64_t now = endpoint_.GetClock().NowMs();
  const int64_t delay_ms = delay.count() < 0 ? 0 : delay.count();
  return ArmTimerLocked(now + delay_ms, std::move(task));
}

MeshRuntime::TimerId MeshRuntime::PostAt(int64_t deadline_ms_abs, IoTask task) {
  if (!task) {
    return 0;
  }
  std::lock_guard lock(io_mu_);
  return ArmTimerLocked(deadline_ms_abs, std::move(task));
}

void MeshRuntime::CancelTimer(TimerId id) {
  if (id == 0) {
    return;
  }
  std::lock_guard lock(io_mu_);
  for (auto it = timers_.begin(); it != timers_.end(); ++it) {
    if (it->id == id) {
      timers_.erase(it);
      return;
    }
  }
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

LinkEventListenerId MeshRuntime::AddLinkEventListener(LinkEventListener listener) {
  return links_.AddLinkEventListener(std::move(listener));
}

void MeshRuntime::RemoveLinkEventListener(const LinkEventListenerId id) {
  links_.RemoveLinkEventListener(id);
}

size_t MeshRuntime::RequestDropLink(const DialKey& peer_key) {
  std::lock_guard lock(io_mu_);
  return links_.RequestDropLink(peer_key);
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

void MeshRuntime::BurstDial(const std::vector<std::string>& multiaddrs, std::chrono::milliseconds window,
                            BurstDialCb on_done) {
  if (!on_done) {
    return;
  }

  constexpr size_t kMaxAddrs = 8;
  std::vector<std::string> addrs;
  addrs.reserve(std::min(multiaddrs.size(), kMaxAddrs));
  for (const std::string& ma : multiaddrs) {
    if (addrs.size() >= kMaxAddrs) {
      break;
    }
    if (ma.empty() || !ParseAdpMultiaddr(ma)) {
      continue;
    }
    bool dup = false;
    for (const std::string& existing : addrs) {
      if (existing == ma) {
        dup = true;
        break;
      }
    }
    if (!dup) {
      addrs.push_back(ma);
    }
  }

  auto settle = [this](BurstDialResult result, BurstDialCb done,
                       std::shared_ptr<std::vector<std::string>> keys,
                       std::shared_ptr<std::vector<std::string>> peer_ids) {
    PostDeferred([this, result = std::move(result), done = std::move(done), keys = std::move(keys),
                  peer_ids = std::move(peer_ids)]() mutable {
      if (keys) {
        for (size_t i = 0; i < keys->size(); ++i) {
          const std::string& key = (*keys)[i];
          // After PeerId adopt the winner may no longer live under amp:burst:*.
          if (result.ok && peer_ids && i < peer_ids->size() && IsConnectedToPeerId((*peer_ids)[i])) {
            continue;
          }
          auto snap = SnapshotByDialKey(key);
          if (result.ok && snap.base.phase == PeerLinkPhase::Connected &&
              snap.transport == TransportClass::Adp) {
            continue;
          }
          AbortInflightDial(key);
        }
      }
      if (done) {
        done(std::move(result));
      }
    });
  };

  if (addrs.empty()) {
    BurstDialResult early;
    early.error = "no peer_addrs";
    settle(std::move(early), std::move(on_done), nullptr, nullptr);
    return;
  }

  const int window_ms = window.count() > 0 ? static_cast<int>(window.count()) : 2000;
  struct State {
    std::atomic<bool> settled{false};
    std::vector<std::string> keys;
    std::vector<std::string> peer_ids;
    std::vector<std::string> multiaddrs;
    std::string last_error;
    BurstDialCb on_done;
  };
  auto state = std::make_shared<State>();
  state->on_done = std::move(on_done);

  auto finish = [this, state, settle](BurstDialResult result) {
    if (state->settled.exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    auto keys = std::make_shared<std::vector<std::string>>(state->keys);
    auto peer_ids = std::make_shared<std::vector<std::string>>(state->peer_ids);
    settle(std::move(result), std::move(state->on_done), std::move(keys), std::move(peer_ids));
  };

  for (size_t i = 0; i < addrs.size(); ++i) {
    const std::string& ma = addrs[i];
    auto parsed = ParseAdpMultiaddr(ma);
    if (!parsed) {
      state->last_error = "peer addr is not an ADP multiaddr";
      continue;
    }
    const std::string peer_id = parsed->peer_id;
    if (IsConnectedToPeerId(peer_id)) {
      BurstDialResult ok;
      ok.ok = true;
      ok.dialed = ma;
      finish(std::move(ok));
      return;
    }
    const std::string key = std::string(kBurstDialKeyPrefix) + std::to_string(i) + ":" +
                            peer_id.substr(0, std::min<size_t>(peer_id.size(), 12));
    if (auto registered = RegisterEndpoint(key, ma); !registered) {
      if (IsConnectedToPeerId(peer_id)) {
        BurstDialResult ok;
        ok.ok = true;
        ok.dialed = ma;
        finish(std::move(ok));
        return;
      }
      state->last_error = registered.error().message;
      continue;
    }
    state->keys.push_back(key);
    state->peer_ids.push_back(peer_id);
    state->multiaddrs.push_back(ma);
    EnsureAssociation(key, [state](PeerLinkManager::LinkRoe result) {
      if (state->settled.load(std::memory_order_acquire)) {
        return;
      }
      if (!result) {
        state->last_error = result.error().message;
      }
    });
  }

  if (state->keys.empty()) {
    BurstDialResult fail;
    fail.error = state->last_error.empty() ? "no peer_addrs" : state->last_error;
    finish(std::move(fail));
    return;
  }

  PostAfter(std::chrono::milliseconds(window_ms), [finish, state]() {
    if (state->settled.load(std::memory_order_acquire)) {
      return;
    }
    BurstDialResult fail;
    fail.error = state->last_error.empty() ? "punch burst window expired" : state->last_error;
    if (!state->multiaddrs.empty()) {
      fail.dialed = state->multiaddrs.back();
    }
    finish(std::move(fail));
  });

  auto poll = std::make_shared<std::function<void()>>();
  *poll = [this, state, finish, poll]() {
    if (state->settled.load(std::memory_order_acquire)) {
      return;
    }
    for (size_t i = 0; i < state->peer_ids.size(); ++i) {
      if (IsConnectedToPeerId(state->peer_ids[i])) {
        BurstDialResult ok;
        ok.ok = true;
        ok.dialed = state->multiaddrs[i];
        finish(std::move(ok));
        return;
      }
    }
    if (state->settled.load(std::memory_order_acquire)) {
      return;
    }
    PostToIo([poll, state]() {
      if (!state->settled.load(std::memory_order_acquire)) {
        (*poll)();
      }
    });
  };
  PostToIo([poll]() { (*poll)(); });
}

void MeshRuntime::EstablishNestedOverCarrier(const DialKey& peer_key, std::shared_ptr<ChannelSession> carrier,
                                             bool initiator, PeerLinkManager::LinkCb on_complete) {
  links_.EstablishNestedOverCarrier(peer_key, std::move(carrier), initiator, std::move(on_complete));
}

void MeshRuntime::EnableNestedCarrierAccept(bool enable, std::string protocol_id) {
  links_.EnableNestedCarrierAccept(enable, std::move(protocol_id));
}

} // namespace pp::amp
