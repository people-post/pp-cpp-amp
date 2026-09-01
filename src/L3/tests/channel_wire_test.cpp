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

  auto wire = ChannelWire::Encode(frame);
  ASSERT_TRUE(static_cast<bool>(wire));
  auto decoded = ChannelWire::Decode(*wire);
  ASSERT_TRUE(static_cast<bool>(decoded));
  EXPECT_EQ(decoded->open.protocol_id, frame.open.protocol_id);
  EXPECT_EQ(decoded->open.channel_class, frame.open.channel_class);
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
