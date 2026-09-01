#pragma once

#include "amp/L1/Endpoint.h"
#include "amp/link/MeshPump.h"
#include "amp/link/PeerLinkManager.h"
#include "amp/L2/Types.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace pp::amp {

/**
 * Io-thread composer for Endpoint + PeerLinkManager + MeshPump.
 * L4 services must not touch ChannelSession/Mux off-thread except via PostToIo().
 *
 * Pump/Tick/Drive/PostToIo are serialized (recursive_mutex): product may pump from the
 * coordinator Tick and from worker Connect wait loops without data races.
 */
class MeshRuntime {
public:
  using IoTask = std::function<void()>;
  /** Opaque id from AddIoTick; 0 is never assigned. */
  using IoTickId = uint64_t;

  MeshRuntime(adp::Endpoint& endpoint, MshIdentity local_identity, std::string local_peer_id,
              PeerLinkConfig config = {});

  adp::Endpoint& GetEndpoint() { return endpoint_; }
  PeerLinkManager& Links() { return links_; }
  const PeerLinkManager& Links() const { return links_; }

  void Start();
  void Stop();
  bool IsStarted() const { return started_; }

  /**
   * ADP pump + link tick + drain PostToIo (io entry).
   * Reentrant: a nested Pump from an in-pump callback still drives ADP I/O
   * (needed by L4 OpenChannel wait loops) but does not re-enter ticks/queue.
   */
  void Pump();
  void Tick();
  /** Pump then Tick under one lock — prefer for MeshHost product ticks. */
  void Drive();

  /** Queue work for the next Pump(); one queued task runs per Pump() before ADP I/O. */
  void PostToIo(IoTask task);

  /**
   * Register a Pump()-start hook (e.g. L4 connect deadlines). Multiple L4 coordinators
   * on one runtime must each AddIoTick — a single slot would overwrite peers.
   */
  IoTickId AddIoTick(IoTask tick);
  void RemoveIoTick(IoTickId id);

private:
  void PumpLocked();
  void TickLocked();

  struct IoTickEntry {
    IoTickId id = 0;
    IoTask tick;
  };

  adp::Endpoint& endpoint_;
  PeerLinkManager links_;
  MeshPump pump_;
  std::deque<IoTask> io_queue_;
  std::vector<IoTickEntry> io_ticks_;
  IoTickId next_io_tick_id_ = 1;
  bool started_ = false;
  bool pumping_ = false;
  mutable std::recursive_mutex io_mu_;
};

} // namespace pp::amp
