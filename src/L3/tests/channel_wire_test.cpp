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

std::vector<uint8_t> Encoded(const ChannelFrame& frame) {
  auto wire = ChannelWire::Encode(frame);
  EXPECT_TRUE(static_cast<bool>(wire));
  return wire ? *wire : std::vector<uint8_t>{};
}

ChannelFrame Control(const ChannelFrameType type, const uint32_t channel_id) {
  ChannelFrame frame;
  frame.header.frame_type = type;
  frame.header.channel_id = channel_id;
  return frame;
}

// Release compatibility T9: OPEN, OpenAck, CLOSE and RESET read the fields they know and ignore what a later release
// appends (before, the frame was refused and dropped silently: the opener waited out its deadline).
TEST(ChannelWireTest, ControlFramesIgnoreTrailingBytes) {
  const std::vector<uint8_t> later = {0xA1, 0xB2, 0xC3};

  ChannelFrame open = Control(ChannelFrameType::Open, 7);
  open.open.protocol_id = "/test/chat/1.0.0";
  open.open.channel_class = ChannelClass::Bulk;
  open.open.flags = 0x8001;
  open.open.max_message_bytes = 4096;
  auto wire = Encoded(open);
  wire.insert(wire.end(), later.begin(), later.end());
  auto decoded = ChannelWire::Decode(wire);
  ASSERT_TRUE(static_cast<bool>(decoded)) << decoded.error().message;
  EXPECT_EQ(decoded->open.protocol_id, "/test/chat/1.0.0");
  EXPECT_EQ(decoded->open.channel_class, ChannelClass::Bulk);
  EXPECT_EQ(decoded->open.flags, 0x8001);
  EXPECT_EQ(decoded->open.max_message_bytes, 4096u);

  ChannelFrame ack = Control(ChannelFrameType::OpenAck, 7);
  ack.open_ack_result = 2;
  wire = Encoded(ack);
  wire.insert(wire.end(), later.begin(), later.end());
  decoded = ChannelWire::Decode(wire);
  ASSERT_TRUE(static_cast<bool>(decoded)) << decoded.error().message;
  EXPECT_EQ(decoded->open_ack_result, 2);

  ChannelFrame close = Control(ChannelFrameType::Close, 7);
  close.payload = {'b', 'y', 'e'};
  wire = Encoded(close);
  wire.insert(wire.end(), later.begin(), later.end());
  decoded = ChannelWire::Decode(wire);
  ASSERT_TRUE(static_cast<bool>(decoded)) << decoded.error().message;
  EXPECT_EQ(decoded->payload, (std::vector<uint8_t>{'b', 'y', 'e'}));

  ChannelFrame reset = Control(ChannelFrameType::Reset, 7);
  reset.reset_code = 0x01020304u;
  wire = Encoded(reset);
  wire.insert(wire.end(), later.begin(), later.end());
  decoded = ChannelWire::Decode(wire);
  ASSERT_TRUE(static_cast<bool>(decoded)) << decoded.error().message;
  EXPECT_EQ(decoded->reset_code, 0x01020304u);
}

// What this release writes is byte for byte what v6.0.0 wrote: no field was added.
TEST(ChannelWireTest, GoldenControlFrames) {
  ChannelFrame reset = Control(ChannelFrameType::Reset, 0x04030201u);
  reset.header.channel_seq = 5;
  reset.reset_code = 1;
  EXPECT_EQ(Encoded(reset), (std::vector<uint8_t>{4, 1, 2, 3, 4, 5, 0, 0, 0, 1, 0, 0, 0}));
  ChannelFrame ack = Control(ChannelFrameType::OpenAck, 2);
  ack.open_ack_result = 1;
  EXPECT_EQ(Encoded(ack), (std::vector<uint8_t>{1, 2, 0, 0, 0, 0, 0, 0, 0, 1}));
  EXPECT_EQ(Encoded(Control(ChannelFrameType::Close, 2)), (std::vector<uint8_t>{3, 2, 0, 0, 0, 0, 0, 0, 0}));
}

TEST(ChannelWireTest, TruncatedKnownFieldsAreStillRefused) {
  const std::vector<uint8_t> reset = {4, 7, 0, 0, 0, 0, 0, 0, 0, 1, 0}; // a RESET code of two bytes
  EXPECT_FALSE(static_cast<bool>(ChannelWire::Decode(reset)));
  const std::vector<uint8_t> ack = {1, 7, 0, 0, 0, 0, 0, 0, 0}; // no result
  EXPECT_FALSE(static_cast<bool>(ChannelWire::Decode(ack)));
  const std::vector<uint8_t> close = {3, 7, 0, 0, 0, 0, 0, 0, 0, 9, 0, 0, 0, 'x'}; // a 9-byte reason, 1 there
  EXPECT_FALSE(static_cast<bool>(ChannelWire::Decode(close)));
}

// An unknown frame type decodes as its header and body, so the mux can answer it instead of dropping it unseen.
TEST(ChannelWireTest, AnUnknownFrameTypeDecodesAsItsHeader) {
  const std::vector<uint8_t> wire = {9, 5, 0, 0, 0, 0, 0, 0, 0, 0xEE, 0xFF};
  auto decoded = ChannelWire::Decode(wire);
  ASSERT_TRUE(static_cast<bool>(decoded)) << decoded.error().message;
  EXPECT_FALSE(ChannelWire::IsKnownFrameType(decoded->header.frame_type));
  EXPECT_EQ(static_cast<uint8_t>(decoded->header.frame_type), 9);
  EXPECT_EQ(decoded->header.channel_id, 5u);
  EXPECT_EQ(decoded->payload, (std::vector<uint8_t>{0xEE, 0xFF}));
  for (uint8_t t = 0; t <= 5; ++t) {
    EXPECT_TRUE(ChannelWire::IsKnownFrameType(static_cast<ChannelFrameType>(t))) << int(t);
  }
}

} // namespace
} // namespace pp::amp
