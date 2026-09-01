#include "amp/L1/HmacBinder.h"
#include "amp/L1/Types.h"
#include "amp/L1/WireCodec.h"

#include <gtest/gtest.h>
#include <sodium.h>

#include <string>

namespace {

pp::adp::PeerKey TestKey(uint8_t fill = 0x42) {
  pp::adp::PeerKey k;
  k.bytes.fill(fill);
  return k;
}

pp::adp::AssocId TestAssoc(uint8_t fill = 0x11) {
  pp::adp::AssocId id;
  id.bytes.fill(fill);
  return id;
}

class AdpWireTest : public ::testing::Test {
protected:
  void SetUp() override { ASSERT_GE(sodium_init(), 0); }
};

TEST_F(AdpWireTest, RoundTripEmptyPayload) {
  pp::adp::WirePacket pkt;
  pkt.type = pp::adp::PacketType::DataBestEffort;
  pkt.assoc = TestAssoc();
  pkt.seq = 1;
  pkt.timestamp_ms = 123456789u;
  auto enc = pp::adp::WireCodec::Encode(pkt);
  ASSERT_TRUE(enc);
  auto dec = pp::adp::WireCodec::Decode(
      std::vector<uint8_t>(enc->begin(), enc->end())); // no hmac yet — need pad?
  // Decode expects trailing hmac bytes in size check — encode alone is header+payload.
  // Append fake hmac for decode length, then test seal path separately.
  auto sealed = pp::adp::HmacBinder(TestKey()).Seal(*enc);
  ASSERT_TRUE(sealed);
  auto got = pp::adp::WireCodec::Decode(*sealed);
  ASSERT_TRUE(got);
  EXPECT_EQ(got->seq, 1u);
  EXPECT_EQ(got->timestamp_ms, 123456789u);
  EXPECT_TRUE(got->payload.empty());
  EXPECT_EQ(got->assoc.bytes, TestAssoc().bytes);
}

TEST_F(AdpWireTest, RoundTripMaxPayload) {
  pp::adp::WirePacket pkt;
  pkt.type = pp::adp::PacketType::DataReliable;
  pkt.assoc = TestAssoc(0x22);
  pkt.seq = 99;
  pkt.timestamp_ms = 1;
  pkt.payload.assign(pp::adp::kMaxPayload, 0xab);
  auto enc = pp::adp::WireCodec::Encode(pkt);
  ASSERT_TRUE(enc);
  auto sealed = pp::adp::HmacBinder(TestKey()).Seal(*enc);
  ASSERT_TRUE(sealed);
  auto got = pp::adp::WireCodec::Decode(*sealed);
  ASSERT_TRUE(got);
  EXPECT_EQ(got->payload.size(), pp::adp::kMaxPayload);
}

TEST_F(AdpWireTest, RejectOversizePayload) {
  pp::adp::WirePacket pkt;
  pkt.payload.assign(static_cast<size_t>(pp::adp::kMaxPayload) + 1, 1);
  EXPECT_FALSE(pp::adp::WireCodec::Encode(pkt));
}

TEST_F(AdpWireTest, RejectUnknownVersion) {
  pp::adp::WirePacket pkt;
  pkt.seq = 1;
  auto enc = pp::adp::WireCodec::Encode(pkt);
  ASSERT_TRUE(enc);
  auto sealed = pp::adp::HmacBinder(TestKey()).Seal(*enc);
  ASSERT_TRUE(sealed);
  (*sealed)[0] = 99;
  // HMAC will fail if we verify; decode checks version before caring about hmac body length.
  // Fix length: version byte changed but length ok.
  auto got = pp::adp::WireCodec::Decode(*sealed);
  EXPECT_FALSE(got);
}

TEST_F(AdpWireTest, RejectTruncated) {
  std::vector<uint8_t> tiny{1, 2, 3};
  EXPECT_FALSE(pp::adp::WireCodec::Decode(tiny));
}

TEST_F(AdpWireTest, RejectBadPacketType) {
  pp::adp::WirePacket pkt;
  pkt.type = pp::adp::PacketType::DataBestEffort;
  pkt.seq = 1;
  auto enc = pp::adp::WireCodec::Encode(pkt);
  ASSERT_TRUE(enc);
  auto sealed = pp::adp::HmacBinder(TestKey()).Seal(*enc);
  ASSERT_TRUE(sealed);
  (*sealed)[1] = 99;
  EXPECT_FALSE(pp::adp::WireCodec::Decode(*sealed));
}

TEST_F(AdpWireTest, RejectLengthMismatch) {
  pp::adp::WirePacket pkt;
  pkt.type = pp::adp::PacketType::DataBestEffort;
  pkt.seq = 1;
  pkt.payload = {1, 2, 3};
  auto enc = pp::adp::WireCodec::Encode(pkt);
  ASSERT_TRUE(enc);
  auto sealed = pp::adp::HmacBinder(TestKey()).Seal(*enc);
  ASSERT_TRUE(sealed);
  // Claim zero payload bytes while body still contains 3 bytes.
  (*sealed)[26] = 0;
  (*sealed)[27] = 0;
  EXPECT_FALSE(pp::adp::WireCodec::Decode(*sealed));
}

TEST_F(AdpWireTest, GoldenHeaderLayoutLittleEndian) {
  pp::adp::WirePacket pkt;
  pkt.type = pp::adp::PacketType::Ack;
  pkt.assoc = TestAssoc(0x01);
  pkt.seq = 0x04030201u;
  pkt.timestamp_ms = 0x08070605u;
  auto enc = pp::adp::WireCodec::Encode(pkt);
  ASSERT_TRUE(enc);
  ASSERT_EQ(enc->size(), pp::adp::kHeaderBytes);
  EXPECT_EQ((*enc)[0], 1);
  EXPECT_EQ((*enc)[1], static_cast<uint8_t>(pp::adp::PacketType::Ack));
  EXPECT_EQ((*enc)[18], 0x01);
  EXPECT_EQ((*enc)[19], 0x02);
  EXPECT_EQ((*enc)[20], 0x03);
  EXPECT_EQ((*enc)[21], 0x04);
  EXPECT_EQ((*enc)[22], 0x05);
  EXPECT_EQ((*enc)[23], 0x06);
  EXPECT_EQ((*enc)[24], 0x07);
  EXPECT_EQ((*enc)[25], 0x08);
  EXPECT_EQ((*enc)[26], 0);
  EXPECT_EQ((*enc)[27], 0);
}

TEST_F(AdpWireTest, HmacAcceptsAndRejectsBitFlip) {
  pp::adp::WirePacket pkt;
  pkt.seq = 7;
  pkt.assoc = TestAssoc();
  pkt.payload = {1, 2, 3};
  auto enc = pp::adp::WireCodec::Encode(pkt);
  ASSERT_TRUE(enc);
  pp::adp::HmacBinder binder(TestKey());
  auto sealed = binder.Seal(*enc);
  ASSERT_TRUE(sealed);
  EXPECT_TRUE(binder.Verify(*sealed));
  (*sealed)[10] ^= 0xff;
  EXPECT_FALSE(binder.Verify(*sealed));
}

TEST_F(AdpWireTest, HmacWrongKey) {
  pp::adp::WirePacket pkt;
  pkt.seq = 1;
  auto enc = pp::adp::WireCodec::Encode(pkt);
  ASSERT_TRUE(enc);
  auto sealed = pp::adp::HmacBinder(TestKey(1)).Seal(*enc);
  ASSERT_TRUE(sealed);
  EXPECT_FALSE(pp::adp::HmacBinder(TestKey(2)).Verify(*sealed));
}

TEST_F(AdpWireTest, HmacTagFlipRejected) {
  pp::adp::WirePacket pkt;
  pkt.seq = 1;
  auto enc = pp::adp::WireCodec::Encode(pkt);
  ASSERT_TRUE(enc);
  pp::adp::HmacBinder binder(TestKey());
  auto sealed = binder.Seal(*enc);
  ASSERT_TRUE(sealed);
  sealed->back() ^= 1;
  EXPECT_FALSE(binder.Verify(*sealed));
}

} // namespace
