#include "amp/link/CarrierLane.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace pp::amp {
namespace {

std::vector<uint8_t> Bytes(const std::string& s) { return {s.begin(), s.end()}; }

/** Seq + inner of a LaneData wire. */
std::pair<uint32_t, std::vector<uint8_t>> Unwrap(const std::vector<uint8_t>& wire) {
  auto data = AmpAdpCarrier::DecodeLaneData(wire);
  EXPECT_TRUE(static_cast<bool>(data));
  return {data->first, {data->second.begin(), data->second.end()}};
}

TEST(CarrierLaneTest, LaneFramesRoundTrip) {
  const auto data = AmpAdpCarrier::EncodeLaneData(7, Bytes("abc"));
  auto [seq, inner] = Unwrap(data);
  EXPECT_EQ(seq, 7U);
  EXPECT_EQ(inner, Bytes("abc"));
  const auto ack = AmpAdpCarrier::EncodeLaneAck({5, 0x8000000000000001ULL});
  auto decoded = AmpAdpCarrier::DecodeLaneAck(ack);
  ASSERT_TRUE(static_cast<bool>(decoded));
  EXPECT_EQ(decoded->cumulative, 5U);
  EXPECT_EQ(decoded->selective, 0x8000000000000001ULL);
}

TEST(CarrierLaneTest, OutOfOrderFramesAreReleasedInOrderOnce) {
  CarrierLane rx;
  auto r3 = rx.OnData(3, Bytes("c"));
  EXPECT_TRUE(r3.deliver.empty());
  EXPECT_EQ(r3.ack.cumulative, 0U);
  EXPECT_EQ(r3.ack.selective, 0b10U) << "seq 3 = cumulative + 3 → bit 1";
  auto r1 = rx.OnData(1, Bytes("a"));
  ASSERT_EQ(r1.deliver.size(), 1U);
  EXPECT_EQ(r1.deliver[0], Bytes("a"));
  EXPECT_EQ(r1.ack.cumulative, 1U);
  EXPECT_EQ(r1.ack.selective, 0b1U);
  auto r2 = rx.OnData(2, Bytes("b"));
  ASSERT_EQ(r2.deliver.size(), 2U);
  EXPECT_EQ(r2.deliver[0], Bytes("b"));
  EXPECT_EQ(r2.deliver[1], Bytes("c"));
  EXPECT_EQ(r2.ack.cumulative, 3U);
  auto dup = rx.OnData(2, Bytes("b"));
  EXPECT_TRUE(dup.deliver.empty()) << "a duplicate is not delivered again";
  EXPECT_EQ(dup.ack.cumulative, 3U) << "but acked again (our last ack may be lost)";
}

TEST(CarrierLaneTest, UnackedFrameIsResentAfterTheRtoWithBackoff) {
  CarrierLane tx(CarrierLaneConfig{.initial_rto_ms = 500});
  auto wire = tx.Wrap(Bytes("a"), 1000);
  ASSERT_TRUE(wire.has_value());
  EXPECT_TRUE(tx.DueResends(1499).empty());
  auto first = tx.DueResends(1500);
  ASSERT_EQ(first.size(), 1U);
  EXPECT_EQ(first[0], *wire);
  EXPECT_TRUE(tx.DueResends(2499).empty()) << "second resend waits 2 × RTO";
  EXPECT_EQ(tx.DueResends(2500).size(), 1U);
  tx.OnAck({1, 0}, 2600);
  EXPECT_EQ(tx.InFlight(), 0U);
  EXPECT_TRUE(tx.DueResends(100000).empty());
}

TEST(CarrierLaneTest, SelectiveAcksTriggerAFastResendOfTheHole) {
  CarrierLane tx(CarrierLaneConfig{.initial_rto_ms = 1000, .fast_retransmit_skips = 3});
  for (int i = 0; i < 5; ++i) {
    ASSERT_TRUE(tx.Wrap(Bytes("x"), 0).has_value());
  }
  // Seq 1 lost; 2, 3, 4 arrive one by one.
  tx.OnAck({0, 0b1}, 10);
  tx.OnAck({0, 0b11}, 11);
  EXPECT_TRUE(tx.DueResends(12).empty());
  tx.OnAck({0, 0b111}, 12);
  auto resent = tx.DueResends(12);
  ASSERT_EQ(resent.size(), 1U) << "only the hole, long before its RTO";
  EXPECT_EQ(Unwrap(resent[0]).first, 1U);
  EXPECT_EQ(tx.InFlight(), 2U) << "seq 1 and 5 remain";
}

TEST(CarrierLaneTest, RtoFollowsTheMeasuredRoundTrip) {
  CarrierLane tx(CarrierLaneConfig{.initial_rto_ms = 1000, .min_rto_ms = 50, .max_rto_ms = 3000});
  for (int i = 0; i < 8; ++i) {
    ASSERT_TRUE(tx.Wrap(Bytes("x"), i * 1000).has_value());
    tx.OnAck({static_cast<uint32_t>(i + 1), 0}, i * 1000 + 100);
  }
  EXPECT_GE(tx.RtoMs(), 100);
  EXPECT_LT(tx.RtoMs(), 400) << "near the 100 ms round trip, not the 1 s initial guess";
}

TEST(CarrierLaneTest, FullWindowRefusesAndAnAckReopensIt) {
  CarrierLane tx(CarrierLaneConfig{.window = 2});
  ASSERT_TRUE(tx.Wrap(Bytes("a"), 0).has_value());
  ASSERT_TRUE(tx.Wrap(Bytes("b"), 0).has_value());
  EXPECT_EQ(tx.Credits(), 0U);
  EXPECT_FALSE(tx.Wrap(Bytes("c"), 0).has_value());
  tx.OnAck({1, 0}, 10);
  EXPECT_EQ(tx.Credits(), 1U);
  EXPECT_TRUE(tx.Wrap(Bytes("c"), 10).has_value());
}

TEST(CarrierLaneTest, GivesUpAfterMaxAttempts) {
  CarrierLane tx(CarrierLaneConfig{.initial_rto_ms = 100, .max_rto_ms = 100, .max_attempts = 3});
  ASSERT_TRUE(tx.Wrap(Bytes("a"), 0).has_value());
  EXPECT_EQ(tx.DueResends(100).size(), 1U);
  EXPECT_EQ(tx.DueResends(200).size(), 1U);
  EXPECT_FALSE(tx.Failed());
  EXPECT_TRUE(tx.DueResends(300).empty());
  EXPECT_TRUE(tx.Failed());
}

} // namespace
} // namespace pp::amp
