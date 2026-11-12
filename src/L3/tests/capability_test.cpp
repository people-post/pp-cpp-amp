#include "amp/L2/SessionControl.h"
#include "amp/L3/Capability.h"

#include <gtest/gtest.h>

namespace pp::amp {
namespace {

TEST(CapabilityTest, EncodeDecodeRoundTrip) {
  CapabilityPayload payload;
  payload.local_peer_id = "QmTestPeer";
  payload.listen_multiaddrs = {"/ip4/127.0.0.1/udp/4001/adp/2/p2p/QmTestPeer"};
  payload.protocols = {"/test/chat/1.0.0", "/test/call-media/1.0.0"};
  payload.amp_wire = 2;
  payload.node_version = "4.1.0";
  payload.features = {"hop_data_lane", "standby_keepalive"};

  auto wire = CapabilityCodec::Encode(payload);
  ASSERT_TRUE(static_cast<bool>(wire));
  EXPECT_FALSE(SessionControlCodec::LooksLike(*wire)) << "never taken for session control (channel 0's other message)";
  auto decoded = CapabilityCodec::Decode(*wire);
  ASSERT_TRUE(static_cast<bool>(decoded)) << decoded.error().message;
  EXPECT_EQ(decoded->local_peer_id, payload.local_peer_id);
  EXPECT_EQ(decoded->listen_multiaddrs, payload.listen_multiaddrs);
  EXPECT_EQ(decoded->protocols, payload.protocols);
  EXPECT_EQ(decoded->amp_wire, 2);
  EXPECT_EQ(decoded->node_version, "4.1.0");
  EXPECT_EQ(decoded->features, payload.features);

  // A later version's extra fields are ignored, not refused.
  auto with_later_fields = *wire;
  with_later_fields.insert(with_later_fields.end(), {0x01, 0x02, 0x03});
  auto later = CapabilityCodec::Decode(with_later_fields);
  ASSERT_TRUE(static_cast<bool>(later)) << later.error().message;
  EXPECT_EQ(later->features, payload.features);
}

// The retired offer versions (1: before the offer said what the node runs; 3: that, alone) are refused, as is an
// offer cut short.
TEST(CapabilityTest, ARetiredOrShortOfferIsRefused) {
  CapabilityPayload payload;
  payload.local_peer_id = "QmTestPeer";
  auto wire = CapabilityCodec::Encode(payload);
  ASSERT_TRUE(static_cast<bool>(wire));
  for (const uint8_t retired : {uint8_t{1}, uint8_t{3}}) {
    auto old = *wire;
    old[0] = retired;
    EXPECT_FALSE(static_cast<bool>(CapabilityCodec::Decode(old))) << int(retired);
  }
  auto cut = *wire;
  cut.resize(cut.size() - 3);
  EXPECT_FALSE(static_cast<bool>(CapabilityCodec::Decode(cut)));
}

} // namespace
} // namespace pp::amp
