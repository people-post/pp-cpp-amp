#include "amp/L2/SessionControl.h"

#include <gtest/gtest.h>

namespace pp::amp {
namespace {

TEST(SessionControlCodecTest, RoundTripRequestAndAck) {
  SessionRekeyMessage request;
  request.kind = SessionControlKind::RekeyRequest;
  request.target_epoch = 2;
  auto encoded = SessionControlCodec::Encode(request);
  ASSERT_TRUE(static_cast<bool>(encoded));
  EXPECT_TRUE(SessionControlCodec::LooksLike(*encoded));
  auto decoded = SessionControlCodec::Decode(*encoded);
  ASSERT_TRUE(static_cast<bool>(decoded));
  EXPECT_EQ(decoded->kind, SessionControlKind::RekeyRequest);
  EXPECT_EQ(decoded->target_epoch, 2u);

  SessionRekeyMessage ack;
  ack.kind = SessionControlKind::RekeyAck;
  ack.target_epoch = 2;
  auto ack_wire = SessionControlCodec::Encode(ack);
  ASSERT_TRUE(static_cast<bool>(ack_wire));
  auto ack_decoded = SessionControlCodec::Decode(*ack_wire);
  ASSERT_TRUE(static_cast<bool>(ack_decoded));
  EXPECT_EQ(ack_decoded->kind, SessionControlKind::RekeyAck);
}

TEST(SessionControlCodecTest, DistinguishesFromCapabilityVersionOne) {
  const std::vector<uint8_t> cap_like = {1, 0, 0, 0, 0};
  EXPECT_FALSE(SessionControlCodec::LooksLike(cap_like));
}

TEST(SessionControlCodecTest, RejectBadLength) {
  const std::vector<uint8_t> short_wire = {kSessionControlWireVersion, 1, 0, 0, 0};
  EXPECT_FALSE(static_cast<bool>(SessionControlCodec::Decode(short_wire)));
  const std::vector<uint8_t> long_wire = {kSessionControlWireVersion, 1, 0, 0, 0, 0, 0};
  EXPECT_FALSE(static_cast<bool>(SessionControlCodec::Decode(long_wire)));
}

TEST(SessionControlCodecTest, RejectBadVersionAndKind) {
  const std::vector<uint8_t> bad_version = {1, 1, 0, 0, 0, 0};
  EXPECT_FALSE(static_cast<bool>(SessionControlCodec::Decode(bad_version)));
  const std::vector<uint8_t> bad_kind = {kSessionControlWireVersion, 9, 0, 0, 0, 0};
  EXPECT_FALSE(static_cast<bool>(SessionControlCodec::Decode(bad_kind)));
}

TEST(SessionControlCodecTest, LooksLikeRequiresVersionTwoAndKnownKind) {
  const std::vector<uint8_t> bad_kind = {kSessionControlWireVersion, 9, 0, 0, 0, 0};
  const std::vector<uint8_t> request = {kSessionControlWireVersion, 1, 0, 0, 0, 0};
  const std::vector<uint8_t> ack = {kSessionControlWireVersion, 2, 0, 0, 0, 0};
  EXPECT_FALSE(SessionControlCodec::LooksLike(bad_kind));
  EXPECT_TRUE(SessionControlCodec::LooksLike(request));
  EXPECT_TRUE(SessionControlCodec::LooksLike(ack));
}

} // namespace
} // namespace pp::amp
