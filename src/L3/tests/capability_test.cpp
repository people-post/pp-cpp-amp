#include "amp/L2/SessionControl.h"
#include "amp/L3/Capability.h"

#include <gtest/gtest.h>

namespace pp::amp {
namespace {

TEST(CapabilityTest, EncodeDecodeRoundTrip) {
  CapabilityPayload payload;
  payload.local_peer_id = "QmTestPeer";
  payload.listen_multiaddrs = {"/ip4/127.0.0.1/udp/4001/adp/1.0.0/p2p/QmTestPeer"};
  payload.protocols = {"/test/chat/1.0.0", "/test/call-media/1.0.0"};

  auto wire = CapabilityCodec::Encode(payload);
  ASSERT_TRUE(static_cast<bool>(wire));
  auto decoded = CapabilityCodec::Decode(*wire);
  ASSERT_TRUE(static_cast<bool>(decoded));
  EXPECT_EQ(decoded->local_peer_id, payload.local_peer_id);
  EXPECT_EQ(decoded->listen_multiaddrs, payload.listen_multiaddrs);
  EXPECT_EQ(decoded->protocols, payload.protocols);
}

// N032: channel 0's second message. A decoder of the first (every peer before N032) refuses it, so such a peer drops
// it after the v1 exchange instead of misreading it; a later version's extra fields are ignored, not refused.
TEST(CapabilityTest, TheExtensionRoundTripsAndOlderDecodersRefuseIt) {
  CapabilityExtension extension;
  extension.amp_wire = 2;
  extension.node_version = "4.1.0";
  extension.features = {"hop_data_lane", "standby_keepalive"};
  auto wire = CapabilityCodec::EncodeExtension(extension);
  ASSERT_TRUE(static_cast<bool>(wire));
  EXPECT_TRUE(CapabilityCodec::IsExtension(*wire));
  EXPECT_FALSE(static_cast<bool>(CapabilityCodec::Decode(*wire))) << "a v1 decoder drops it";
  EXPECT_FALSE(SessionControlCodec::LooksLike(*wire)) << "never taken for session control (channel 0's other message)";

  auto with_later_fields = *wire;
  with_later_fields.insert(with_later_fields.end(), {0x01, 0x02, 0x03});
  auto decoded = CapabilityCodec::DecodeExtension(with_later_fields);
  ASSERT_TRUE(static_cast<bool>(decoded)) << decoded.error().message;
  EXPECT_EQ(decoded->amp_wire, 2);
  EXPECT_EQ(decoded->node_version, "4.1.0");
  EXPECT_EQ(decoded->features, extension.features);

  CapabilityPayload v1;
  v1.local_peer_id = "QmTestPeer";
  auto v1_wire = CapabilityCodec::Encode(v1);
  ASSERT_TRUE(static_cast<bool>(v1_wire));
  EXPECT_FALSE(CapabilityCodec::IsExtension(*v1_wire));
  EXPECT_FALSE(static_cast<bool>(CapabilityCodec::DecodeExtension(*v1_wire)));
}

} // namespace
} // namespace pp::amp
