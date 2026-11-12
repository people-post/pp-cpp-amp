#include "amp/link/MeshRuntime.h"

#include "amp/link/AdpMultiaddr.h"

#include <algorithm>
#include <atomic>
#include <utility>
#include <vector>

namespace pp::amp {
namespace {

/** The runtimes whose io lock this thread holds through IoExecutor::RunAndWait, innermost last. */
std::vector<const MeshRuntime*>& RunningAndWaiting() {
  thread_local std::vector<const MeshRuntime*> runtimes;
  return runtimes;
}

/** A MeshRuntime's io lane as a SerialExecutor (MeshRuntime::IoExecutor). */
class IoLaneExecutor final : public SerialExecutor {
public:
  /** `with_runtime`: runs its argument on the runtime, or not at all once the runtime is gone. */
  using WithRuntime = std::function<void(const std::function<void(MeshRuntime&)>&)>;

  explicit IoLaneExecutor(WithRuntime with_runtime) : with_runtime_(std::move(with_runtime)) {}

  void Post(Task task) override {
    with_runtime_([&task](MeshRuntime& runtime) { runtime.PostToIo(std::move(task)); });
  }

  bool RunsTasksOnCurrentThread() const override {
    bool on = false;
    with_runtime_([&on](MeshRuntime& runtime) {
      const auto& running = RunningAndWaiting();
      on = runtime.OnDriverThread() || std::find(running.begin(), running.end(), &runtime) != running.end();
    });
    return on;
  }

  TimerId PostAfter(std::chrono::milliseconds delay, Task task) override {
    TimerId id = 0;
    with_runtime_([&](MeshRuntime& runtime) { id = runtime.PostAfter(delay, std::move(task)); });
    return id;
  }

  void CancelTimer(TimerId id) override {
    with_runtime_([id](MeshRuntime& runtime) { runtime.CancelTimer(id); });
  }

