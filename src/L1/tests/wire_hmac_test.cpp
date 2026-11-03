#include "amp/L1/HmacBinder.h"
#include "amp/L1/PacketProtection.h"
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
  pkt.type = pp::adp::PacketType::PlainBestEffort;
  pkt.assoc = TestAssoc();
  pkt.seq = 1;
  pkt.timestamp_ms = 123456789u;
  auto enc = pp::adp::WireCodec::Encode(pkt);
  ASSERT_TRUE(enc);
  auto sealed = pp::adp::HmacBinder(TestKey()).Seal(*enc);
  ASSERT_TRUE(sealed);
  auto got = pp::adp::WireCodec::Decode(*sealed);
  ASSERT_TRUE(got);
  EXPECT_EQ(got->type, pp::adp::PacketType::PlainBestEffort);
  EXPECT_EQ(got->seq, 1u);
  EXPECT_EQ(got->timestamp_ms, 123456789u);
  EXPECT_TRUE(got->payload.empty());
  EXPECT_EQ(got->assoc.bytes, TestAssoc().bytes);
}

// The largest payload fills the largest datagram exactly: IPv6's smallest MTU less its headers (A029 H1).
TEST_F(AdpWireTest, RoundTripMaxPayloadFillsTheLargestDatagram) {
  pp::adp::WirePacket pkt;
  pkt.type = pp::adp::PacketType::PlainReliable;
  pkt.assoc = TestAssoc(0x22);
  pkt.seq = 99;
  pkt.timestamp_ms = 1;
  pkt.payload.assign(pp::adp::kMaxPayload, 0xab);
  auto enc = pp::adp::WireCodec::Encode(pkt);
  ASSERT_TRUE(enc);
  auto sealed = pp::adp::HmacBinder(TestKey()).Seal(*enc);
  ASSERT_TRUE(sealed);
  EXPECT_EQ(sealed->size(), pp::adp::kMaxDatagramBytes);
  EXPECT_EQ(pp::adp::kMaxDatagramBytes, 1280u - 40u - 8u);
  auto got = pp::adp::WireCodec::Decode(*sealed);
  ASSERT_TRUE(got);
  EXPECT_EQ(got->payload.size(), pp::adp::kMaxPayload);
}

TEST_F(AdpWireTest, RejectOversizePayload) {
  pp::adp::WirePacket pkt;
  pkt.payload.assign(static_cast<size_t>(pp::adp::kMaxPayload) + 1, 1);
  EXPECT_FALSE(pp::adp::WireCodec::Encode(pkt));
}

// An older peer's packet (wire v1: version byte 1) is refused, not misread.
TEST_F(AdpWireTest, RejectUnknownVersion) {
  pp::adp::WirePacket pkt;
  pkt.seq = 1;
  auto enc = pp::adp::WireCodec::Encode(pkt);
  ASSERT_TRUE(enc);
  auto sealed = pp::adp::HmacBinder(TestKey()).Seal(*enc);
  ASSERT_TRUE(sealed);
  for (const uint8_t first : {uint8_t{0x01}, uint8_t{0x15}, uint8_t{0x35}}) {
    (*sealed)[0] = first;
    EXPECT_FALSE(pp::adp::WireCodec::Decode(*sealed)) << int(first);
    EXPECT_FALSE(pp::adp::WireCodec::PeekType(*sealed)) << int(first);
  }
}

TEST_F(AdpWireTest, RejectTruncated) {
  std::vector<uint8_t> tiny{1, 2, 3};
  EXPECT_FALSE(pp::adp::WireCodec::Decode(tiny));
  EXPECT_FALSE(pp::adp::WireCodec::DecodeSealedHeader(tiny));
}

TEST_F(AdpWireTest, RejectBadPacketType) {
  pp::adp::WirePacket pkt;
  pkt.type = pp::adp::PacketType::PlainBestEffort;
  pkt.seq = 1;
  auto enc = pp::adp::WireCodec::Encode(pkt);
  ASSERT_TRUE(enc);
  auto sealed = pp::adp::HmacBinder(TestKey()).Seal(*enc);
  ASSERT_TRUE(sealed);
  (*sealed)[0] = static_cast<uint8_t>((pp::adp::kWireVersion << 4) | 0x0f);
  EXPECT_FALSE(pp::adp::WireCodec::Decode(*sealed));
}

// Data is sealed by the session: it has no plain encoding to fall back to, either way.
TEST_F(AdpWireTest, SealedTypesHaveNoPlainEncoding) {
  pp::adp::WirePacket pkt;
  pkt.type = pp::adp::PacketType::DataReliable;
  EXPECT_FALSE(pp::adp::WireCodec::Encode(pkt));
  pkt.type = pp::adp::PacketType::PlainReliable;
  auto enc = pp::adp::WireCodec::Encode(pkt);
  ASSERT_TRUE(enc);
  auto maced = pp::adp::HmacBinder(TestKey()).Seal(*enc);
  ASSERT_TRUE(maced);
  (*maced)[0] = static_cast<uint8_t>((pp::adp::kWireVersion << 4) | uint8_t(pp::adp::PacketType::DataReliable));
  EXPECT_FALSE(pp::adp::WireCodec::Decode(*maced));
}

TEST_F(AdpWireTest, GoldenHeaderLayoutLittleEndian) {
  pp::adp::WirePacket pkt;
  pkt.type = pp::adp::PacketType::Ack;
  pkt.assoc = TestAssoc(0x01);
  pkt.seq = 0x04030201u;
  pkt.timestamp_ms = 0x08070605u;
  auto enc = pp::adp::WireCodec::Encode(pkt);
  ASSERT_TRUE(enc);
  const std::vector<uint8_t> expected = {0x22, 1, 1, 1, 1, 1, 1, 1, 1, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
  EXPECT_EQ(*enc, expected);
  EXPECT_EQ(enc->size(), pp::adp::kHeaderBytes);
}

TEST_F(AdpWireTest, GoldenSealedHeaderLayout) {
  pp::adp::SealedHeader header;
  header.type = pp::adp::PacketType::DataReliable;
  header.assoc = TestAssoc(0x01);
  header.number = 0x04030201u;
  const auto enc = pp::adp::WireCodec::EncodeSealedHeader(header);
  const std::array<uint8_t, pp::adp::kSealedHeaderBytes> expected = {0x21, 1, 1,    1,    1,    1,   1,
                                                                     1,    1, 0x01, 0x02, 0x03, 0x04};
  EXPECT_EQ(enc, expected);

  std::vector<uint8_t> datagram(enc.begin(), enc.end());
  datagram.resize(datagram.size() + pp::adp::kPacketTagBytes, 0xee);
  auto got = pp::adp::WireCodec::DecodeSealedHeader(datagram);
  ASSERT_TRUE(got);
  EXPECT_EQ(got->type, pp::adp::PacketType::DataReliable);
  EXPECT_EQ(got->number, 0x04030201u);
  EXPECT_EQ(pp::adp::WireCodec::PeekAssoc(datagram).bytes, TestAssoc(0x01).bytes);
  datagram.pop_back();
  EXPECT_FALSE(pp::adp::WireCodec::DecodeSealedHeader(datagram)) << "no room for the tag";
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
