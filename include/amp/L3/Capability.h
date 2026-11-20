#pragma once

#include "amp/L3/ChannelWire.h"

#include <string>
#include <vector>

namespace pp::amp {

/**
 * Channel 0's capability offer: who the node is, where it listens, what protocols it serves, and what it runs
 * (N032). Later fields append: a decoder ignores bytes past the ones it knows.
 */
struct CapabilityPayload {
  std::string local_peer_id;
  std::vector<std::string> listen_multiaddrs;
  std::vector<std::string> protocols;
  /** The sender's amp wire version (adp::kWireVersion). */
  uint8_t amp_wire = 0;
  /** The node's software version (semver), as its host sets it; empty when unset. */
  std::string node_version;
  /** What the node serves, as its host names it (e.g. "hop_data_lane"). */
  std::vector<std::string> features;
};

/**
 * Channel 0's leading bytes: 2 session control (kSessionControlWireVersion), 4 the capability offer. 1 (the offer
 * before it said what the node runs) and 3 (that, alone) are retired: they are refused, never read.
 *
 * Frozen (release compatibility U6): the offer keeps leading byte 4 for good. A later release adds fields by
 * appending them after `features`; it never changes the byte, since a reader of another byte learns nothing.
 */
inline constexpr uint8_t kCapabilityOfferVersion = 4;

/**
 * A peer's channel-0 offer that did not decode (U6) — most likely a later release's, or a retired one. The reader
 * still answers with its own offer, so the peer learns what this side runs.
 */
struct UnreadableCapabilityOffer {
  /** The offer's first byte (kCapabilityOfferVersion when it was a v4 offer that did not parse). */
  uint8_t leading_byte = 0;
  /** Why it did not decode. */
  std::string error;
  /** "unreadable offer, byte N (why)". */
  std::string Describe() const {
    return "unreadable offer, byte " + std::to_string(static_cast<unsigned>(leading_byte)) + " (" + error + ")";
  }
};

class CapabilityCodec {
public:
  static Roe<std::vector<uint8_t>> Encode(const CapabilityPayload& payload);
  static Roe<CapabilityPayload> Decode(std::span<const uint8_t> wire);
};

} // namespace pp::amp
