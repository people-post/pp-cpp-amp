#pragma once

#include "amp/L1/Types.h"


#include <array>
#include <span>
#include <vector>

namespace pp::adp {

class HmacBinder {
public:
  explicit HmacBinder(PeerKey key);

  Roe<std::vector<uint8_t>> Seal(std::span<const uint8_t> header_and_payload) const;
  /** Append HMAC in place (avoids an extra body copy when the caller owns the wire buffer). */
  Roe<std::vector<uint8_t>> Seal(std::vector<uint8_t>&& header_and_payload) const;
  Roe<void> Verify(std::span<const uint8_t> datagram) const;

  void SetKey(PeerKey key) { key_ = key; }

  static std::array<uint8_t, kHmacBytes> ComputeTag(const PeerKey& key,
                                                    std::span<const uint8_t> mac_input);

private:
  PeerKey key_;
};

} // namespace pp::adp
