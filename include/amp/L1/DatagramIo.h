#pragma once

#include "amp/L1/Types.h"


#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace pp::adp {

class DatagramIo {
public:
  virtual ~DatagramIo() = default;

  virtual Roe<void> SendTo(const IpEndpoint& peer, std::span<const uint8_t> datagram) = 0;

  /** Non-blocking: returns nullopt if no datagram ready. */
  virtual Roe<std::optional<std::pair<IpEndpoint, std::vector<uint8_t>>>> RecvFrom() = 0;

  virtual IpEndpoint LocalEndpoint() const = 0;
};

} // namespace pp::adp
