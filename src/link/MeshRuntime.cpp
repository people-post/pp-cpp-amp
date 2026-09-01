#include "amp/link/MeshRuntime.h"

namespace pp::amp {

MeshRuntime::MeshRuntime(adp::Endpoint& endpoint, MshIdentity local_identity, std::string local_peer_id,
                         PeerLinkConfig config)
    : endpoint_(endpoint), links_(endpoint, std::move(local_identity), std::move(local_peer_id), std::move(config)),
      pump_(endpoint, links_) {}

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
    // Nested wait loops (OpenChannel callbacks in AmpDirectChat / dial-back / history)
    // need ADP I/O progress without re-entering io ticks or the PostToIo drain.
    pump_.Pump();
    return;
  }
  pumping_ = true;
  // Copy ids so a tick may RemoveIoTick without invalidating iteration.
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
  for (size_t budget = 0; budget < 32 && !io_queue_.empty(); ++budget) {
    auto task = std::move(io_queue_.front());
    io_queue_.pop_front();
    if (task) {
      task();
    }
  }
  pump_.Pump();
  pumping_ = false;
}

void MeshRuntime::TickLocked() { pump_.Tick(); }

void MeshRuntime::Pump() {
  std::lock_guard lock(io_mu_);
  PumpLocked();
}

void MeshRuntime::Tick() {
  std::lock_guard lock(io_mu_);
  TickLocked();
}

void MeshRuntime::Drive() {
  std::lock_guard lock(io_mu_);
  PumpLocked();
  TickLocked();
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

} // namespace pp::amp
