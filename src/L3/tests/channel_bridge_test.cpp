#include "amp/L3/AmpChannelLimits.h"
#include "amp/L3/ChannelBridge.h"
#include "amp/L3/ChannelPolicy.h"
#include "amp_test_relay.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace pp::amp {
namespace {

/** What a spliced tunnel carries after its handshake: opaque data, never dropped. */
ChannelPolicy TunnelPolicy() {
  ChannelPolicy policy;
  policy.cls = ChannelClass::Bulk;
  policy.drop = ChannelDropPolicy::Never;
  policy.max_outbound_frames = AmpChannelLimits::kMaxControlOutboundFrames;
  policy.max_message_bytes = AmpChannelLimits::kMaxBulkFrameBytes;
  return policy;
}

// A relay splices a dialer's channel to a target's: DATA flows both ways and is counted.
TEST(ChannelBridgeTest, ForwardsBothWaysAndCountsTheBytes) {
  auto created = test::AmpTestRelayPath::Create("/pp-browser/circuit-relay/1.0.0", TunnelPolicy());
  ASSERT_TRUE(static_cast<bool>(created)) << created.error().message;
  auto& path = **created;
  ASSERT_EQ(path.TargetMux().State(path.far_channel), ChannelState::Open) << "after create";

  std::vector<uint8_t> at_target;
  std::vector<uint8_t> at_dialer;
  path.TargetMux().SetDataHandler(path.far_channel, [&](uint32_t, std::vector<uint8_t> payload) {
    at_target = std::move(payload);
  });
  path.DialerMux().SetDataHandler(path.near_channel, [&](uint32_t, std::vector<uint8_t> payload) {
    at_dialer = std::move(payload);
  });

  std::string target_closed;
  path.TargetMux().SetTerminalHandler(path.far_channel, [&](uint32_t, const char* why) { target_closed = why; });
  ChannelBridge bridge;
  bool closed = false;
  bridge.Attach(path.relay_near, path.relay_far, {}, [&]() { closed = true; });

  const std::vector<uint8_t> hello = {'h', 'e', 'l', 'l', 'o'};
  ASSERT_TRUE(static_cast<bool>(path.DialerMux().SendData(path.near_channel, hello)));
  EXPECT_EQ(at_target, hello);
  ASSERT_EQ(path.TargetMux().State(path.far_channel), ChannelState::Open) << "target far channel closed: " << target_closed;
  ASSERT_EQ(path.DialerMux().State(path.near_channel), ChannelState::Open) << "dialer near channel";
  const std::vector<uint8_t> back = {'o', 'k'};
  auto sent_back = path.TargetMux().SendData(path.far_channel, back);
  ASSERT_TRUE(static_cast<bool>(sent_back)) << sent_back.error().message;
  EXPECT_EQ(at_dialer, back);

  EXPECT_EQ(bridge.ForwardedBytes(), hello.size() + back.size());
  EXPECT_FALSE(closed);
  bridge.Stop();
}

} // namespace
} // namespace pp::amp
