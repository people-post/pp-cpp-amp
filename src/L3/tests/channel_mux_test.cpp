#include "amp/L3/AmpChannelLimits.h"
#include "amp/L3/ChannelPolicy.h"
#include "amp/L3/ChannelSession.h"
#include "amp_test_link.h"

#include <gtest/gtest.h>

#include <string>
#include <unordered_map>

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
  policy.max_message_bytes = AmpChannelLimits::kMaxBulkFrameBytes;
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

TEST(ChannelMuxTest, CountsOpenChannelsByProtocol) {
  auto link_result = test::AmpTestLink::Create();
  ASSERT_TRUE(static_cast<bool>(link_result));
  auto& link = **link_result;

  auto chat1 = link.initiator.mux.OpenOutbound("/pp-browser/chat/1.0.0", ControlJsonChannelPolicy());
  auto chat2 = link.initiator.mux.OpenOutbound("/pp-browser/chat/1.0.0", ControlJsonChannelPolicy());
  auto media = link.initiator.mux.OpenOutbound("/pp-browser/call-media/1.0.0", TestRealtimePolicy());
  ASSERT_TRUE(static_cast<bool>(chat1));
  ASSERT_TRUE(static_cast<bool>(chat2));
  ASSERT_TRUE(static_cast<bool>(media));

  std::unordered_map<std::string, size_t> counts;
  link.initiator.mux.CountOpenChannels(counts);
  EXPECT_EQ(counts["/pp-browser/chat/1.0.0"], 2u);
  EXPECT_EQ(counts["/pp-browser/call-media/1.0.0"], 1u);

  // Closing and reset channels are no longer open.
  ASSERT_TRUE(static_cast<bool>(link.initiator.mux.CloseChannel(*chat1)));
  ASSERT_TRUE(static_cast<bool>(link.initiator.mux.ResetChannel(*media)));
  counts.clear();
  link.initiator.mux.CountOpenChannels(counts);
  EXPECT_EQ(counts["/pp-browser/chat/1.0.0"], 1u);
  EXPECT_EQ(counts.count("/pp-browser/call-media/1.0.0"), 0u);
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

  auto ch = link.initiator.mux.OpenOutbound("/pp-browser/chat-blob/1.0.0", BulkChannelPolicy());
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

// Regression: an inbound Open declaring Control class but a Bulk-sized max_message_bytes must be
// clamped to the local Control ceiling, not trusted at face value — the offered byte count is a
// hint, not policy (a Bulk channel genuinely gets the larger ceiling; see
// OpenCarriesMaxMessageBytesForBlob above).
TEST(ChannelMuxTest, InboundOpenClampsOfferedBytesToLocalClassCeiling) {
  auto link_result = test::AmpTestLink::Create();
  ASSERT_TRUE(static_cast<bool>(link_result));
  auto& link = **link_result;

  ChannelPolicy spoofed;
  spoofed.cls = ChannelClass::Control;
  spoofed.max_message_bytes = AmpChannelLimits::kMaxBulkFrameBytes;
  auto ch = link.initiator.mux.OpenOutbound("/pp-browser/chat/1.0.0", spoofed);
  ASSERT_TRUE(static_cast<bool>(ch));

  // Between the Control ceiling and the Bulk ceiling: must be refused on the responder, which
  // only ever saw channel_class=Control on the wire.
  std::vector<uint8_t> mid(AmpChannelLimits::kMaxChatStreamJsonBytes + 4096, 0xEE);
  EXPECT_FALSE(static_cast<bool>(link.responder.mux.SendData(*ch, mid)));

  std::vector<uint8_t> received;
  link.initiator.mux.SetDataHandler(*ch, [&](uint32_t, std::vector<uint8_t> payload) {
    received = std::move(payload);
  });
  std::vector<uint8_t> small(1024, 0xAA);
  ASSERT_TRUE(static_cast<bool>(link.responder.mux.SendData(*ch, small)));
  EXPECT_EQ(received, small);
}

TEST(ChannelMuxTest, ApplyChannelPolicyRaisesReassemblyBudget) {
  auto link_result = test::AmpTestLink::Create();
  ASSERT_TRUE(static_cast<bool>(link_result));
  auto& link = **link_result;

  // OPEN with default ControlJson budget (256 KiB).
  auto ch = link.initiator.mux.OpenOutbound("/pp-browser/chat/1.0.0", ControlJsonChannelPolicy());
  ASSERT_TRUE(static_cast<bool>(ch));

  ASSERT_TRUE(static_cast<bool>(link.responder.mux.ApplyChannelPolicy(*ch, BulkChannelPolicy())));

  std::vector<uint8_t> received;
  link.responder.mux.SetDataHandler(*ch, [&](uint32_t, std::vector<uint8_t> payload) {
    received = std::move(payload);
  });

  std::vector<uint8_t> large(512 * 1024, 0xEE);
  // Initiator still limited by ControlJson until Apply on initiator too.
  ASSERT_TRUE(static_cast<bool>(link.initiator.mux.ApplyChannelPolicy(*ch, BulkChannelPolicy())));
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

// Regression: an Open whose channel id has the *same* parity as the receiver's own dynamic ids
// (i.e. the parity the receiver expects for ITS OWN opens, not the peer's) is a glare / squatting
// attempt and must be refused, not silently accepted.
TEST(ChannelMuxTest, RejectsOpenWithWrongIdParity) {
  auto link_result = test::AmpTestLink::Create();
  ASSERT_TRUE(static_cast<bool>(link_result));
  auto& link = **link_result;

  // Responder's own opens use even ids; the initiator must use odd. Force an even fixed_id from
  // the initiator to simulate a peer violating that.
  auto bad = link.initiator.mux.OpenOutbound("/x/1", ControlJsonChannelPolicy(), /*fixed_id=*/2u);
  ASSERT_TRUE(static_cast<bool>(bad));
  EXPECT_EQ(link.responder.mux.State(*bad), ChannelState::Closed) << "no record kept on the receiver";
  EXPECT_EQ(link.initiator.mux.State(*bad), ChannelState::Closed) << "rejection round-trips back";
}

// Regression: a peer cannot Open more than kMaxConcurrentChannels channel records at once.
TEST(ChannelMuxTest, RejectsOpenBeyondConcurrentChannelCap) {
  auto link_result = test::AmpTestLink::Create();
  ASSERT_TRUE(static_cast<bool>(link_result));
  auto& link = **link_result;

  // Fixed ids, odd (initiator's own parity), starting past the capability/default ids in use.
  uint32_t id = 101;
  for (size_t i = 0; i < AmpChannelLimits::kMaxConcurrentChannels; ++i, id += 2) {
    auto ch = link.initiator.mux.OpenOutbound("/x/1", ControlJsonChannelPolicy(), id);
    ASSERT_TRUE(static_cast<bool>(ch)) << "open #" << i;
    ASSERT_EQ(link.responder.mux.State(*ch), ChannelState::Open) << "open #" << i;
  }

  auto over_cap = link.initiator.mux.OpenOutbound("/x/1", ControlJsonChannelPolicy(), id);
  ASSERT_TRUE(static_cast<bool>(over_cap));
  EXPECT_EQ(link.responder.mux.State(*over_cap), ChannelState::Closed);
  EXPECT_EQ(link.initiator.mux.State(*over_cap), ChannelState::Closed);
}

TEST(ChannelMuxTest, RefusingMuxRejectsOpensNobodyHandles) {
  auto link_result = test::AmpTestLink::Create();
  ASSERT_TRUE(static_cast<bool>(link_result));
  auto& link = **link_result;
  link.responder.mux.SetRefuseUnhandledOpens(true);

  std::string terminal_reason;
  auto refused = link.initiator.mux.OpenOutbound("/nobody/1", ControlJsonChannelPolicy());
  ASSERT_TRUE(static_cast<bool>(refused));
  EXPECT_EQ(link.initiator.mux.State(*refused), ChannelState::Closed);
  EXPECT_EQ(link.responder.mux.State(*refused), ChannelState::Closed) << "no record kept";

  bool handler_called = false;
  link.responder.mux.SetProtocolHandler("/served/1", [&](uint32_t, const std::string&) { handler_called = true; });
  auto served = link.initiator.mux.OpenOutbound("/served/1", ControlJsonChannelPolicy());
  ASSERT_TRUE(static_cast<bool>(served));
  EXPECT_EQ(link.initiator.mux.State(*served), ChannelState::Open);
  EXPECT_TRUE(handler_called);
}

TEST(ChannelMuxTest, RefusingMuxStillAcceptsPreboundAndCapabilityChannels) {
  auto link_result = test::AmpTestLink::Create();
  ASSERT_TRUE(static_cast<bool>(link_result));
  auto& link = **link_result;
  link.responder.mux.SetRefuseUnhandledOpens(true);

  // Data handler bound to the id before the OPEN arrives (the capability channel pattern).
  CapabilityPayload decoded;
  link.responder.mux.SetDataHandler(kCapabilityChannelId, [&](uint32_t, std::vector<uint8_t> payload) {
    auto cap = CapabilityCodec::Decode(payload);
    ASSERT_TRUE(static_cast<bool>(cap));
    decoded = std::move(*cap);
  });
  CapabilityPayload offer;
  offer.local_peer_id = "QmCap";
  ASSERT_TRUE(static_cast<bool>(ChannelMux::SendCapabilityOffer(link.initiator.mux, offer)));
  EXPECT_EQ(decoded.local_peer_id, "QmCap");
}

TEST(ChannelMuxTest, RawMuxAcceptsUnhandledOpensByDefault) {
  auto link_result = test::AmpTestLink::Create();
  ASSERT_TRUE(static_cast<bool>(link_result));
  auto& link = **link_result;
  auto ch = link.initiator.mux.OpenOutbound("/nobody/1", ControlJsonChannelPolicy());
  ASSERT_TRUE(static_cast<bool>(ch));
  EXPECT_EQ(link.initiator.mux.State(*ch), ChannelState::Open) << "policy is opt-in";
}

} // namespace
} // namespace pp::amp