  void RunAndWait(const Task& task) override {
    if (!task) {
      return;
    }
    bool ran = false;
    with_runtime_([&](MeshRuntime& runtime) {
      runtime.WithIoLock([&]() {
        auto& running = RunningAndWaiting();
        running.push_back(&runtime);
        struct Leave {
          std::vector<const MeshRuntime*>& running;
          ~Leave() { running.pop_back(); }
        } leave{running};
        ran = true;
        task();
      });
    });
    if (!ran) {
      task(); // the runtime is gone: nothing runs on its lane any more
    }
  }

private:
  WithRuntime with_runtime_;
};

} // namespace

MeshRuntime::MeshRuntime(adp::Endpoint& endpoint, MshIdentity local_identity, std::string local_peer_id,
                         PeerLinkConfig config)
    : endpoint_(endpoint),
      links_(endpoint, std::move(local_identity), std::move(local_peer_id), std::move(config), io_mu_),
      pump_(endpoint, links_),
      resolver_([this](std::function<void()> fn) { PostToIo(std::move(fn)); }) {
  links_.SetCompletionPoster([this](std::function<void()> fn) { PostToIo(std::move(fn)); });
  links_.SetHostResolver([this](const std::string& host, HostFamily family, uint16_t port,
                                std::function<void(Roe<adp::IpEndpoint>)> done) {
    resolver_.Resolve(host, family, port, std::move(done));
  });
  io_executor_ = std::make_shared<IoLaneExecutor>(
      io_lifetime_.Bind([this](const std::function<void(MeshRuntime&)>& fn) { fn(*this); }));
  io_mu_.SetReleaseHook(this, &MeshRuntime::OnLaneRelease);
}

MeshRuntime::~MeshRuntime() {
  io_lifetime_.InvalidateAndWait();
  std::lock_guard lock(io_mu_);
  io_mu_.SetReleaseHook(nullptr, nullptr); // the members it reads go before the lock does
  wake_ = nullptr;
}

void MeshRuntime::Start() {
  std::lock_guard lock(io_mu_);
  started_ = true;
}

void MeshRuntime::Stop() {
  std::lock_guard lock(io_mu_);
  // Tell every peer before going quiet: a silent stop left each one holding our link until its
  // alive timeout — enough short-lived clients filled a server's max_links and it refused new peers.
  // Completions / events the drops post are discarded below with the rest of the queue.
  links_.CloseAllLinks();
  started_ = false;
  claimed_driver_.store(std::thread::id{}, std::memory_order_release); // a restart claims afresh
  io_queue_.clear();
  deferred_queue_.clear();
  timers_.clear();
  io_ticks_.clear();
  timers_due_ms_ = kNotDue;
  last_sweep_ms_ = std::numeric_limits<int64_t>::min();
  sweep_due_ms_ = std::numeric_limits<int64_t>::min();
  pump_idle_until_ms_ = kNotWaiting;
  touched_ = false;
}

bool MeshRuntime::BeginDriveLocked() {
  if (driving_.load(std::memory_order_relaxed)) {
    // Nested Pump/Tick/Drive is forbidden — callers must Post/PostDeferred instead.
    return false;
  }
  const std::thread::id claimed = claimed_driver_.load(std::memory_order_acquire);
  if (claimed != std::thread::id{} && claimed != std::this_thread::get_id()) {
    refused_drives_.fetch_add(1, std::memory_order_relaxed); // a second driver
    return false;
  }
  driving_.store(true, std::memory_order_release);
  driving_thread_.store(std::this_thread::get_id(), std::memory_order_release);
  return true;
}

void MeshRuntime::EndDriveLocked() {
  driving_thread_.store(std::thread::id{}, std::memory_order_release);
  driving_.store(false, std::memory_order_release);
  ending_drive_ = true; // the release that follows is this drive's own (OnLaneReleaseLocked)
}

void MeshRuntime::ClaimDriver() {
  claimed_driver_.store(std::this_thread::get_id(), std::memory_order_release);
}

void MeshRuntime::ReleaseDriver() {
  claimed_driver_.store(std::thread::id{}, std::memory_order_release);
}

bool MeshRuntime::OnDriverThread() const {
  const std::thread::id self = std::this_thread::get_id();
  const std::thread::id claimed = claimed_driver_.load(std::memory_order_acquire);
  if (claimed != std::thread::id{}) {
    return claimed == self;
  }
  return driving_thread_.load(std::memory_order_acquire) == self;
}

void MeshRuntime::SetHousekeepingInterval(const std::chrono::milliseconds interval) {
  std::lock_guard lock(io_mu_);
  housekeeping_interval_ms_ = std::max<int64_t>(0, interval.count());
}

void MeshRuntime::SetWakeHandler(std::function<void()> wake) {
  std::lock_guard lock(io_mu_);
  wake_ = std::move(wake);
}

void MeshRuntime::OnLaneRelease(void* self) {
  static_cast<MeshRuntime*>(self)->OnLaneReleaseLocked();
}

void MeshRuntime::OnLaneReleaseLocked() {
  if (std::exchange(ending_drive_, false)) {
    return; // a Drive letting go: it planned its wake with everything it did
  }
  // Another thread (or the driver between turns): what it did is seen by the next turn. Wake an idle driver when that
  // turn is needed before the one it planned. While it waits longer than the housekeeping interval any change wakes
  // it, so a change only the walk acts on (a dial, a mark, a network change) is looked at within the interval — as
  // when the driver polled every few ms.
  touched_ = true;
  if (!wake_ || pump_idle_until_ms_ == kNotWaiting) {
    return;
  }
  const int64_t now = endpoint_.GetClock().NowMs();
  if (pump_idle_until_ms_ <= now) {
    return; // already due (or woken): the driver is on its way
  }
  if (pump_idle_until_ms_ - now <= housekeeping_interval_ms_ && FastDueMsLocked(now) >= pump_idle_until_ms_) {
    return;
  }
  pump_idle_until_ms_ = now; // one wake per wait
  wake_();
}

void MeshRuntime::PullHousekeepingLocked() {
  if (last_sweep_ms_ != std::numeric_limits<int64_t>::min()) {
    sweep_due_ms_ = std::min(sweep_due_ms_, last_sweep_ms_ + housekeeping_interval_ms_);
  }
  for (auto& entry : io_ticks_) {
    if (entry.last_ms != std::numeric_limits<int64_t>::min()) {
      entry.due_ms = std::min(entry.due_ms, entry.last_ms + housekeeping_interval_ms_);
    }
  }
}

int64_t MeshRuntime::FastDueMsLocked(const int64_t now) const {
  if (!io_queue_.empty() || !deferred_queue_.empty()) {
    return now;
  }
  return std::min({timers_due_ms_, endpoint_.NextResendDueMs(), links_.EveryTickDueMsLocked(now)});
}

int64_t MeshRuntime::NextDueMsLocked(const int64_t now) const {
  int64_t due = std::min(FastDueMsLocked(now), sweep_due_ms_);
  for (const auto& entry : io_ticks_) {
    due = std::min(due, entry.due_ms);
  }
  return due;
}

std::chrono::milliseconds MeshRuntime::DueIn(const int64_t due_ms, const int64_t now) {
  if (due_ms == kNotDue) {
    return DriveResult::kNothingDue;
  }
  return std::chrono::milliseconds(due_ms <= now ? 0 : due_ms - now);
}

std::chrono::milliseconds MeshRuntime::NextDueIn() const {
  std::lock_guard lock(io_mu_);
  const int64_t now = endpoint_.GetClock().NowMs();
  return DueIn(NextDueMsLocked(now), now);
}

size_t MeshRuntime::RunDueIoTicksLocked(const int64_t now) {
  std::vector<IoTickId> ids;
  for (const auto& entry : io_ticks_) {
    if (housekeeping_interval_ms_ == 0 || entry.due_ms <= now) {
      ids.push_back(entry.id);
    }
  }
  for (const IoTickId id : ids) {
    // A copy: the tick may remove itself (or another) from io_ticks_.
    IoTask tick;
    IoDueTick due_tick;
    for (const auto& entry : io_ticks_) {
      if (entry.id == id) {
        tick = entry.tick;
        due_tick = entry.due_tick;
        break;
      }
    }
    std::chrono::milliseconds again{housekeeping_interval_ms_};
    if (due_tick) {
      again = due_tick();
    } else if (tick) {
      tick();
    }
    for (auto& entry : io_ticks_) {
      if (entry.id == id) {
        entry.last_ms = now;
        // Never more often than the housekeeping interval: a busy node runs its ticks at that cadence, as before.
        entry.due_ms =
            again == kIoTickIdle ? kNotDue : now + std::max<int64_t>(again.count(), housekeeping_interval_ms_);
        break;
      }
    }
  }
  return ids.size();
}

size_t MeshRuntime::PumpLocked(size_t& ran) {
  // An io tick that ran is not activity by itself (it would keep itself at the cadence); what it posts is.
  (void)RunDueIoTicksLocked(endpoint_.GetClock().NowMs());
  ran += DrainPostedIoLocked();
  return pump_.Pump();
}

void MeshRuntime::TickLocked() {
  const int64_t now = endpoint_.GetClock().NowMs();
  const bool sweep = housekeeping_interval_ms_ == 0 || now >= sweep_due_ms_;
  pump_.Tick(sweep);
  if (sweep) {
    last_sweep_ms_ = now;
    // The earliest deadline the walk found, but not sooner than the interval: a busy node always has one due.
    const int64_t found = links_.SweepDueMs();
    sweep_due_ms_ = found == PeerLinkManager::kNotDue ? kNotDue : std::max(found, now + housekeeping_interval_ms_);
  }
}

size_t MeshRuntime::DrainPostedIoLocked() {
  size_t budget = 0;
  for (; budget < 32 && !io_queue_.empty(); ++budget) {
    auto task = std::move(io_queue_.front());
    io_queue_.pop_front();
    if (task) {
      task();
    }
  }
  return budget;
}

size_t MeshRuntime::DrainDeferredLocked() {
  // Drain all deferred posted this turn (teardown must not wait on a budget).
  size_t ran = 0;
  while (!deferred_queue_.empty()) {
    auto batch = std::move(deferred_queue_);
    deferred_queue_.clear();
    for (auto& task : batch) {
      if (task) {
        task();
      }
      ++ran;
    }
  }
  return ran;
}

size_t MeshRuntime::FireTimersLocked() {
  const int64_t now = endpoint_.GetClock().NowMs();
  if (now < timers_due_ms_) {
    return 0; // none due (the earliest is known)
  }
  std::vector<IoTask> due;
  int64_t next = kNotDue;
  for (auto it = timers_.begin(); it != timers_.end();) {
    if (it->deadline_ms <= now) {
      if (it->task) {
        due.push_back(std::move(it->task));
      }
      it = timers_.erase(it);
    } else {
      next = std::min(next, it->deadline_ms);
      ++it;
    }
  }
  timers_due_ms_ = next; // before the tasks: a timer they arm lowers it
  for (auto& task : due) {
    if (task) {
      task();
    }
  }
  return due.size();
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
  timers_due_ms_ = std::min(timers_due_ms_, deadline_ms_abs);
  return id;
}

void MeshRuntime::Pump() {
  std::lock_guard lock(io_mu_);
  if (!BeginDriveLocked()) {
    return;
  }
  touched_ = false;
  size_t ran = 0;
  const size_t received = PumpLocked(ran);
  ran += DrainDeferredLocked();
  if (received > 0 || ran > 0) {
    PullHousekeepingLocked();
  }
  EndDriveLocked();
}

void MeshRuntime::Tick() {
  std::lock_guard lock(io_mu_);
  if (!BeginDriveLocked()) {
    return;
  }
  touched_ = false;
  TickLocked();
  // Dial timeout / WhenChannelOpen completions are PostToIo'd from Tick; drain so a
  // single Pump→Tick cycle (harness AdvanceMs / Drive) observes waiter callbacks.
  size_t ran = DrainPostedIoLocked();
  ran += DrainDeferredLocked();
  ran += FireTimersLocked();
  ran += DrainDeferredLocked();
  if (ran > 0) {
    PullHousekeepingLocked();
  }
  EndDriveLocked();
}

DriveResult MeshRuntime::Drive() {
  std::lock_guard lock(io_mu_);
  if (!BeginDriveLocked()) {
    return DriveResult{0, false, std::chrono::milliseconds(std::max<int64_t>(housekeeping_interval_ms_, 1))};
  }
  // Turn order: work ticks → UDP pump → mux Tick → work drain → deferred → timers → deferred. What the round sends
  // goes out at its end, grouped by peer (a hop's fan-out of one frame to a member is one GSO send).
  const bool touched = std::exchange(touched_, false);
  endpoint_.CorkSends();
  size_t ran = 0;
  const size_t received = PumpLocked(ran);
  // Activity before the walk: it runs in this turn if its interval has passed (it sees what changed), else soon.
  const bool active = touched || ran > 0 || received > 0;
  if (active) {
    PullHousekeepingLocked();
  }
  TickLocked();
  size_t ran_after = DrainPostedIoLocked();
  ran_after += DrainDeferredLocked();
  ran_after += FireTimersLocked();
  ran_after += DrainDeferredLocked();
  if (ran_after > 0) {
    PullHousekeepingLocked(); // work the walk did not see: the next walk within the interval
  }
  if ((active || ran_after > 0) && housekeeping_interval_ms_ > 0) {
    // Ticks that the activity made due (their interval passed) run now rather than in a turn of their own.
    (void)RunDueIoTicksLocked(endpoint_.GetClock().NowMs());
  }
  endpoint_.FlushSends();
  const bool worked = touched || ran > 0 || ran_after > 0;
  const int64_t now = endpoint_.GetClock().NowMs();
  const int64_t due = NextDueMsLocked(now);
  pump_idle_until_ms_ = due == kNotDue ? std::numeric_limits<int64_t>::max() - 1 : due;
  EndDriveLocked();
  return DriveResult{received, worked, DueIn(due, now)};
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
  IoTickEntry entry;
  entry.id = id;
  entry.tick = std::move(tick);
  io_ticks_.push_back(std::move(entry));
  return id;
}

MeshRuntime::IoTickId MeshRuntime::AddIoDueTick(IoDueTick tick) {
  if (!tick) {
    return 0;
  }
  std::lock_guard lock(io_mu_);
  const IoTickId id = next_io_tick_id_++;
  if (next_io_tick_id_ == 0) {
    next_io_tick_id_ = 1;
  }
  IoTickEntry entry;
  entry.id = id;
  entry.due_tick = std::move(tick);
  io_ticks_.push_back(std::move(entry));
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

void MeshRuntime::WhenChannelOpenIn(const DialKey& peer_key, uint32_t channel_id, std::chrono::milliseconds remaining,
                                    std::function<void(bool ok)> done) {
  links_.WhenChannelOpenIn(peer_key, channel_id, remaining, std::move(done));
}

std::shared_ptr<ChannelSession> MeshRuntime::BindChannel(const DialKey& peer_key, uint32_t channel_id,
                                                         ChannelPolicy policy, ChannelSession::FrameHandler on_frame,
                                                         ChannelSession::ClosedCallback on_closed) {
  return links_.BindChannel(peer_key, channel_id, std::move(policy), std::move(on_frame), std::move(on_closed));
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

void MeshRuntime::UnregisterEndpoint(const DialKey& peer_key) {
  links_.UnregisterEndpoint(peer_key);
}

Roe<void> MeshRuntime::RegisterEndpoints(const DialKey& peer_key, const std::vector<std::string>& multiaddrs) {
  return links_.RegisterEndpoints(peer_key, multiaddrs);
}

std::optional<std::string> MeshRuntime::PreferredMultiaddr(const std::string& peer_id) const {
  return links_.PreferredMultiaddr(peer_id);
}

void MeshRuntime::SetProtocolHandler(const std::string& protocol_id, PeerLinkManager::ProtocolHandler handler) {
  links_.SetProtocolHandler(protocol_id, std::move(handler));
}

void MeshRuntime::SetRefuseUnhandledOpens(const bool refuse) {
  links_.SetRefuseUnhandledOpens(refuse);
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

void MeshRuntime::SetAdvertisedNodeInfo(std::string node_version, std::vector<std::string> features) {
  links_.SetAdvertisedNodeInfo(std::move(node_version), std::move(features));
}

std::optional<CapabilityPayload> MeshRuntime::RemoteCapability(const std::string& peer_id) const {
  return links_.RemoteCapability(peer_id);
}

void MeshRuntime::MarkWarm(const DialKey& peer_key) {
  links_.MarkWarm(peer_key);
}

void MeshRuntime::MarkHot(const DialKey& peer_key) {
  links_.MarkHot(peer_key);
}

void MeshRuntime::ClearWarm(const DialKey& peer_key) {
  links_.ClearWarm(peer_key);
}

void MeshRuntime::ClearDialBackoff(const DialKey& peer_key) {
  links_.ClearDialBackoff(peer_key);
}

void MeshRuntime::AbortInflightDial(const DialKey& peer_key) {
  links_.AbortInflightDial(peer_key);
}

void MeshRuntime::NotifyNetworkChanged() {
  PostToIo([this]() { (void)links_.OnNetworkChanged(); });
}

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

  auto settle = [this](BurstDialResult result, BurstDialCb done, std::shared_ptr<std::vector<std::string>> keys,
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
          if (result.ok && snap.base.phase == PeerLinkPhase::Connected && snap.transport == TransportClass::Adp) {
            continue;
          }
          AbortInflightDial(key);
        }
        // Burst keys are per job: without this the dial book kept one per punch target forever.
        for (const std::string& key : *keys) {
          UnregisterEndpoint(key);
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

  auto finish = [state, settle](BurstDialResult result) {
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

  // The poll reposts itself: the posted task owns it, and it reaches itself only weakly. A strong
  // self-capture was a cycle that leaked every burst's state and callbacks (LeakSanitizer).
  auto poll = std::make_shared<std::function<void()>>();
  *poll = [this, state, finish, weak_poll = std::weak_ptr<std::function<void()>>(poll)]() {
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
    auto self = weak_poll.lock();
    if (!self) {
      return;
    }
    PostToIo([self = std::move(self), state]() {
      if (!state->settled.load(std::memory_order_acquire)) {
        (*self)();
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
