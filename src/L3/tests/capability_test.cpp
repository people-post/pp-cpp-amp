#include "amp/L3/Capability.h"

#include <gtest/gtest.h>

namespace pp::amp {
namespace {

TEST(CapabilityTest, EncodeDecodeRoundTrip) {
  CapabilityPayload payload;
  payload.local_peer_id = "QmTestPeer";
  payload.listen_multiaddrs = {"/ip4/127.0.0.1/udp/4001/adp/1.0.0/p2p/QmTestPeer"};
  payload.protocols = {"/pp-browser/chat/1.0.0", "/pp-browser/call-media/1.0.0"};

  auto wire = CapabilityCodec::Encode(payload);
  ASSERT_TRUE(static_cast<bool>(wire));
  auto decoded = CapabilityCodec::Decode(*wire);
  ASSERT_TRUE(static_cast<bool>(decoded));
  EXPECT_EQ(decoded->local_peer_id, payload.local_peer_id);
  EXPECT_EQ(decoded->listen_multiaddrs, payload.listen_multiaddrs);
  EXPECT_EQ(decoded->protocols, payload.protocols);
}

} // namespace
} // namespace pp::amp
