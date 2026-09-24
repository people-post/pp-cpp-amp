#pragma once

#include "amp/L1/Endpoint.h"
#include "amp/link/MeshPump.h"
#include "amp/link/PeerLinkManager.h"
#include "amp/L2/Types.h"
#include "amp/L3/ChannelPolicy.h"
#include "amp/L3/ChannelSession.h"
#include "amp/link/LinkIdentity.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
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
 *   acting as Amp). Nested Pump/Tick/Drive is refused (assert in debug).
 * - PostToIo — work lane (SM steps, dial start, enqueue). Drained during Drive.
 * - PostDeferred — teardown lane (Abort, Close, DropLink, on_done). Runs after Tick
 *   and work drain so mux/session frames are off the stack.
 * - PostAfter — Amp-clock timers (sync windows, deadlines). Fired each Drive turn.
 */
class MeshRuntime {
public:
  using IoTask = std::function<void()>;
  using IoTickId = uint64_t;
  using TimerId = uint64_t;
  using BurstDialCb = std::function<void(BurstDialResult)>;

  MeshRuntime(adp::Endpoint& endpoint, MshIdentity local_identity, std::string local_peer_id,
              PeerLinkConfig config = {});

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

  /** True while Pump/Tick/Drive is on the stack (exclusive-driver guard). */
  bool IsDriving() const { return driving_; }

  void Pump();
  void Tick();
  void Drive();

  /** Work lane — drained during Drive (after io ticks / before or with pump). */
  void PostToIo(IoTask task);

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

  IoTickId AddIoTick(IoTask tick);
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
  std::shared_ptr<ChannelSession> BindChannel(const DialKey& peer_key, uint32_t channel_id,
                                              ChannelPolicy policy,
                                              ChannelSession::FrameHandler on_frame,
                                              ChannelSession::ClosedCallback on_closed = {});
  LinkSnapshotEx SnapshotByDialKey(const DialKey& key) const;
  LinkSnapshotEx SnapshotByPeerId(const std::string& peer_id,
                                  TransportClass prefer = TransportClass::Adp) const;
  PeerLinkSnapshot GetLinkSnapshot(const DialKey& peer_key) const;
  bool IsConnected(const DialKey& peer_key) const;
  bool IsConnectedToPeerId(const std::string& peer_id) const;
  bool IsReachable(const std::string& peer_id) const;
  Roe<void> RegisterEndpoint(const DialKey& peer_key, const std::string& multiaddr);
  Roe<void> RegisterEndpoints(const DialKey& peer_key, const std::vector<std::string>& multiaddrs);
  std::optional<std::string> PreferredMultiaddr(const std::string& peer_id) const;

  void SetProtocolHandler(const std::string& protocol_id, PeerLinkManager::ProtocolHandler handler);
  void RemoveProtocolHandler(const std::string& protocol_id);
  void SetCapabilityHandler(PeerLinkManager::CapabilityHandler handler);
  void SetLocalListenMultiaddrs(std::vector<std::string> multiaddrs);
  void SetAdvertisedProtocols(std::vector<std::string> protocols);

  void MarkWarm(const DialKey& peer_key);
  void MarkHot(const DialKey& peer_key);
  void ClearWarm(const DialKey& peer_key);
  void ClearDialBackoff(const DialKey& peer_key);
  void AbortInflightDial(const DialKey& peer_key);

  /**
   * Parallel ephemeral dials to ADP multiaddrs within an Amp-clock window.
   * Registers under DialKeys `amp:burst:N:…` so inbound PeerId adopt can own the
   * live link (A026). Win = IsConnectedToPeerId (Connected ADP, not carrier).
   * Abort losers + on_done settle via PostDeferred. Prefer calling from PostToIo /
   * SM work already off the mux stack — do not start under ChannelMux delivery.
   */
  void BurstDial(const std::vector<std::string>& multiaddrs, std::chrono::milliseconds window,
                 BurstDialCb on_done);

  void EstablishNestedOverCarrier(const DialKey& peer_key, std::shared_ptr<ChannelSession> carrier,
                                  bool initiator, PeerLinkManager::LinkCb on_complete);
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
  void PumpLocked();
  void TickLocked();
  void DrainPostedIoLocked();
  void DrainDeferredLocked();
  void FireTimersLocked();
  TimerId ArmTimerLocked(int64_t deadline_ms_abs, IoTask task);

  struct IoTickEntry {
    IoTickId id = 0;
    IoTask tick;
  };

  struct TimerEntry {
    TimerId id = 0;
    int64_t deadline_ms = 0;
    IoTask task;
  };

  adp::Endpoint& endpoint_;
  mutable std::recursive_mutex io_mu_;
  PeerLinkManager links_;
  MeshPump pump_;
  std::deque<IoTask> io_queue_;
  std::deque<IoTask> deferred_queue_;
  std::vector<TimerEntry> timers_;
  std::vector<IoTickEntry> io_ticks_;
  IoTickId next_io_tick_id_ = 1;
  TimerId next_timer_id_ = 1;
  bool started_ = false;
  bool driving_ = false;
};

} // namespace pp::amp
