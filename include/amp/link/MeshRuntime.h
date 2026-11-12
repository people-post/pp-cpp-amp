#pragma once

#include "amp/L1/Endpoint.h"
#include "amp/link/MeshPump.h"
#include "amp/link/HostResolver.h"
#include "amp/link/IoLaneMutex.h"
#include "amp/link/PeerLinkManager.h"
#include "amp/L2/Types.h"
#include "amp/L3/ChannelPolicy.h"
#include "amp/L3/ChannelSession.h"
#include "amp/link/LinkIdentity.h"
#include "common/Lifetime.h"
#include "common/SerialExecutor.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <limits>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace pp::amp {

/** Result of MeshRuntime::BurstDial (parallel ephemeral ADP dials). */
struct BurstDialResult {
  bool ok = false;
  std::string dialed;
  std::string error;
};

/**
 * Io-thread composer for Endpoint + PeerLinkManager + MeshPump.
 * Sole product entry for link ops (ADR_LINK_PLANE). Prefer these APIs over bare Links().
 *
 * Drive model (exclusive):
 * - Exactly one driver calls Pump/Tick/Drive (MeshPump thread, or the test harness
 *   acting as Amp). Nested Pump/Tick/Drive is refused. A driver thread that claims the
 *   runtime (ClaimDriver) is its only driver: a drive from any other thread is refused
 *   too (counted: RefusedDrives) — a second driver is a bug, not a race to serialize.
 * - PostToIo — work lane (SM steps, dial start, enqueue). Drained during Drive.
 * - PostDeferred — teardown lane (Abort, Close, DropLink, on_done). Runs after Tick
 *   and work drain so mux/session frames are off the stack.
 * - PostAfter — Amp-clock timers (sync windows, deadlines). Fired each Drive turn.
 *
 * Idle driver: each Drive says how soon the next turn has something to do (DriveResult::next_due — timers, resends,
 * the walks' deadlines, io ticks), and the driver waits until then or a datagram (Endpoint::WaitReadable). A change
 * another thread makes on the lane that is due sooner wakes it (SetWakeHandler): a post, an earlier timer, a
 * reliable send, anything at all while it waits longer than the housekeeping interval.
 */
/**
 * The most often MeshRuntime walks every link and runs an io tick — while the lane is active (datagrams, posted work,
 * timers, other threads' calls), and once after it; idle, a walk waits for the earliest deadline the last one found
 * and a tick for the time it asked for. Their deadlines are seconds long
 * (handshake and liveness timeouts, keepalives, FRAG sweeps, nested-carrier resends from 500 ms). What must be
 * prompt is not on it: resends of ADP reliable packets (Endpoint::Tick, every round, only connections with packets
 * in flight) and queued sends (flushed when an ack frees window). Walked every 5 ms, a node with 6k idle parked
 * clients spent ~85 % of its CPU walking (pp-browser STRESS_LAB, 2026-11-07).
 */
inline constexpr std::chrono::milliseconds kDefaultHousekeepingInterval{50};

/** One Drive turn: what it did and when the next one is due. */
struct DriveResult {
  /** Datagrams handled. */
  size_t received = 0;
  /**
   * It did more than read: ran posted or deferred work, fired a timer, or followed a change another thread made on
   * the lane. A wait that ended early followed by a turn that neither read nor worked was woken for nothing.
   */
  bool worked = false;
  /** How soon the next turn has something to do (0: at once; kNothingDue: nothing waits on time). */
  std::chrono::milliseconds next_due{0};
  static constexpr std::chrono::milliseconds kNothingDue = std::chrono::milliseconds::max();
};

class MeshRuntime {
public:
  using IoTask = std::function<void()>;
  /** An io tick that says how soon it wants to run again (kIoTickIdle: not before the lane is active again). */
  using IoDueTick = std::function<std::chrono::milliseconds()>;
  static constexpr std::chrono::milliseconds kIoTickIdle = std::chrono::milliseconds::max();
  using IoTickId = uint64_t;
  using TimerId = uint64_t;
  using BurstDialCb = std::function<void(BurstDialResult)>;

  MeshRuntime(adp::Endpoint& endpoint, MshIdentity local_identity, std::string local_peer_id,
              PeerLinkConfig config = {});
  /** Its IoExecutor's calls stop here: they are no-ops from now on (one running elsewhere is waited for). */
  ~MeshRuntime();
  MeshRuntime(const MeshRuntime&) = delete;
  MeshRuntime& operator=(const MeshRuntime&) = delete;

  adp::Endpoint& GetEndpoint() { return endpoint_; }
  const std::string& LocalPeerId() const { return links_.LocalPeerId(); }

  /**
   * Amp-internal / tests. Product L4 should use MeshRuntime helpers below.
   * Prefer not to grow new product call sites on Links().
   */
  PeerLinkManager& Links() { return links_; }
  const PeerLinkManager& Links() const { return links_; }

