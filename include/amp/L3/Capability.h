#pragma once

#include "amp/L3/ChannelWire.h"

#include <string>
#include <vector>

namespace pp::amp {

/** Channel 0 capability payload (binary v1). */
struct CapabilityPayload {
  std::string local_peer_id;
  std::vector<std::string> listen_multiaddrs;
  std::vector<std::string> protocols;
};

/**
 * Channel 0's second message (N032): what this node runs. Its own message with its own leading byte
 * (kCapabilityExtensionTag), so a peer from before it refuses it as an unknown capability version and drops it, after
 * the v1 exchange has completed. Later fields append: a decoder ignores bytes past the ones it knows.
 */
struct CapabilityExtension {
  /** The sender's amp wire version (adp::kWireVersion). */
  uint8_t amp_wire = 0;
  /** The node's software version (semver), as its host sets it; empty when unset. */
  std::string node_version;
  /** What the node serves, as its host names it (e.g. "hop_data_lane"). */
  std::vector<std::string> features;
};

/** Channel 0's leading bytes: 1 the capability offer, 2 session control (kSessionControlWireVersion), 3 this. */
inline constexpr uint8_t kCapabilityExtensionTag = 3;

class CapabilityCodec {
public:
  static Roe<std::vector<uint8_t>> Encode(const CapabilityPayload& payload);
  static Roe<CapabilityPayload> Decode(std::span<const uint8_t> wire);

  static Roe<std::vector<uint8_t>> EncodeExtension(const CapabilityExtension& extension);
  static Roe<CapabilityExtension> DecodeExtension(std::span<const uint8_t> wire);
  static bool IsExtension(std::span<const uint8_t> wire) { return !wire.empty() && wire[0] == kCapabilityExtensionTag; }
};

} // namespace pp::amp
