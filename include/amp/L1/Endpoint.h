#pragma once

#include "amp/L1/Connection.h"
#include "amp/L1/DatagramIo.h"
#include "amp/L1/Types.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace pp::adp {

/**
 * Traffic totals for one Endpoint since it was made (all its associations; no per-peer split).
 * Read from any thread with `Endpoint::Stats()`.
 */
struct EndpointStats {
  uint64_t tx_datagrams = 0;
  uint64_t tx_bytes = 0;
  uint64_t rx_datagrams = 0;
  uint64_t rx_bytes = 0;
  /** Received datagrams dropped before any association took them (bad HMAC / decode, unknown). */
  uint64_t rx_rejected = 0;
  /** Reliable data packets sent (first sends only). */
  uint64_t reliable_sent = 0;
  uint64_t retransmits = 0;
  /** Reliable packets given up after the retransmit cap (never acked). */
  uint64_t reliable_lost = 0;
  /** Round trips measured (Ack of a never-retransmitted Reliable packet — Karn) and their sum. */
  uint64_t rtt_samples = 0;
  uint64_t rtt_sum_ms = 0;
};

class Endpoint {
public:
  /** Tuning for connections this endpoint accepts, and its pre-association skew check. */
  void SetTuning(const AdpTuning& tuning) { tuning_ = tuning; }
  const AdpTuning& Tuning() const { return tuning_; }

  Endpoint(std::shared_ptr<DatagramIo> io, std::shared_ptr<Clock> clock);

  DatagramIo& Io() { return *io_; }
  Clock& GetClock() { return *clock_; }
  const Clock& GetClock() const { return *clock_; }

  Roe<std::shared_ptr<Connection>> Open(OpenParams params);

  Roe<std::shared_ptr<Connection>> AcceptOrCreate(const AssocId& id, const PeerKey& key,
                                                   const IpEndpoint& peer);

  std::shared_ptr<Connection> Find(const AssocId& id) const;

  /**
   * Drains up to `budget` datagrams (default kDefaultPumpBudget), not until EAGAIN: an
   * unbounded drain under a packet flood could hold a caller-shared lock (MeshRuntime's io_mu_)
   * for as long as packets keep arriving, starving every other PeerLinkManager op. The caller's
   * own loop (Tick cadence) picks up any remainder on the next call.
   */
  void Pump(size_t budget = kDefaultPumpBudget);
  void Tick();

  Roe<void> SendRaw(const IpEndpoint& peer, std::span<const uint8_t> datagram);

  void Unregister(const AssocId& id);

  void SetAcceptKey(PeerKey key) { accept_key_ = key; }
  void SetAcceptEnabled(bool on) { accept_enabled_ = on; }
  using AcceptHandler = std::function<void(std::shared_ptr<Connection>)>;
  void SetAcceptHandler(AcceptHandler handler) { accept_handler_ = std::move(handler); }
  /**
   * Backstop cap on newly *accepted* associations (explicit Open()/outbound dials are never
   * capped here). Independent of any higher-level link-table cap: even if a consumer forgets to
   * wire one, or its accept handler is slow to reject, conns_ cannot grow without bound.
   */
  void SetMaxAcceptedConnections(size_t max) { max_accepted_conns_ = max; }

  /** Traffic totals so far. Any thread. */
  EndpointStats Stats() const;
  /**
   * Called with each round-trip sample (ms), on the thread that drives this Endpoint (for a
   * histogram). Set before traffic starts.
   */
  using RttObserver = std::function<void(int64_t rtt_ms)>;
  void SetRttObserver(RttObserver observer) { rtt_observer_ = std::move(observer); }

  // Counted by the associations (Connection); not for other callers.
  void NoteReliableSent() { reliable_sent_.fetch_add(1, std::memory_order_relaxed); }
  void NoteRetransmit() { retransmits_.fetch_add(1, std::memory_order_relaxed); }
  void NoteReliableLost() { reliable_lost_.fetch_add(1, std::memory_order_relaxed); }
  void NoteRejected() { rx_rejected_.fetch_add(1, std::memory_order_relaxed); }
  void NoteRttSample(int64_t rtt_ms);

private:
  void HandleDatagram(const IpEndpoint& from, std::span<const uint8_t> datagram);

  std::shared_ptr<DatagramIo> io_;
  std::shared_ptr<Clock> clock_;
  std::unordered_map<AssocId, std::shared_ptr<Connection>, AssocIdHash> conns_;
  std::optional<PeerKey> accept_key_;
  bool accept_enabled_ = false;
  AcceptHandler accept_handler_;
  size_t max_accepted_conns_ = 4096;

  std::atomic<uint64_t> tx_datagrams_{0};
  std::atomic<uint64_t> tx_bytes_{0};
  std::atomic<uint64_t> rx_datagrams_{0};
  std::atomic<uint64_t> rx_bytes_{0};
  std::atomic<uint64_t> rx_rejected_{0};
  std::atomic<uint64_t> reliable_sent_{0};
  std::atomic<uint64_t> retransmits_{0};
  std::atomic<uint64_t> reliable_lost_{0};
  std::atomic<uint64_t> rtt_samples_{0};
  std::atomic<uint64_t> rtt_sum_ms_{0};
  RttObserver rtt_observer_;
  AdpTuning tuning_{};
};

} // namespace pp::adp
