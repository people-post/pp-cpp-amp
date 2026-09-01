#include "amp/link/AdpMultiaddr.h"

#include <gtest/gtest.h>

namespace pp::amp {
namespace {

TEST(AdpMultiaddrTest, ParseAndFormatRoundTrip) {
  const std::string multiaddr = "/ip4/127.0.0.1/udp/4001/adp/1.0.0/p2p/QmTestPeer";
  auto parsed = ParseAdpMultiaddr(multiaddr);
  ASSERT_TRUE(static_cast<bool>(parsed));
  EXPECT_EQ(parsed->endpoint.port, 4001u);
  EXPECT_EQ(parsed->peer_id, "QmTestPeer");

  auto formatted = FormatAdpMultiaddr(parsed->endpoint, parsed->peer_id);
  ASSERT_TRUE(static_cast<bool>(formatted));
  EXPECT_EQ(*formatted, multiaddr);
}

} // namespace
} // namespace pp::amp
