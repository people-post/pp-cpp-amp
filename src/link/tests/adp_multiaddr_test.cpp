#include "amp/link/AdpMultiaddr.h"
#include "amp/link/HostResolver.h"

#include <condition_variable>
#include <future>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

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

namespace pp::amp {
namespace {

TEST(AdpMultiaddrTest, ParsesHostnamesWithoutResolvingThem) {
  auto v4 = ParseAdpMultiaddr("/dns4/relay.example.org/udp/8519/adp/1.0.0/p2p/QmRelay");
  ASSERT_TRUE(static_cast<bool>(v4)) << v4.error().message;
  EXPECT_TRUE(v4->IsHostname());
  EXPECT_EQ(v4->hostname, "relay.example.org");
  EXPECT_EQ(v4->host_family, HostFamily::V4);
  EXPECT_EQ(v4->endpoint.port, 8519u);
  EXPECT_EQ(v4->peer_id, "QmRelay");

  auto v6 = ParseAdpMultiaddr("/dns6/beacon/udp/8517/adp/1.0.0/p2p/QmBeacon");
  ASSERT_TRUE(static_cast<bool>(v6));
  EXPECT_EQ(v6->host_family, HostFamily::V6);
  auto any = ParseAdpMultiaddr("/dns/my-relay-1/udp/8519/adp/1.0.0/p2p/QmRelay");
  ASSERT_TRUE(static_cast<bool>(any));
  EXPECT_EQ(any->host_family, HostFamily::Any);
  EXPECT_FALSE(ParseAdpMultiaddr("/ip4/10.0.0.1/udp/1/adp/1.0.0/p2p/QmX")->IsHostname());

  for (const char *bad : {"/dns4/-bad/udp/1/adp/1.0.0/p2p/QmX", "/dns4/a..b/udp/1/adp/1.0.0/p2p/QmX",
                          "/dns4/under_score/udp/1/adp/1.0.0/p2p/QmX", "/dns4/host/udp/0/adp/1.0.0/p2p/QmX",
                          "/dns4/host/udp/1/adp/1.0.0/p2p/", "/dnsaddr/host/udp/1/adp/1.0.0/p2p/QmX"}) {
    EXPECT_FALSE(static_cast<bool>(ParseAdpMultiaddr(bad))) << bad;
  }
  EXPECT_FALSE(static_cast<bool>(
      ParseAdpMultiaddr("/dns4/" + std::string(254, 'a') + "/udp/1/adp/1.0.0/p2p/QmX")));
}

TEST(HostResolverTest, ResolvesLocalhost) {
  auto ep = ResolveHostNow("localhost", HostFamily::V4, 4001);
  ASSERT_TRUE(static_cast<bool>(ep)) << ep.error().message;
  EXPECT_EQ(ep->family, adp::IpEndpoint::Family::V4);
  EXPECT_EQ(ep->addr[0], 127u);
  EXPECT_EQ(ep->port, 4001u);
  EXPECT_FALSE(static_cast<bool>(ResolveHostNow("no-such-host.invalid", HostFamily::Any, 1)));
}

TEST(HostResolverTest, DeliversThroughItsPosterAndNothingOnceGone) {
  std::mutex mu;
  std::condition_variable cv;
  std::vector<std::function<void()>> posted;
  auto post = [&](std::function<void()> task) {
    std::lock_guard lock(mu);
    posted.push_back(std::move(task));
    cv.notify_all();
  };
  std::promise<void> release;
  auto released = release.get_future().share();
  auto lookup = [released](const std::string &host, HostFamily, uint16_t port) -> Roe<adp::IpEndpoint> {
    if (host == "slow") {
      released.wait();
    }
    return adp::IpEndpoint::V4(10, 0, 0, 7, port);
  };
  std::optional<Roe<adp::IpEndpoint>> got;
  {
    HostResolver resolver(post, lookup);
    resolver.Resolve("fast", HostFamily::V4, 9, [&](Roe<adp::IpEndpoint> r) { got = std::move(r); });
    std::unique_lock lock(mu);
    ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(5), [&] { return !posted.empty(); }));
    EXPECT_FALSE(got.has_value()) << "done runs where the poster puts it, not on the lookup's thread";
    lock.unlock();
    posted.front()();
    ASSERT_TRUE(got.has_value() && got->isOk());
    EXPECT_EQ(got->value().port, 9u);
    resolver.Resolve("slow", HostFamily::V4, 9, [](Roe<adp::IpEndpoint>) { FAIL() << "delivered after the resolver went"; });
  } // gone while "slow" is still looking up
  release.set_value();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  std::lock_guard lock(mu);
  EXPECT_EQ(posted.size(), 1u);
}

} // namespace
} // namespace pp::amp
