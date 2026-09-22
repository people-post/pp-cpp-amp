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

/**
 * Io-thread composer for Endpoint + PeerLinkManager + MeshPump.
 * Sole product entry for link ops (ADR_LINK_PLANE). Prefer these APIs over bare Links().
 */
class MeshRuntime {
public:
  using IoTask = std::function<void()>;
  using IoTickId = uint64_t;

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

  void Pump();
  void Tick();
  void Drive();
  void PostToIo(IoTask task);

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
  void PumpLocked();
  void TickLocked();
  /** Drain PostToIo tasks posted by Tick (FinishDial / WhenChannelOpen). */
  void DrainPostedIoLocked();

  struct IoTickEntry {
    IoTickId id = 0;
    IoTask tick;
  };

  adp::Endpoint& endpoint_;
  mutable std::recursive_mutex io_mu_;
  PeerLinkManager links_;
  MeshPump pump_;
  std::deque<IoTask> io_queue_;
  std::vector<IoTickEntry> io_ticks_;
  IoTickId next_io_tick_id_ = 1;
  bool started_ = false;
  bool pumping_ = false;
};

} // namespace pp::amp