  void Start();
  void Stop();
  bool IsStarted() const { return started_; }

  /** True while Pump/Tick/Drive is on the stack (exclusive-driver guard). Any thread. */
  bool IsDriving() const { return driving_.load(std::memory_order_acquire); }

  /**
   * The calling thread becomes this runtime's only driver until ReleaseDriver or Stop: Pump,
   * Tick and Drive from any other thread return at once, doing nothing. The thread that owns
   * the drive loop calls it as the loop starts (a MeshPump thread, a pump of one's own).
   * Unclaimed, any thread may drive, one at a time (test harnesses driving by hand).
   */
  void ClaimDriver();
  void ReleaseDriver();
  /**
   * True on the claimed driver thread, or — unclaimed — on the thread inside Pump/Tick/Drive:
   * where io work and handlers run. Code that would wait for posted io work must not run there
   * (it would wait on itself). Any thread.
   */
  bool OnDriverThread() const;
  /** Drives refused because another thread claimed the runtime (a second driver). Any thread. */
  uint64_t RefusedDrives() const { return refused_drives_.load(std::memory_order_relaxed); }

  void Pump();
  void Tick();
  /**
   * One turn: io ticks that are due, the UDP drain, resends, the walk when due, posted work, deferred work, timers.
   * Returns what it did and when the next turn is due; a refused drive (nested, or not the claimed driver) does
   * nothing and says the housekeeping interval.
   */
  DriveResult Drive();
  /** How soon a Drive has something to do now (as Drive's next_due). Any thread. */
  std::chrono::milliseconds NextDueIn() const;
  /**
   * Any thread: how to end the driver's idle wait (MeshPumpThread::Wake). Called with the io lock held, by a thread
   * other than the one driving, when its change on the lane is due before the wake the last Drive planned
   * (next_due) — work posted, a timer sooner, a resend sooner — or when the driver waits longer than the
   * housekeeping interval (whatever it changed is then looked at within that interval, as before). Must not block or
   * take the io lock again. Unset: no wake (a driver of its own polls).
   */
  void SetWakeHandler(std::function<void()> wake);

  /** Work lane — drained during Drive (after io ticks / before or with pump). */
  void PostToIo(IoTask task);

  /**
   * This runtime's io lane as a SerialExecutor, for code that runs on it without driving it (a
   * library's control logic, a guest's servers) and for tests to swap for a manual one:
   * - Post = PostToIo; PostAfter / CancelTimer = Amp-clock timers (PostAfter / CancelTimer).
   * - RunsTasksOnCurrentThread: on the driver thread (OnDriverThread), or inside RunAndWait.
   * - RunAndWait runs the task on the caller under the io lock (WithIoLock): serialized with
   *   everything on the lane like a posted task, with no wait for a drive turn — so a thread that
   *   drives by hand cannot wait on itself. The task must not destroy this runtime.
   * Any thread. It may outlive the runtime: then posts and timers are dropped and RunAndWait runs
   * the task on the caller.
   */
  std::shared_ptr<SerialExecutor> IoExecutor() const { return io_executor_; }

  /**
   * Teardown lane — AbortInflightDial, ChannelSession::Close, DropLink, completion
   * callbacks. Drained after Tick + PostToIo so handlers never run under mux delivery.
   */
  void PostDeferred(IoTask task);

  /**
   * Amp-clock delayed task (`Endpoint::GetClock().NowMs()`). Prefer over steady_clock
   * polls so VirtualClock harnesses stay deterministic.
   */
  TimerId PostAfter(std::chrono::milliseconds delay, IoTask task);
  /** Absolute Amp-clock deadline. */
  TimerId PostAt(int64_t deadline_ms_abs, IoTask task);
  void CancelTimer(TimerId id);

  template <typename Fn>
  auto WithIoLock(Fn&& fn) -> decltype(fn()) {
    std::lock_guard lock(io_mu_);
    return std::forward<Fn>(fn)();
  }
  template <typename Fn>
  auto WithIoLock(Fn&& fn) const -> decltype(fn()) {
    std::lock_guard lock(io_mu_);
    return std::forward<Fn>(fn)();
  }

  /**
   * `tick` runs on the io lane at the housekeeping cadence (SetHousekeepingInterval) whether or not anything is due:
   * an idle driver then wakes that often. Prefer AddIoDueTick.
   */
  IoTickId AddIoTick(IoTask tick);
  /**
   * `tick` runs on the io lane when it asked to (its result: how soon it runs again), and at the housekeeping cadence
   * while the lane is active — never more often than that cadence. A deadline sweep returns its earliest deadline;
   * one that polls state other events change (a link gone, a session closed) is run soon after those events anyway.
   */
  IoTickId AddIoDueTick(IoDueTick tick);
  /**
   * How often the walks run — over every connection, every link and the io ticks (default
   * kDefaultHousekeepingInterval; 0: every round). Datagrams, posted tasks and timers are every round whatever this
   * says. A walk per round cost a busy node its links times its datagram rate.
   */
  void SetHousekeepingInterval(std::chrono::milliseconds interval);
  void RemoveIoTick(IoTickId id);

