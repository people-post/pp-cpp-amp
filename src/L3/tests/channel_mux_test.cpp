#include "amp/L3/AmpChannelLimits.h"
#include "amp/L3/ChannelPolicy.h"
#include "amp/L3/ChannelSession.h"
#include "amp_test_link.h"

#include <gtest/gtest.h>

namespace pp::amp {
namespace {

ChannelPolicy TestRealtimePolicy() {
  ChannelPolicy policy;
  policy.cls = ChannelClass::Realtime;
  policy.drop = ChannelDropPolicy::Oldest;
  policy.max_outbound_frames = AmpChannelLimits::kMaxCallMediaOutboundFrames;
  policy.write_preferred = true;
  policy.max_message_bytes = AmpChannelLimits::kMaxCallMediaFrameBytes;
  return policy;
}

ChannelPolicy TestBulkPolicy() {
  ChannelPolicy policy;
  policy.cls = ChannelClass::Bulk;
  policy.drop = ChannelDropPolicy::Never;
  policy.max_outbound_frames = AmpChannelLimits::kMaxControlOutboundFrames;
  policy.max_message_bytes = AmpChannelLimits::kMaxChatBlobFrameBytes;
  return policy;
}

TEST(ChannelMuxTest, OpenAndDataRoundTrip) {
  auto link_result = test::AmpTestLink::Create();
  ASSERT_TRUE(static_cast<bool>(link_result));
  auto& link = **link_result;

  std::vector<uint8_t> received;
  auto ch = link.initiator.mux.OpenOutbound("/pp-browser/chat/1.0.0", ControlJsonChannelPolicy());
  ASSERT_TRUE(static_cast<bool>(ch));
  EXPECT_EQ(link.initiator.mux.State(*ch), ChannelState::Open);

  link.responder.mux.SetDataHandler(*ch, [&](uint32_t, std::vector<uint8_t> payload) {
    received = std::move(payload);
  });

  const std::vector<uint8_t> msg = {'h', 'i'};
  ASSERT_TRUE(static_cast<bool>(link.initiator.mux.SendData(*ch, msg)));
  EXPECT_EQ(received, msg);
}

TEST(ChannelMuxTest, RealtimeUsesBestEffortQos) {
  auto link_result = test::AmpTestLink::Create();
  ASSERT_TRUE(static_cast<bool>(link_result));
  auto& link = **link_result;

  auto ch = link.initiator.mux.OpenOutbound("/pp-browser/call-media/1.0.0", TestRealtimePolicy());
  ASSERT_TRUE(static_cast<bool>(ch));
  ASSERT_TRUE(static_cast<bool>(link.initiator.mux.SendData(*ch, {'o'})));
  EXPECT_EQ(link.initiator.mux.LastSendQos(), adp::QosClass::BestEffort);
}

TEST(ChannelMuxTest, ResetDoesNotKillSiblingChannel) {
  auto link_result = test::AmpTestLink::Create();
  ASSERT_TRUE(static_cast<bool>(link_result));
  auto& link = **link_result;

  auto ch1 = link.initiator.mux.OpenOutbound("/pp-browser/chat/1.0.0", ControlJsonChannelPolicy());
  auto ch2 = link.initiator.mux.OpenOutbound("/pp-browser/chat-history/1.0.0", ControlJsonChannelPolicy());
  ASSERT_TRUE(static_cast<bool>(ch1));
  ASSERT_TRUE(static_cast<bool>(ch2));

  std::vector<uint8_t> received;
  link.responder.mux.SetDataHandler(*ch2, [&](uint32_t, std::vector<uint8_t> payload) {
    received = std::move(payload);
  });

  ASSERT_TRUE(static_cast<bool>(link.initiator.mux.ResetChannel(*ch1)));
  EXPECT_EQ(link.initiator.mux.State(*ch1), ChannelState::Closed);
  EXPECT_EQ(link.initiator.mux.State(*ch2), ChannelState::Open);

  const std::vector<uint8_t> msg = {'o', 'k'};
  ASSERT_TRUE(static_cast<bool>(link.initiator.mux.SendData(*ch2, msg)));
  EXPECT_EQ(received, msg);
}

TEST(ChannelMuxTest, LargePayloadFragments) {
  auto link_result = test::AmpTestLink::Create();
  ASSERT_TRUE(static_cast<bool>(link_result));
  auto& link = **link_result;

  auto ch = link.initiator.mux.OpenOutbound("/pp-browser/chat-blob/1.0.0", TestBulkPolicy());
  ASSERT_TRUE(static_cast<bool>(ch));

  std::vector<uint8_t> received;
  link.responder.mux.SetDataHandler(*ch, [&](uint32_t, std::vector<uint8_t> payload) {
    received = std::move(payload);
  });

  std::vector<uint8_t> large(2500, 0xAB);
  ASSERT_TRUE(static_cast<bool>(link.initiator.mux.SendData(*ch, large)));
  EXPECT_EQ(received, large);
}

TEST(ChannelMuxTest, OpenCarriesMaxMessageBytesForBlob) {
  auto link_result = test::AmpTestLink::Create();
  ASSERT_TRUE(static_cast<bool>(link_result));
  auto& link = **link_result;

  auto ch = link.initiator.mux.OpenOutbound("/pp-browser/chat-blob/1.0.0", ChatBlobChannelPolicy());
  ASSERT_TRUE(static_cast<bool>(ch));

  std::vector<uint8_t> received;
  link.responder.mux.SetDataHandler(*ch, [&](uint32_t, std::vector<uint8_t> payload) {
    received = std::move(payload);
  });

  // Ledger-sized (512 KiB) and beyond the prior responder default (256 KiB).
  std::vector<uint8_t> large(512 * 1024, 0xCD);
  ASSERT_TRUE(static_cast<bool>(link.initiator.mux.SendData(*ch, large)));
  ASSERT_EQ(received.size(), large.size());
  EXPECT_EQ(received, large);
}

TEST(ChannelMuxTest, ApplyChannelPolicyRaisesReassemblyBudget) {
  auto link_result = test::AmpTestLink::Create();
  ASSERT_TRUE(static_cast<bool>(link_result));
  auto& link = **link_result;

  // OPEN with default ControlJson budget (256 KiB).
  auto ch = link.initiator.mux.OpenOutbound("/pp-browser/chat/1.0.0", ControlJsonChannelPolicy());
  ASSERT_TRUE(static_cast<bool>(ch));

  ASSERT_TRUE(static_cast<bool>(link.responder.mux.ApplyChannelPolicy(*ch, ChatBlobChannelPolicy())));

  std::vector<uint8_t> received;
  link.responder.mux.SetDataHandler(*ch, [&](uint32_t, std::vector<uint8_t> payload) {
    received = std::move(payload);
  });

  std::vector<uint8_t> large(512 * 1024, 0xEE);
  // Initiator still limited by ControlJson until Apply on initiator too.
  ASSERT_TRUE(static_cast<bool>(link.initiator.mux.ApplyChannelPolicy(*ch, ChatBlobChannelPolicy())));
  ASSERT_TRUE(static_cast<bool>(link.initiator.mux.SendData(*ch, large)));
  EXPECT_EQ(received, large);
}

TEST(ChannelMuxTest, CapabilityChannelZero) {
  auto link_result = test::AmpTestLink::Create();
  ASSERT_TRUE(static_cast<bool>(link_result));
  auto& link = **link_result;

  CapabilityPayload offer;
  offer.local_peer_id = "QmCap";
  offer.protocols = {"/pp-browser/chat/1.0.0"};

  CapabilityPayload decoded;
  link.responder.mux.SetDataHandler(kCapabilityChannelId, [&](uint32_t, std::vector<uint8_t> payload) {
    auto cap = CapabilityCodec::Decode(payload);
    ASSERT_TRUE(static_cast<bool>(cap));
    decoded = std::move(*cap);
  });

  ASSERT_TRUE(static_cast<bool>(ChannelMux::SendCapabilityOffer(link.initiator.mux, offer)));
  EXPECT_EQ(decoded.local_peer_id, offer.local_peer_id);
  EXPECT_EQ(decoded.protocols, offer.protocols);
}

TEST(ChannelMuxTest, InboundProtocolHandler) {
  auto link_result = test::AmpTestLink::Create();
  ASSERT_TRUE(static_cast<bool>(link_result));
  auto& link = **link_result;

  uint32_t opened_id = 0;
  link.responder.mux.SetProtocolHandler("/pp-browser/chat/1.0.0", [&](const uint32_t channel_id, const std::string& pid) {
    opened_id = channel_id;
    EXPECT_EQ(pid, "/pp-browser/chat/1.0.0");
  });

  auto ch = link.initiator.mux.OpenOutbound("/pp-browser/chat/1.0.0", ControlJsonChannelPolicy());
  ASSERT_TRUE(static_cast<bool>(ch));
  EXPECT_EQ(opened_id, *ch);
}

TEST(ChannelMuxTest, RemoteResetNotifiesChannelSession) {
  auto link_result = test::AmpTestLink::Create();
  ASSERT_TRUE(static_cast<bool>(link_result));
  auto& link = **link_result;

  auto ch1 = link.initiator.mux.OpenOutbound("/pp-browser/chat/1.0.0", ControlJsonChannelPolicy());
  auto ch2 = link.initiator.mux.OpenOutbound("/pp-browser/chat-history/1.0.0", ControlJsonChannelPolicy());
  ASSERT_TRUE(static_cast<bool>(ch1));
  ASSERT_TRUE(static_cast<bool>(ch2));

  std::string terminal_reason;
  ChannelSession session;
  session.Bind(link.responder.mux, *ch1, ControlJsonChannelPolicy(), [](Roe<std::vector<uint8_t>>) { return true; },
                 [&](const char* reason) { terminal_reason = reason ? reason : ""; });

  ASSERT_TRUE(static_cast<bool>(link.initiator.mux.ResetChannel(*ch1)));
  EXPECT_EQ(terminal_reason, "peer_reset");
  EXPECT_TRUE(session.IsClosed());
  EXPECT_EQ(link.initiator.mux.State(*ch2), ChannelState::Open);
}

TEST(ChannelSessionTest, ReadOnceClosesAfterFirstFrame) {
  auto link_result = test::AmpTestLink::Create();
  ASSERT_TRUE(static_cast<bool>(link_result));
  auto& link = **link_result;

  auto ch = link.initiator.mux.OpenOutbound("/pp-browser/chat/1.0.0", ControlJsonChannelPolicy());
  ASSERT_TRUE(static_cast<bool>(ch));

  ChannelSession session;
  int deliveries = 0;
  session.Bind(link.responder.mux, *ch, ControlJsonChannelPolicy(), [&](Roe<std::vector<uint8_t>> body) {
    EXPECT_TRUE(static_cast<bool>(body));
    ++deliveries;
    return false;
  });

  ASSERT_TRUE(static_cast<bool>(link.initiator.mux.SendData(*ch, {'a'})));
  EXPECT_EQ(deliveries, 1);
  EXPECT_TRUE(session.IsClosed());
}

TEST(ChannelMuxTest, FragPreflightRefusesWhenCreditsLow) {
  auto link_result = test::AmpTestLink::Create();
  ASSERT_TRUE(static_cast<bool>(link_result));
  auto& link = **link_result;

  size_t credits = 1; // 2500 B needs 3 FRAG frames @ 900 B
  size_t transport_calls = 0;
  link.initiator.mux.SetTransportCredits([&] { return credits; });
  link.initiator.mux.SetTransport([&](uint32_t ch, uint32_t seq, adp::QosClass, std::vector<uint8_t> sealed) {
    ++transport_calls;
    (void)link.responder.mux.OnSealedInbound(ch, seq, sealed);
    return Roe<void>();
  });

  auto ch = link.initiator.mux.OpenOutbound("/pp-browser/chat-blob/1.0.0", TestBulkPolicy());
  ASSERT_TRUE(static_cast<bool>(ch));
  transport_calls = 0;

  std::vector<uint8_t> large(2500, 0xAB);
  auto sent = link.initiator.mux.SendData(*ch, large);
  ASSERT_FALSE(static_cast<bool>(sent));
  EXPECT_NE(sent.error().message.find("window full"), std::string::npos);
  EXPECT_EQ(transport_calls, 0u);

  credits = 8;
  std::vector<uint8_t> received;
  link.responder.mux.SetDataHandler(*ch, [&](uint32_t, std::vector<uint8_t> payload) {
    received = std::move(payload);
  });
  ASSERT_TRUE(static_cast<bool>(link.initiator.mux.SendData(*ch, large)));
  EXPECT_EQ(received, large);
  EXPECT_GE(transport_calls, 3u);
}

} // namespace
} // namespace pp::amp
