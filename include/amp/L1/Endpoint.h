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

  void Pump();
  void Tick();

  Roe<void> SendRaw(const IpEndpoint& peer, std::span<const uint8_t> datagram);

  void Unregister(const AssocId& id);

  void SetAcceptKey(PeerKey key) { accept_key_ = key; }
  void SetAcceptEnabled(bool on) { accept_enabled_ = on; }
  using AcceptHandler = std::function<void(std::shared_ptr<Connection>)>;
  void SetAcceptHandler(AcceptHandler handler) { accept_handler_ = std::move(handler); }

private:
  void HandleDatagram(const IpEndpoint& from, std::span<const uint8_t> datagram);

  std::shared_ptr<DatagramIo> io_;
  std::shared_ptr<Clock> clock_;
  std::unordered_map<AssocId, std::shared_ptr<Connection>, AssocIdHash> conns_;
  std::optional<PeerKey> accept_key_;
  bool accept_enabled_ = false;
  AcceptHandler accept_handler_;
};

} // namespace pp::adp
