#pragma once

#include "amp/L1/Connection.h"
#include "amp/L1/DatagramIo.h"
#include "amp/L1/Types.h"

#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace pp::adp {

class Endpoint {
public:
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

private:
  void HandleDatagram(const IpEndpoint& from, std::span<const uint8_t> datagram);

  std::shared_ptr<DatagramIo> io_;
  std::shared_ptr<Clock> clock_;
  std::unordered_map<AssocId, std::shared_ptr<Connection>, AssocIdHash> conns_;
  std::optional<PeerKey> accept_key_;
  bool accept_enabled_ = false;
  AcceptHandler accept_handler_;
  size_t max_accepted_conns_ = 4096;
};

} // namespace pp::adp