  // --- Product-facing link plane (forwards to PeerLinkManager under strand) ---
  /** Link lifecycle events; delivered on the PostToIo lane. */
  LinkEventListenerId AddLinkEventListener(LinkEventListener listener);
  void RemoveLinkEventListener(LinkEventListenerId id);
  /** PeerLinkManager::RequestDropLink under the strand. */
  size_t RequestDropLink(const DialKey& peer_key);
  void EnsureAssociation(const DialKey& peer_key, PeerLinkManager::LinkCb on_complete);
  void OpenChannel(const DialKey& peer_key, const std::string& protocol_id, ChannelPolicy policy,
                   PeerLinkManager::ChannelCb on_complete);
  /**
   * Absolute Amp-clock deadline (`Endpoint::GetClock().NowMs()`). Prefer WhenChannelOpenIn.
   */
  void WhenChannelOpen(const DialKey& peer_key, uint32_t channel_id, int64_t deadline_ms_abs,
                       std::function<void(bool ok)> done);
  /** Remaining duration from Amp clock now → absolute deadline for WhenChannelOpen. */
  void WhenChannelOpenIn(const DialKey& peer_key, uint32_t channel_id, std::chrono::milliseconds remaining,
                         std::function<void(bool ok)> done);
  std::shared_ptr<ChannelSession> BindChannel(const DialKey& peer_key, uint32_t channel_id, ChannelPolicy policy,
                                              ChannelSession::FrameHandler on_frame,
                                              ChannelSession::ClosedCallback on_closed = {});
  LinkSnapshotEx SnapshotByDialKey(const DialKey& key) const;
  LinkSnapshotEx SnapshotByPeerId(const std::string& peer_id, TransportClass prefer = TransportClass::Adp) const;
  PeerLinkSnapshot GetLinkSnapshot(const DialKey& peer_key) const;
  bool IsConnected(const DialKey& peer_key) const;
  bool IsConnectedToPeerId(const std::string& peer_id) const;
  bool IsReachable(const std::string& peer_id) const;
  Roe<void> RegisterEndpoint(const DialKey& peer_key, const std::string& multiaddr);
  Roe<void> RegisterEndpoints(const DialKey& peer_key, const std::vector<std::string>& multiaddrs);
  /** See PeerLinkManager::UnregisterEndpoint. */
  void UnregisterEndpoint(const DialKey& peer_key);
  std::optional<std::string> PreferredMultiaddr(const std::string& peer_id) const;

  void SetProtocolHandler(const std::string& protocol_id, PeerLinkManager::ProtocolHandler handler);
  /** See PeerLinkManager::SetRefuseUnhandledOpens — product hosts whose handlers are the whole surface. */
  void SetRefuseUnhandledOpens(bool refuse);
  void RemoveProtocolHandler(const std::string& protocol_id);
  void SetCapabilityHandler(PeerLinkManager::CapabilityHandler handler);
  void SetLocalListenMultiaddrs(std::vector<std::string> multiaddrs);
  void SetAdvertisedProtocols(std::vector<std::string> protocols);
  /** See PeerLinkManager::SetAdvertisedNodeInfo (N032). */
  void SetAdvertisedNodeInfo(std::string node_version, std::vector<std::string> features);
  /** See PeerLinkManager::RemoteCapability (N032). */
  std::optional<CapabilityPayload> RemoteCapability(const std::string& peer_id) const;

  void MarkWarm(const DialKey& peer_key);
  void MarkHot(const DialKey& peer_key);
  void ClearWarm(const DialKey& peer_key);
  void ClearDialBackoff(const DialKey& peer_key);
  /** Local network changed: probe every ADP link, evict the silent ones fast (PeerLinkManager::OnNetworkChanged). Any thread. */
  void NotifyNetworkChanged();
  void AbortInflightDial(const DialKey& peer_key);

  /**
   * Parallel ephemeral dials to ADP multiaddrs within an Amp-clock window.
   * Registers under DialKeys `amp:burst:N:…` so inbound PeerId adopt can own the
   * live link (A026). Win = IsConnectedToPeerId (Connected ADP, not carrier).
   * Abort losers + on_done settle via PostDeferred. Prefer calling from PostToIo /
   * SM work already off the mux stack — do not start under ChannelMux delivery.
   */
  void BurstDial(const std::vector<std::string>& multiaddrs, std::chrono::milliseconds window, BurstDialCb on_done);

