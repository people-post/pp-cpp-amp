#include "amp/L3/ChannelWire.h"

#include <gtest/gtest.h>

namespace pp::amp {
namespace {

TEST(ChannelWireTest, OpenRoundTrip) {
  ChannelFrame frame;
  frame.header.frame_type = ChannelFrameType::Open;
  frame.header.channel_id = 7;
  frame.header.channel_seq = 0;
  frame.open.protocol_id = "/pp-browser/chat/1.0.0";
  frame.open.channel_class = ChannelClass::Transactional;
  frame.open.flags = 1;
  frame.open.max_message_bytes = 512 * 1024;

  auto wire = ChannelWire::Encode(frame);
  ASSERT_TRUE(static_cast<bool>(wire));
  auto decoded = ChannelWire::Decode(*wire);
  ASSERT_TRUE(static_cast<bool>(decoded));
  EXPECT_EQ(decoded->open.protocol_id, frame.open.protocol_id);
  EXPECT_EQ(decoded->open.channel_class, frame.open.channel_class);
  EXPECT_EQ(decoded->open.flags, frame.open.flags);
  EXPECT_EQ(decoded->open.max_message_bytes, frame.open.max_message_bytes);
}

TEST(ChannelWireTest, OpenDecodesLegacyWithoutMaxMessageBytes) {
  // Header + len-prefixed "/x" + class + flags; no trailing max_message_bytes u32.
  std::vector<uint8_t> wire = {1, 0, 7, 0, 0, 0, 0, 0, 0, 0};
  wire.push_back(2);
  wire.push_back(0);
  wire.push_back(0);
  wire.push_back(0);
  wire.push_back('/');
  wire.push_back('x');
  wire.push_back(static_cast<uint8_t>(ChannelClass::Control));
  wire.push_back(0);
  wire.push_back(0); // flags = 0

  auto decoded = ChannelWire::Decode(wire);
  ASSERT_TRUE(static_cast<bool>(decoded));
  EXPECT_EQ(decoded->open.protocol_id, "/x");
  EXPECT_EQ(decoded->open.max_message_bytes, 0u);
}

TEST(ChannelWireTest, DataRoundTrip) {
  ChannelFrame frame;
  frame.header.frame_type = ChannelFrameType::Data;
  frame.header.channel_id = 3;
  frame.header.channel_seq = 9;
  frame.payload = {'a', 'b', 'c'};

  auto wire = ChannelWire::Encode(frame);
  ASSERT_TRUE(static_cast<bool>(wire));
  auto decoded = ChannelWire::Decode(*wire);
  ASSERT_TRUE(static_cast<bool>(decoded));
  EXPECT_EQ(decoded->payload, frame.payload);
}

} // namespace
} // namespace pp::amp
