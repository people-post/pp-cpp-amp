#pragma once

#include "amp/L1/DatagramIo.h"

#include <memory>

namespace pp::adp {

/** Thin non-blocking UDP socket (POSIX / Winsock). Asio-free. */
class OsUdpDatagramIo final : public DatagramIo {
public:
  static Roe<std::unique_ptr<OsUdpDatagramIo>> Bind(const IpEndpoint& local);

  ~OsUdpDatagramIo() override;

  Roe<void> SendTo(const IpEndpoint& peer, std::span<const uint8_t> datagram) override;
  Roe<std::optional<std::pair<IpEndpoint, std::vector<uint8_t>>>> RecvFrom() override;
  IpEndpoint LocalEndpoint() const override { return local_; }

  OsUdpDatagramIo(const OsUdpDatagramIo&) = delete;
  OsUdpDatagramIo& operator=(const OsUdpDatagramIo&) = delete;

private:
  OsUdpDatagramIo(int fd, IpEndpoint local);

  int fd_ = -1;
  IpEndpoint local_;
};

} // namespace pp::adp
