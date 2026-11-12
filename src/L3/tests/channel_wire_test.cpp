#include "amp/L3/ChannelWire.h"

#include <gtest/gtest.h>

namespace pp::amp {
namespace {

TEST(ChannelWireTest, OpenRoundTrip) {
  ChannelFrame frame;
  frame.header.frame_type = ChannelFrameType::Open;
  frame.header.channel_id = 7;
  frame.header.channel_seq = 0;
  frame.open.protocol_id = "/test/chat/1.0.0";
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

TEST(ChannelWireTest, AnOpenWithoutMaxMessageBytesIsRefused) {
  // Header + len-prefixed "/x" + class + flags; no trailing max_message_bytes u32.
  std::vector<uint8_t> wire = {0, 7, 0, 0, 0, 0, 0, 0, 0};
  wire.push_back(2);
  wire.push_back(0);
  wire.push_back(0);
  wire.push_back(0);
  wire.push_back('/');
  wire.push_back('x');
  wire.push_back(static_cast<uint8_t>(ChannelClass::Control));
  wire.push_back(0);
  wire.push_back(0); // flags = 0

  EXPECT_FALSE(static_cast<bool>(ChannelWire::Decode(wire)));
}

// Type, channel id, channel seq: 9 bytes, no version byte (the packet's first byte carries the wire version; A029 H1).
TEST(ChannelWireTest, GoldenDataHeaderLayout) {
  ChannelFrame frame;
  frame.header.frame_type = ChannelFrameType::Data;
  frame.header.channel_id = 0x04030201u;
  frame.header.channel_seq = 0x08070605u;
  frame.payload = {0xaa};
  auto wire = ChannelWire::Encode(frame);
  ASSERT_TRUE(static_cast<bool>(wire));
  const std::vector<uint8_t> expected = {static_cast<uint8_t>(ChannelFrameType::Data), 1, 2, 3, 4, 5, 6, 7, 8, 0xaa};
  EXPECT_EQ(*wire, expected);
  EXPECT_EQ(kChannelHeaderBytes, 9u);
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