  void EstablishNestedOverCarrier(const DialKey& peer_key, std::shared_ptr<ChannelSession> carrier, bool initiator,
                                  PeerLinkManager::LinkCb on_complete);
  void EnableNestedCarrierAccept(bool enable, std::string protocol_id = kAmpCircuitCarrierProtocolId);

  template <typename Fn>
  bool WithLiveLink(LinkHandle handle, Fn&& fn) {
    return links_.WithLiveLink(handle, std::forward<Fn>(fn));
  }
  template <typename Fn>
  bool WithLiveLinkByDialKey(const DialKey& key, Fn&& fn) {
    return links_.WithLiveLinkByDialKey(key, std::forward<Fn>(fn));
  }

private:
  bool BeginDriveLocked();
  void EndDriveLocked();
  /** Runs the io ticks that are due, then drains posted work and the socket: datagrams handled; `ran` += tasks. */
  size_t PumpLocked(size_t& ran);
  void TickLocked();
  size_t RunDueIoTicksLocked(int64_t now);
  size_t DrainPostedIoLocked();
  size_t DrainDeferredLocked();
  size_t FireTimersLocked();
  TimerId ArmTimerLocked(int64_t deadline_ms_abs, IoTask task);
  /** The lane was active: walk and tick again within the housekeeping interval (not later than their due). */
  void PullHousekeepingLocked();
  /** What a post, a timer, a resend or a link's queued work makes due (cheap: no walk). */
  int64_t FastDueMsLocked(int64_t now) const;
  /** FastDueMsLocked, the walk's due and the io ticks'. */
  int64_t NextDueMsLocked(int64_t now) const;
  static std::chrono::milliseconds DueIn(int64_t due_ms, int64_t now);
  /** IoLaneMutex hook: a thread let go of the lane. */
  static void OnLaneRelease(void* self);
  void OnLaneReleaseLocked();

  struct IoTickEntry {
    IoTickId id = 0;
    IoTask tick;
    IoDueTick due_tick;
    /** When it runs next (Amp clock); INT64_MIN: at the next turn. */
    int64_t due_ms = std::numeric_limits<int64_t>::min();
    /** When it last ran; INT64_MIN: never. */
    int64_t last_ms = std::numeric_limits<int64_t>::min();
  };
  static constexpr int64_t kNotDue = std::numeric_limits<int64_t>::max();
  /** pump_idle_until_ms_ when no Drive has planned a wake. */
  static constexpr int64_t kNotWaiting = std::numeric_limits<int64_t>::min();

  struct TimerEntry {
    TimerId id = 0;
    int64_t deadline_ms = 0;
    IoTask task;
  };

  adp::Endpoint& endpoint_;
  mutable IoLaneMutex io_mu_;
  PeerLinkManager links_;
  MeshPump pump_;
  std::deque<IoTask> io_queue_;
  std::deque<IoTask> deferred_queue_;
  std::vector<TimerEntry> timers_;
  std::vector<IoTickEntry> io_ticks_;
  int64_t housekeeping_interval_ms_ = kDefaultHousekeepingInterval.count();
  /** When the link walk last ran (endpoint clock; INT64_MIN: never) and when it runs next. */
  int64_t last_sweep_ms_ = std::numeric_limits<int64_t>::min();
  int64_t sweep_due_ms_ = std::numeric_limits<int64_t>::min();
  /** The earliest timer deadline (may be early after a cancel: the next fire looks again). */
  int64_t timers_due_ms_ = kNotDue;
  /** A thread other than the driver changed the lane since the last turn. */
  bool touched_ = false;
  /** EndDriveLocked to the lock's release: that release is the drive's own. */
  bool ending_drive_ = false;
  /** When the last Drive's next_due ends (Amp clock), until a wake; kNotWaiting: none. */
  int64_t pump_idle_until_ms_ = kNotWaiting;
  std::function<void()> wake_;
  IoTickId next_io_tick_id_ = 1;
  TimerId next_timer_id_ = 1;
  bool started_ = false;
  std::atomic<bool> driving_{false};
  /** The thread inside Pump/Tick/Drive now (none outside a drive). */
  std::atomic<std::thread::id> driving_thread_{};
  /** ClaimDriver's thread (none: unclaimed). */
  std::atomic<std::thread::id> claimed_driver_{};
  std::atomic<uint64_t> refused_drives_{0};
  /** Resolves `/dns*` candidates off the strand. Last: destroyed first, so no lookup posts into a
   *  runtime that is going. */
  HostResolver resolver_;
  /** Gates IoExecutor's calls into this runtime (invalidated first thing in the destructor). */
  Lifetime io_lifetime_;
  std::shared_ptr<SerialExecutor> io_executor_;
};

} // namespace pp::amp
