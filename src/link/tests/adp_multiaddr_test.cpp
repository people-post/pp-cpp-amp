#include "amp/link/AdpMultiaddr.h"

#include <gtest/gtest.h>

namespace pp::amp {
namespace {

TEST(AdpMultiaddrTest, ParseAndFormatRoundTripIpv4) {
  const std::string multiaddr = "/ip4/127.0.0.1/udp/4001/adp/1.0.0/p2p/QmTestPeer";
  auto parsed = ParseAdpMultiaddr(multiaddr);
  ASSERT_TRUE(static_cast<bool>(parsed));
  EXPECT_EQ(parsed->endpoint.family, adp::IpEndpoint::Family::V4);
  EXPECT_EQ(parsed->endpoint.port, 4001u);
  EXPECT_EQ(parsed->peer_id, "QmTestPeer");

  auto formatted = FormatAdpMultiaddr(parsed->endpoint, parsed->peer_id);
  ASSERT_TRUE(static_cast<bool>(formatted));
  EXPECT_EQ(*formatted, multiaddr);
}

TEST(AdpMultiaddrTest, ParseAndFormatRoundTripIpv6) {
  const std::string multiaddr = "/ip6/2001:db8::1/udp/4001/adp/1.0.0/p2p/QmTestPeer6";
  auto parsed = ParseAdpMultiaddr(multiaddr);
  ASSERT_TRUE(static_cast<bool>(parsed));
  EXPECT_EQ(parsed->endpoint.family, adp::IpEndpoint::Family::V6);
  EXPECT_EQ(parsed->endpoint.port, 4001u);
  EXPECT_EQ(parsed->peer_id, "QmTestPeer6");

  auto formatted = FormatAdpMultiaddr(parsed->endpoint, parsed->peer_id);
  ASSERT_TRUE(static_cast<bool>(formatted));
  // inet_ntop may expand or compress; re-parse must be equal.
  auto again = ParseAdpMultiaddr(*formatted);
  ASSERT_TRUE(static_cast<bool>(again));
  EXPECT_EQ(again->endpoint, parsed->endpoint);
  EXPECT_EQ(again->peer_id, parsed->peer_id);
}

TEST(AdpMultiaddrTest, ParseIpv6BracketedHost) {
  auto parsed = ParseAdpMultiaddr("/ip6/[::1]/udp/18517/adp/1.0.0/p2p/QmLoop");
  ASSERT_TRUE(static_cast<bool>(parsed));
  EXPECT_EQ(parsed->endpoint.family, adp::IpEndpoint::Family::V6);
  EXPECT_EQ(parsed->endpoint.port, 18517u);
}

TEST(AdpMultiaddrTest, RejectIpv6ZoneId) {
  auto parsed = ParseAdpMultiaddr("/ip6/fe80::1%eth0/udp/18517/adp/1.0.0/p2p/QmLinkLocal");
  EXPECT_FALSE(static_cast<bool>(parsed));
}

} // namespace
} // namespace pp::amp
