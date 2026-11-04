#include "amp/L1/Clock.h"
#include "amp/L1/Connection.h"
#include "amp/L1/Endpoint.h"
#include "amp/L1/MemoryDatagramIo.h"
#include "amp/L1/Types.h"
#include "amp/L1/WireCodec.h"

#include <gtest/gtest.h>
#include <sodium.h>

#include <vector>

namespace pp::adp {
namespace {

struct Pair {
  std::shared_ptr<VirtualClock> clock = std::make_shared<VirtualClock>(1'000'000);
  std::shared_ptr<MemoryDatagramHub> hub = MemoryDatagramIo::MakeHub();
  IpEndpoint addr_a = IpEndpoint::V4(10, 0, 0, 1, 1000);
  IpEndpoint addr_b = IpEndpoint::V4(10, 0, 0, 2, 2000);
  /** A raw socket: a peer of another wire version, or a forger. */
  std::shared_ptr<MemoryDatagramIo> io_a = std::make_shared<MemoryDatagramIo>(hub, addr_a);
  std::shared_ptr<MemoryDatagramIo> io_b = std::make_shared<MemoryDatagramIo>(hub, addr_b);
  std::unique_ptr<Endpoint> ep_b = std::make_unique<Endpoint>(io_b, clock);
};

/** A datagram of wire version `version`, `size` bytes, its bytes after the first counting up. */
std::vector<uint8_t> Foreign(const uint8_t version, const size_t size) {
  std::vector<uint8_t> d(size);
  d[0] = static_cast<uint8_t>((version << 4) | 0x05);
  for (size_t i = 1; i < size; ++i) {
    d[i] = static_cast<uint8_t>(i);
  }
  return d;
}

std::vector<std::vector<uint8_t>> Drain(MemoryDatagramIo& io) {
  std::vector<std::vector<uint8_t>> out;
  while (true) {
    auto got = io.RecvFrom();
    if (!got || !got->has_value()) {
      return out;
    }
    out.push_back((*got)->second);
  }
}

class VersionNegotiationTest : public ::testing::Test {
protected:
  void SetUp() override { ASSERT_GE(sodium_init(), 0); }
};

// N032: the layout every wire version keeps — marker, a 16 B echo, the count, the versions.
TEST_F(VersionNegotiationTest, GoldenLayoutAndRoundTrip) {
  const auto received = Foreign(3, 40);
  const uint8_t versions[] = {2};
  const auto reply = WireCodec::EncodeVersionNegotiation(received, versions);
  std::vector<uint8_t> expected = {0xF0};
  expected.insert(expected.end(), received.begin(), received.begin() + kVersionNegotiationEchoBytes);
  expected.push_back(1);
  expected.push_back(2);
  EXPECT_EQ(reply, expected);

  auto decoded = WireCodec::DecodeVersionNegotiation(reply);
  ASSERT_TRUE(decoded) << decoded.error().message;
  EXPECT_TRUE(std::equal(decoded->echo.begin(), decoded->echo.end(), received.begin()));
  EXPECT_EQ(decoded->versions, std::vector<uint8_t>{2});

  const auto short_echo = WireCodec::EncodeVersionNegotiation(std::vector<uint8_t>{0x31, 0x02}, versions);
  EXPECT_EQ(short_echo[2], 0x02);
  EXPECT_EQ(short_echo[3], 0x00) << "a short datagram's echo is zero padded";
}

TEST_F(VersionNegotiationTest, MalformedRepliesAreRefused) {
  const uint8_t versions[] = {2, 3};
  auto reply = WireCodec::EncodeVersionNegotiation(Foreign(3, 40), versions);
  auto bad = reply;
  bad[0] = 0x20;
  EXPECT_FALSE(WireCodec::DecodeVersionNegotiation(bad)) << "not the marker";
  bad = reply;
  bad.pop_back();
  EXPECT_FALSE(WireCodec::DecodeVersionNegotiation(bad)) << "shorter than its count";
  bad = reply;
  bad[1 + kVersionNegotiationEchoBytes] = 0;
  bad.resize(2 + kVersionNegotiationEchoBytes);
  EXPECT_FALSE(WireCodec::DecodeVersionNegotiation(bad)) << "no versions";
}

// A node that takes dials tells a dialer of another version which one it speaks, echoing the dialer's packet.
TEST_F(VersionNegotiationTest, AnotherVersionsPacketIsAnsweredWithOurVersion) {
  Pair p;
  p.ep_b->SetAcceptEnabled(true);
  const auto hello = Foreign(3, 60);
  ASSERT_TRUE(p.io_a->SendTo(p.addr_b, hello));
  p.ep_b->Pump();
  const auto replies = Drain(*p.io_a);
  ASSERT_EQ(replies.size(), 1u);
  auto decoded = WireCodec::DecodeVersionNegotiation(replies[0]);
  ASSERT_TRUE(decoded) << decoded.error().message;
  EXPECT_EQ(decoded->versions, std::vector<uint8_t>{kWireVersion});
  EXPECT_TRUE(std::equal(decoded->echo.begin(), decoded->echo.end(), hello.begin()));
  // Wire v1 put the version in the whole first byte: its high nibble is 0, answered the same.
  ASSERT_TRUE(p.io_a->SendTo(p.addr_b, Foreign(0, 60)));
  p.ep_b->Pump();
  EXPECT_EQ(Drain(*p.io_a).size(), 1u);
}

// The reply answers unauthenticated datagrams, so it never amplifies, loops or floods.
TEST_F(VersionNegotiationTest, RepliesAreBoundedLoopFreeAndOnlyFromANodeTakingDials) {
  Pair p;
  ASSERT_TRUE(p.io_a->SendTo(p.addr_b, Foreign(3, 60)));
  p.ep_b->Pump();
  EXPECT_TRUE(Drain(*p.io_a).empty()) << "an endpoint that takes no dials says nothing";

  p.ep_b->SetAcceptEnabled(true);
  ASSERT_TRUE(p.io_a->SendTo(p.addr_b, Foreign(3, 10)));
  p.ep_b->Pump();
  EXPECT_TRUE(Drain(*p.io_a).empty()) << "a reply larger than the datagram it answers is not sent";

  const uint8_t versions[] = {3};
  ASSERT_TRUE(p.io_a->SendTo(p.addr_b, WireCodec::EncodeVersionNegotiation(Foreign(2, 60), versions)));
  p.ep_b->Pump();
  EXPECT_TRUE(Drain(*p.io_a).empty()) << "a negotiation reply is never answered";

  for (int i = 0; i < 3 * kVersionNegotiationPerSecond; ++i) {
    ASSERT_TRUE(p.io_a->SendTo(p.addr_b, Foreign(3, 60)));
  }
  p.ep_b->Pump(1000);
  EXPECT_EQ(Drain(*p.io_a).size(), static_cast<size_t>(kVersionNegotiationPerSecond)) << "rate limited";
  p.clock->Advance(1000);
  ASSERT_TRUE(p.io_a->SendTo(p.addr_b, Foreign(3, 60)));
  p.ep_b->Pump();
  EXPECT_EQ(Drain(*p.io_a).size(), 1u) << "the next second has room again";
}

// Only a dialer still handshaking takes a reply naming its association; it learns the peer's versions.
TEST_F(VersionNegotiationTest, OnlyAnUnauthenticatedDialerTakesTheReply) {
  Pair p;
  Endpoint ep_a(p.io_a, p.clock);
  OpenParams dial;
  dial.key.bytes.fill(9);
  dial.peer = p.addr_b;
  dial.reduce_rtx_until_authenticated = true;
  dial.dialer_backoff = true;
  dial.mint_id = false;
  dial.id.bytes.fill(1);
  auto conn = ep_a.Open(dial);
  ASSERT_TRUE(conn) << conn.error().message;
  OpenParams plain = dial;
  plain.dialer_backoff = false;
  plain.id.bytes.fill(2);
  auto other = ep_a.Open(plain);
  ASSERT_TRUE(other) << other.error().message;

  const auto reply_for = [&](const std::shared_ptr<Connection>& c, std::vector<uint8_t> versions) {
    std::vector<uint8_t> ours(1 + kAssocIdBytes, 0);
    ours[0] = static_cast<uint8_t>(kWireVersion << 4);
    const AssocId id = c->Id(); // by value: one copy for both iterators
    std::copy(id.bytes.begin(), id.bytes.end(), ours.begin() + 1);
    return WireCodec::EncodeVersionNegotiation(ours, versions);
  };

  ASSERT_TRUE(p.io_b->SendTo(p.addr_a, reply_for(*conn, {kWireVersion})));
  ep_a.Pump();
  EXPECT_FALSE((*conn)->PeerVersionMismatch()) << "a reply listing our own version is not about us";

  ASSERT_TRUE(p.io_b->SendTo(p.addr_a, reply_for(*other, {3})));
  ep_a.Pump();
  EXPECT_FALSE((*other)->PeerVersionMismatch()) << "not this side's dial";

  ASSERT_TRUE(p.io_b->SendTo(p.addr_a, reply_for(*conn, {3, 4})));
  ep_a.Pump();
  EXPECT_TRUE((*conn)->PeerVersionMismatch());
  EXPECT_EQ((*conn)->PeerWireVersions(), (std::vector<uint8_t>{3, 4}));
  EXPECT_TRUE((*conn)->IsClosed());
}

} // namespace
} // namespace pp::adp
