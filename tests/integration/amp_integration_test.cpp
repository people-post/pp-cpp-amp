#include "amp/L3/AmpChannelLimits.h"
#include "amp/L3/ChannelPolicy.h"
#include "amp/L3/ChannelWire.h"
#include "amp/L3/Types.h"
#include "amp/link/Types.h"
#include "support/amp_integration_harness.h"
#include "amp/L2/SessionControl.h"

#include <gtest/gtest.h>
#include <sodium.h>

namespace pbr::test {
namespace {

pp::amp::ChannelPolicy BulkPolicy() {
  return pp::amp::BulkChannelPolicy();
}

pp::amp::ChannelPolicy RealtimeBulkPolicy() {
  pp::amp::ChannelPolicy policy = pp::amp::BulkChannelPolicy();
  policy.cls = pp::amp::ChannelClass::Realtime;
  return policy;
}

class AmpIntegrationTest : public ::testing::Test {
protected:
  void SetUp() override { ASSERT_GE(sodium_init(), 0); }
};

TEST_F(AmpIntegrationTest, ChannelResetSiblingSurvives) {
  auto created = MakeAmpIntegrationHarness();
  ASSERT_TRUE(static_cast<bool>(created));
  auto& h = **created;
  ASSERT_TRUE(h.Associate());

  const auto ch_chat = h.OpenChannel(HarnessSide::A, "b", "/pp-browser/chat/1.0.0", pp::amp::ControlJsonChannelPolicy());
  const auto ch_hist =
      h.OpenChannel(HarnessSide::A, "b", "/pp-browser/chat-history/1.0.0", pp::amp::ControlJsonChannelPolicy());
  ASSERT_TRUE(ch_chat.has_value());
  ASSERT_TRUE(ch_hist.has_value());

  h.PumpUntil([&] {
    auto* link = h.mgr_a().FindLink("b");
    return link && link->Mux() && link->Mux()->State(*ch_chat) == pp::amp::ChannelState::Open
           && link->Mux()->State(*ch_hist) == pp::amp::ChannelState::Open;
  });

  std::vector<uint8_t> received;
  auto* inbound = h.mgr_b().FindLinkByPeerId(h.peer_id_a);
  ASSERT_NE(inbound, nullptr);
  inbound->Mux()->SetDataHandler(*ch_hist, [&](uint32_t, std::vector<uint8_t> payload) {
    received = std::move(payload);
  });

  auto* outbound = h.mgr_a().FindLink("b");
  ASSERT_NE(outbound, nullptr);
  ASSERT_TRUE(static_cast<bool>(outbound->Mux()->ResetChannel(*ch_chat)));
  h.PumpBoth();
  EXPECT_EQ(outbound->Mux()->State(*ch_chat), pp::amp::ChannelState::Closed);
  EXPECT_EQ(outbound->Mux()->State(*ch_hist), pp::amp::ChannelState::Open);

  const std::vector<uint8_t> msg = {'o', 'k'};
  ASSERT_TRUE(h.SendMuxData(HarnessSide::A, "b", *ch_hist, msg));
  ASSERT_TRUE(h.PumpUntilReceived(received, [&] { return received == msg; }));
  EXPECT_EQ(received, msg);
}

TEST_F(AmpIntegrationTest, ReliableDataSurvivesLoss) {
  auto created = MakeAmpIntegrationHarness();
  ASSERT_TRUE(static_cast<bool>(created));
  auto& h = **created;
  ASSERT_TRUE(h.Associate());

  const auto ch = h.OpenChannel(HarnessSide::A, "b", "/pp-browser/chat/1.0.0", pp::amp::ControlJsonChannelPolicy());
  ASSERT_TRUE(ch.has_value());

  std::vector<uint8_t> received;
  auto* inbound = h.mgr_b().FindLinkByPeerId(h.peer_id_a);
  ASSERT_NE(inbound, nullptr);
  inbound->Mux()->SetDataHandler(*ch, [&](uint32_t, std::vector<uint8_t> payload) {
    received = std::move(payload);
  });

  h.ConfigureLoss(HarnessSide::A, 3);
  const std::vector<uint8_t> msg = {'l', 'o', 's', 's'};
  ASSERT_TRUE(h.SendMuxData(HarnessSide::A, "b", *ch, msg));
  for (int i = 0; i < 40; ++i) {
    h.AdvanceMs(15);
  }
  EXPECT_EQ(received, msg);
}

/** N4: UDP reorder under Reliable — app still sees messages in send order. */
TEST_F(AmpIntegrationTest, ReliableDataSurvivesReorder) {
  auto created = MakeAmpIntegrationHarness();
  ASSERT_TRUE(static_cast<bool>(created));
  auto& h = **created;
  ASSERT_TRUE(h.Associate());

  const auto ch = h.OpenChannel(HarnessSide::A, "b", "/pp-browser/chat/1.0.0", pp::amp::ControlJsonChannelPolicy());
  ASSERT_TRUE(ch.has_value());

  std::vector<std::vector<uint8_t>> received;
  auto* inbound = h.mgr_b().FindLinkByPeerId(h.peer_id_a);
  ASSERT_NE(inbound, nullptr);
  inbound->Mux()->SetDataHandler(*ch, [&](uint32_t, std::vector<uint8_t> payload) {
    received.push_back(std::move(payload));
  });

  // Small window forces random release (true permute); L1 resequences before L3.
  h.ConfigureReorder(HarnessSide::A, /*window=*/2, /*seed=*/7);
  const std::vector<std::vector<uint8_t>> msgs = {{'a'}, {'b'}, {'c'}, {'d'}, {'e'}};
  for (const auto& msg : msgs) {
    ASSERT_TRUE(h.SendMuxData(HarnessSide::A, "b", *ch, msg));
  }
  h.FlushReorder(HarnessSide::A);
  h.ClearFaultInjection(HarnessSide::A);
  for (int i = 0; i < 40; ++i) {
    h.AdvanceMs(15);
  }
  ASSERT_EQ(received.size(), msgs.size());
  EXPECT_EQ(received, msgs);
  EXPECT_TRUE(h.mgr_a().IsConnected("b"));
}

/** N5: Bulk FRAG under true datagram reorder. */
TEST_F(AmpIntegrationTest, BulkFragSurvivesReorder) {
  auto created = MakeAmpIntegrationHarness();
  ASSERT_TRUE(static_cast<bool>(created));
  auto& h = **created;
  ASSERT_TRUE(h.Associate());

  const auto ch = h.OpenChannel(HarnessSide::A, "b", "/pp-browser/chat-blob/1.0.0", BulkPolicy());
  ASSERT_TRUE(ch.has_value());

  std::vector<uint8_t> received;
  auto* inbound = h.mgr_b().FindLinkByPeerId(h.peer_id_a);
  ASSERT_NE(inbound, nullptr);
  inbound->Mux()->SetDataHandler(*ch, [&](uint32_t, std::vector<uint8_t> payload) {
    received = std::move(payload);
  });

  h.ConfigureReorder(HarnessSide::A, /*window=*/2, /*seed=*/11);
  std::vector<uint8_t> large(2500, 0xCD);
  ASSERT_TRUE(h.SendMuxData(HarnessSide::A, "b", *ch, large));
  h.FlushReorder(HarnessSide::A);
  h.ClearFaultInjection(HarnessSide::A);
  for (int i = 0; i < 60; ++i) {
    h.AdvanceMs(15);
  }
  EXPECT_EQ(received, large);
  EXPECT_TRUE(h.mgr_a().IsConnected("b"));
}

/** N5b: Bulk FRAG survives DropNext loss (multi-datagram). */
TEST_F(AmpIntegrationTest, BulkFragSurvivesLoss) {
  auto created = MakeAmpIntegrationHarness();
  ASSERT_TRUE(static_cast<bool>(created));
  auto& h = **created;
  ASSERT_TRUE(h.Associate());

  const auto ch = h.OpenChannel(HarnessSide::A, "b", "/pp-browser/chat-blob/1.0.0", BulkPolicy());
  ASSERT_TRUE(ch.has_value());

  std::vector<uint8_t> received;
  auto* inbound = h.mgr_b().FindLinkByPeerId(h.peer_id_a);
  ASSERT_NE(inbound, nullptr);
  inbound->Mux()->SetDataHandler(*ch, [&](uint32_t, std::vector<uint8_t> payload) {
    received = std::move(payload);
  });

  h.ConfigureLoss(HarnessSide::A, 1);
  std::vector<uint8_t> large(2500, 0xAB);
  ASSERT_TRUE(h.SendMuxData(HarnessSide::A, "b", *ch, large));
  for (int i = 0; i < 80; ++i) {
    h.AdvanceMs(15);
  }
  EXPECT_EQ(received, large);
  EXPECT_TRUE(h.mgr_a().IsConnected("b"));
}

/** N6: Duplicate datagrams — app handler fires once. */
TEST_F(AmpIntegrationTest, ReliableDataSurvivesDup) {
  auto created = MakeAmpIntegrationHarness();
  ASSERT_TRUE(static_cast<bool>(created));
  auto& h = **created;
  ASSERT_TRUE(h.Associate());

  const auto ch = h.OpenChannel(HarnessSide::A, "b", "/pp-browser/chat/1.0.0", pp::amp::ControlJsonChannelPolicy());
  ASSERT_TRUE(ch.has_value());

  std::vector<std::vector<uint8_t>> received;
  auto* inbound = h.mgr_b().FindLinkByPeerId(h.peer_id_a);
  ASSERT_NE(inbound, nullptr);
  inbound->Mux()->SetDataHandler(*ch, [&](uint32_t, std::vector<uint8_t> payload) {
    received.push_back(std::move(payload));
  });

  h.ConfigureDup(HarnessSide::A, /*rate=*/1.0, /*seed=*/3);
  const std::vector<uint8_t> msg = {'d', 'u', 'p'};
  ASSERT_TRUE(h.SendMuxData(HarnessSide::A, "b", *ch, msg));
  h.ClearFaultInjection(HarnessSide::A);
  for (int i = 0; i < 40; ++i) {
    h.AdvanceMs(15);
  }
  ASSERT_EQ(received.size(), 1u);
  EXPECT_EQ(received[0], msg);
  EXPECT_TRUE(h.mgr_a().IsConnected("b"));
}

/** N13: Simultaneous loss + reorder — small Reliable and Bulk FRAG. */
TEST_F(AmpIntegrationTest, ReliableAndBulkSurviveLossPlusReorder) {
  auto created = MakeAmpIntegrationHarness();
  ASSERT_TRUE(static_cast<bool>(created));
  auto& h = **created;
  ASSERT_TRUE(h.Associate());

  const auto ch_ctrl = h.OpenChannel(HarnessSide::A, "b", "/pp-browser/chat/1.0.0", pp::amp::ControlJsonChannelPolicy());
  const auto ch_bulk = h.OpenChannel(HarnessSide::A, "b", "/pp-browser/chat-blob/1.0.0", BulkPolicy());
  ASSERT_TRUE(ch_ctrl.has_value());
  ASSERT_TRUE(ch_bulk.has_value());

  std::vector<uint8_t> got_ctrl;
  std::vector<uint8_t> got_bulk;
  auto* inbound = h.mgr_b().FindLinkByPeerId(h.peer_id_a);
  ASSERT_NE(inbound, nullptr);
  inbound->Mux()->SetDataHandler(*ch_ctrl, [&](uint32_t, std::vector<uint8_t> payload) {
    got_ctrl = std::move(payload);
  });
  inbound->Mux()->SetDataHandler(*ch_bulk, [&](uint32_t, std::vector<uint8_t> payload) {
    got_bulk = std::move(payload);
  });

  h.ConfigureReorder(HarnessSide::A, /*window=*/2, /*seed=*/19);
  h.ConfigureLoss(HarnessSide::A, 2);

  const std::vector<uint8_t> ctrl = {'n', '1', '3'};
  std::vector<uint8_t> bulk(2500, 0x13);
  ASSERT_TRUE(h.SendMuxData(HarnessSide::A, "b", *ch_ctrl, ctrl));
  ASSERT_TRUE(h.SendMuxData(HarnessSide::A, "b", *ch_bulk, bulk));
  h.FlushReorder(HarnessSide::A);
  h.ClearFaultInjection(HarnessSide::A);
  for (int i = 0; i < 100; ++i) {
    h.AdvanceMs(15);
  }
  EXPECT_EQ(got_ctrl, ctrl);
  EXPECT_EQ(got_bulk, bulk);
  EXPECT_TRUE(h.mgr_a().IsConnected("b"));
}

TEST_F(AmpIntegrationTest, DualDialChannelsWorkAfterElection) {
  auto created = MakeAmpIntegrationHarness();
  ASSERT_TRUE(static_cast<bool>(created));
  auto& h = **created;
  ASSERT_TRUE(h.DualAssociate());

  const auto ch = h.OpenChannel(HarnessSide::A, "b", "/pp-browser/chat/1.0.0", pp::amp::ControlJsonChannelPolicy());
  ASSERT_TRUE(ch.has_value());

  std::vector<uint8_t> received;
  auto* inbound = h.mgr_b().FindLinkByPeerId(h.peer_id_a);
  ASSERT_NE(inbound, nullptr);
  inbound->Mux()->SetDataHandler(*ch, [&](uint32_t, std::vector<uint8_t> payload) {
    received = std::move(payload);
  });

  const std::vector<uint8_t> msg = {'d', 'u', 'a', 'l'};
  ASSERT_TRUE(h.SendMuxData(HarnessSide::A, "b", *ch, msg));
  ASSERT_TRUE(h.PumpUntilReceived(received, [&] { return received == msg; }));
  EXPECT_EQ(received, msg);
}

TEST_F(AmpIntegrationTest, MshFailureNoConnectedMux) {
  auto created = MakeAmpIntegrationHarness();
  ASSERT_TRUE(static_cast<bool>(created));
  auto& h = **created;
  h.ep_b->SetAcceptEnabled(true);
  ASSERT_TRUE(static_cast<bool>(h.mgr_a().RegisterEndpoint("b", h.ma_b)));

  h.io_a->SetDropRate(1.0);
  h.io_b->SetDropRate(1.0);
  bool done = false;
  bool ok = true;
  h.mgr_a().EnsureAssociation("b", [&](pp::amp::PeerLinkManager::LinkRoe result) {
    ok = result.isOk();
    done = true;
  });
  for (size_t i = 0; i < 300; ++i) {
    h.PumpBoth();
  }
  EXPECT_FALSE(h.mgr_a().IsConnected("b"));
  EXPECT_EQ(h.mgr_b().FindConnectedInboundLink(), nullptr);
  if (done) {
    EXPECT_FALSE(ok);
  }
}

TEST_F(AmpIntegrationTest, OpenChannelWithoutEndpointReturnsCodedError) {
  auto created = MakeAmpIntegrationHarness();
  ASSERT_TRUE(static_cast<bool>(created));
  auto& h = **created;

  bool done = false;
  pp::amp::PeerLinkManager::Err err_code = pp::amp::PeerLinkManager::Err::Ok;
  h.mgr_a().OpenChannel("b", "/pp-browser/chat/1.0.0", pp::amp::ControlJsonChannelPolicy(),
                        [&](pp::amp::PeerLinkManager::ChannelRoe channel) {
                          if (!channel) {
                            err_code = channel.error().GetCode();
                          }
                          done = true;
                        });
  EXPECT_TRUE(done);
  EXPECT_EQ(err_code, pp::amp::PeerLinkManager::Err::EndpointNotRegistered);
  EXPECT_FALSE(h.mgr_a().IsConnected("b"));
}

TEST_F(AmpIntegrationTest, AssociationCloseAndRecovery) {
  auto created = MakeAmpIntegrationHarness();
  ASSERT_TRUE(static_cast<bool>(created));
  auto& h = **created;
  ASSERT_TRUE(h.Associate());

  auto* outbound = h.mgr_a().FindLink("b");
  ASSERT_NE(outbound, nullptr);
  auto* conn = outbound->ConnectionOrNull();
  ASSERT_NE(conn, nullptr);
  conn->Close();

  for (int i = 0; i < 50; ++i) {
    h.AdvanceMs(100);
  }
  EXPECT_FALSE(h.mgr_a().IsConnected("b"));

  ASSERT_TRUE(h.Associate());
  EXPECT_TRUE(h.mgr_a().IsConnected("b"));
}

TEST_F(AmpIntegrationTest, PathMigrateMidSession) {
  auto created = MakeAmpIntegrationHarness();
  ASSERT_TRUE(static_cast<bool>(created));
  auto& h = **created;
  ASSERT_TRUE(h.Associate());

  const auto ch = h.OpenChannel(HarnessSide::A, "b", "/pp-browser/chat/1.0.0", pp::amp::ControlJsonChannelPolicy());
  ASSERT_TRUE(ch.has_value());

  std::vector<uint8_t> received;
  auto* inbound = h.mgr_b().FindLinkByPeerId(h.peer_id_a);
  ASSERT_NE(inbound, nullptr);
  auto* inbound_conn = inbound->ConnectionOrNull();
  ASSERT_NE(inbound_conn, nullptr);
  inbound->Mux()->SetDataHandler(*ch, [&](uint32_t, std::vector<uint8_t> payload) {
    received = std::move(payload);
  });

  const std::vector<uint8_t> msg1 = {'a'};
  ASSERT_TRUE(h.SendMuxData(HarnessSide::A, "b", *ch, msg1));
  ASSERT_TRUE(h.PumpUntilReceived(received, [&] { return received == msg1; }));

  std::vector<pp::amp::LinkEvent> path_events;
  h.mgr_b().AddLinkEventListener([&](const pp::amp::LinkEvent& event) {
    if (event.kind == pp::amp::LinkEvent::Kind::PathChanged) {
      path_events.push_back(event);
    }
  });

  const pp::adp::IpEndpoint alt_a = pp::adp::IpEndpoint::V4(10, 0, 0, 1, 1001);
  const std::vector<uint8_t> ping = {'p'};
  ASSERT_TRUE(h.SendSealedFromAlternatePath(HarnessSide::A, "b", *ch, alt_a, ping, 2));
  for (int i = 0; i < 20; ++i) {
    h.PumpBoth();
  }
  EXPECT_EQ(inbound_conn->PeerEndpoint(), alt_a);
  ASSERT_FALSE(path_events.empty());
  EXPECT_EQ(path_events[0].peer_id, h.peer_id_a);
  ASSERT_TRUE(path_events[0].previous_remote.has_value());
  ASSERT_TRUE(path_events[0].remote.has_value());
  EXPECT_EQ(*path_events[0].previous_remote, h.addr_a);
  EXPECT_EQ(*path_events[0].remote, alt_a);

  const std::vector<uint8_t> msg2 = {'b'};
  ASSERT_TRUE(h.SendMuxData(HarnessSide::A, "b", *ch, msg2));
  ASSERT_TRUE(h.PumpUntilReceived(received, [&] { return received == msg2; }));
  EXPECT_EQ(inbound_conn->PeerEndpoint(), h.addr_a);
  EXPECT_EQ(inbound->Mux()->State(*ch), pp::amp::ChannelState::Open);
}

TEST_F(AmpIntegrationTest, LargeFragRoundTripThroughStack) {
  auto created = MakeAmpIntegrationHarness();
  ASSERT_TRUE(static_cast<bool>(created));
  auto& h = **created;
  ASSERT_TRUE(h.Associate());

  const auto ch = h.OpenChannel(HarnessSide::A, "b", "/pp-browser/chat-blob/1.0.0", BulkPolicy());
  ASSERT_TRUE(ch.has_value());

  std::vector<uint8_t> received;
  auto* inbound = h.mgr_b().FindLinkByPeerId(h.peer_id_a);
  ASSERT_NE(inbound, nullptr);
  inbound->Mux()->SetDataHandler(*ch, [&](uint32_t, std::vector<uint8_t> payload) {
    received = std::move(payload);
  });

  std::vector<uint8_t> large(2500, 0xAB);
  ASSERT_TRUE(h.SendMuxData(HarnessSide::A, "b", *ch, large));
  ASSERT_TRUE(h.PumpUntilReceived(received, [&] { return received == large; }));
  EXPECT_EQ(received, large);
}

TEST_F(AmpIntegrationTest, WireRekeyWithGraceWindow) {
  auto created = MakeAmpIntegrationHarness();
  ASSERT_TRUE(static_cast<bool>(created));
  auto& h = **created;
  ASSERT_TRUE(h.Associate());

  const auto ch = h.OpenChannel(HarnessSide::A, "b", "/pp-browser/chat/1.0.0", pp::amp::ControlJsonChannelPolicy());
  ASSERT_TRUE(ch.has_value());
  h.PumpBoth();

  auto* link_a = h.mgr_a().FindLink("b");
  auto* link_b = h.mgr_b().FindLinkByPeerId(h.peer_id_a);
  ASSERT_NE(link_a, nullptr);
  ASSERT_NE(link_b, nullptr);
  ASSERT_NE(link_a->GetSession(), nullptr);
  const uint32_t epoch_before = link_a->GetSession()->Material().session_epoch;

  pp::amp::ChannelFrame frame;
  frame.header.frame_type = pp::amp::ChannelFrameType::Data;
  frame.header.channel_id = *ch;
  frame.header.channel_seq = 99;
  frame.payload = {'g', 'r', 'a', 'c', 'e'};
  auto wire = pp::amp::ChannelWire::Encode(frame);
  ASSERT_TRUE(static_cast<bool>(wire));
  auto stale_sealed = link_a->GetSession()->Seal(*ch, 99, *wire);
  ASSERT_TRUE(static_cast<bool>(stale_sealed));

  ASSERT_TRUE(h.RequestRekey(HarnessSide::A, "b"));
  EXPECT_EQ(link_a->GetSession()->Material().session_epoch, epoch_before + 1);
  EXPECT_EQ(link_b->GetSession()->Material().session_epoch, epoch_before + 1);

  auto opened = link_b->GetSession()->Open(*ch, 99, *stale_sealed, h.clock->NowMs());
  ASSERT_TRUE(static_cast<bool>(opened));
  auto decoded = pp::amp::ChannelWire::Decode(*opened);
  ASSERT_TRUE(static_cast<bool>(decoded));
  EXPECT_EQ(decoded->payload, frame.payload);

  const std::vector<uint8_t> msg = {'n', 'e', 'w'};
  std::vector<uint8_t> received;
  link_b->Mux()->SetDataHandler(*ch, [&](uint32_t, std::vector<uint8_t> payload) {
    received = std::move(payload);
  });
  ASSERT_TRUE(h.SendMuxData(HarnessSide::A, "b", *ch, msg));
  ASSERT_TRUE(h.PumpUntilReceived(received, [&] { return received == msg; }));
}

TEST_F(AmpIntegrationTest, PostGraceStaleEpochDropped) {
  auto created = MakeAmpIntegrationHarness();
  ASSERT_TRUE(static_cast<bool>(created));
  auto& h = **created;
  ASSERT_TRUE(h.Associate());

  const auto ch = h.OpenChannel(HarnessSide::A, "b", "/pp-browser/chat/1.0.0", pp::amp::ControlJsonChannelPolicy());
  ASSERT_TRUE(ch.has_value());

  auto* link_a = h.mgr_a().FindLink("b");
  auto* link_b = h.mgr_b().FindLinkByPeerId(h.peer_id_a);
  ASSERT_NE(link_a, nullptr);
  ASSERT_NE(link_b, nullptr);

  pp::amp::ChannelFrame frame;
  frame.header.frame_type = pp::amp::ChannelFrameType::Data;
  frame.header.channel_id = *ch;
  frame.header.channel_seq = 77;
  frame.payload = {'s', 't', 'a', 'l', 'e'};
  auto wire = pp::amp::ChannelWire::Encode(frame);
  ASSERT_TRUE(static_cast<bool>(wire));
  auto stale_sealed = link_a->GetSession()->Seal(*ch, 77, *wire);
  ASSERT_TRUE(static_cast<bool>(stale_sealed));

  ASSERT_TRUE(h.RequestRekey(HarnessSide::A, "b"));
  h.AdvanceMs(pp::amp::kSessionRekeyGraceMs + 1);

  auto opened = link_b->GetSession()->Open(*ch, 77, *stale_sealed, h.clock->NowMs());
  EXPECT_FALSE(static_cast<bool>(opened));

  const std::vector<uint8_t> msg = {'f', 'r', 'e', 's', 'h'};
  std::vector<uint8_t> received;
  link_b->Mux()->SetDataHandler(*ch, [&](uint32_t, std::vector<uint8_t> payload) {
    received = std::move(payload);
  });
  ASSERT_TRUE(h.SendMuxData(HarnessSide::A, "b", *ch, msg));
  ASSERT_TRUE(h.PumpUntilReceived(received, [&] { return received == msg; }));
}

/** N14: Rekey request/ack survive DropNext; post-rekey data still works. */
TEST_F(AmpIntegrationTest, RekeySurvivesLoss) {
  auto created = MakeAmpIntegrationHarness();
  ASSERT_TRUE(static_cast<bool>(created));
  auto& h = **created;
  ASSERT_TRUE(h.Associate());

  const auto ch = h.OpenChannel(HarnessSide::A, "b", "/pp-browser/chat/1.0.0", pp::amp::ControlJsonChannelPolicy());
  ASSERT_TRUE(ch.has_value());

  auto* link_a = h.mgr_a().FindLink("b");
  auto* link_b = h.mgr_b().FindLinkByPeerId(h.peer_id_a);
  ASSERT_NE(link_a, nullptr);
  ASSERT_NE(link_b, nullptr);
  ASSERT_NE(link_a->GetSession(), nullptr);
  const uint32_t epoch_before = link_a->GetSession()->Material().session_epoch;

  h.ConfigureLoss(HarnessSide::A, 3);
  bool done = false;
  bool ok = false;
  link_a->RequestSessionRekey([&](pp::Roe<void> result) {
    ok = result.isOk();
    done = true;
  });
  // PumpUntil alone does not advance the clock; drive rtx explicitly.
  for (int i = 0; i < 80 && !done; ++i) {
    h.AdvanceMs(15);
  }
  ASSERT_TRUE(done);
  ASSERT_TRUE(ok);
  EXPECT_EQ(link_a->GetSession()->Material().session_epoch, epoch_before + 1);
  EXPECT_EQ(link_b->GetSession()->Material().session_epoch, epoch_before + 1);

  h.ClearFaultInjection(HarnessSide::A);
  h.ConfigureLoss(HarnessSide::A, 2);
  std::vector<uint8_t> received;
  link_b->Mux()->SetDataHandler(*ch, [&](uint32_t, std::vector<uint8_t> payload) {
    received = std::move(payload);
  });
  const std::vector<uint8_t> msg = {'r', 'k'};
  ASSERT_TRUE(h.SendMuxData(HarnessSide::A, "b", *ch, msg));
  for (int i = 0; i < 60; ++i) {
    h.AdvanceMs(15);
  }
  EXPECT_EQ(received, msg);
  EXPECT_TRUE(h.mgr_a().IsConnected("b"));
}

/** N15: After path migrate, Reliable data survives loss on the primary path. */
TEST_F(AmpIntegrationTest, PathMigrateSurvivesLoss) {
  auto created = MakeAmpIntegrationHarness();
  ASSERT_TRUE(static_cast<bool>(created));
  auto& h = **created;
  ASSERT_TRUE(h.Associate());

  const auto ch = h.OpenChannel(HarnessSide::A, "b", "/pp-browser/chat/1.0.0", pp::amp::ControlJsonChannelPolicy());
  ASSERT_TRUE(ch.has_value());

  std::vector<uint8_t> received;
  auto* inbound = h.mgr_b().FindLinkByPeerId(h.peer_id_a);
  ASSERT_NE(inbound, nullptr);
  auto* inbound_conn = inbound->ConnectionOrNull();
  ASSERT_NE(inbound_conn, nullptr);
  inbound->Mux()->SetDataHandler(*ch, [&](uint32_t, std::vector<uint8_t> payload) {
    received = std::move(payload);
  });

  const std::vector<uint8_t> msg1 = {'a'};
  ASSERT_TRUE(h.SendMuxData(HarnessSide::A, "b", *ch, msg1));
  ASSERT_TRUE(h.PumpUntilReceived(received, [&] { return received == msg1; }));

  const pp::adp::IpEndpoint alt_a = pp::adp::IpEndpoint::V4(10, 0, 0, 1, 1001);
  const std::vector<uint8_t> ping = {'p'};
  ASSERT_TRUE(h.SendSealedFromAlternatePath(HarnessSide::A, "b", *ch, alt_a, ping, 2));
  for (int i = 0; i < 20; ++i) {
    h.PumpBoth();
  }
  EXPECT_EQ(inbound_conn->PeerEndpoint(), alt_a);

  h.ConfigureLoss(HarnessSide::A, 3);
  const std::vector<uint8_t> msg2 = {'b'};
  ASSERT_TRUE(h.SendMuxData(HarnessSide::A, "b", *ch, msg2));
  for (int i = 0; i < 80; ++i) {
    h.AdvanceMs(15);
  }
  EXPECT_EQ(received, msg2);
  EXPECT_EQ(inbound->Mux()->State(*ch), pp::amp::ChannelState::Open);
  EXPECT_TRUE(h.mgr_a().IsConnected("b"));
}

TEST_F(AmpIntegrationTest, AdversarialDialTimeoutAdv04) {
  pp::amp::PeerLinkConfig cfg = AmpMeshTestLinkConfig();
  cfg.dial_timeout = std::chrono::milliseconds(200);
  auto created = MakeAmpIntegrationHarness(cfg);
  ASSERT_TRUE(static_cast<bool>(created));
  auto& h = **created;
  h.ep_b->SetAcceptEnabled(false);
  ASSERT_TRUE(static_cast<bool>(h.mgr_a().RegisterEndpoint("b", h.ma_b)));

  bool done = false;
  bool ok = true;
  pp::amp::PeerLinkManager::Err err_code = pp::amp::PeerLinkManager::Err::Ok;
  h.mgr_a().EnsureAssociation("b", [&](pp::amp::PeerLinkManager::LinkRoe result) {
    ok = result.isOk();
    if (!result) {
      err_code = result.error().GetCode();
    }
    done = true;
  });
  h.AdvanceMs(250);
  EXPECT_TRUE(done);
  EXPECT_FALSE(ok);
  EXPECT_EQ(err_code, pp::amp::PeerLinkManager::Err::DialTimeout);
  EXPECT_FALSE(h.mgr_a().IsConnected("b"));
}

TEST_F(AmpIntegrationTest, AdversarialMaxLinksAdv02) {
  pp::amp::PeerLinkConfig cfg = AmpMeshTestLinkConfig();
  cfg.max_links = 1;
  auto created = MakeAmpIntegrationHarness(cfg);
  ASSERT_TRUE(static_cast<bool>(created));
  auto& h = **created;

  h.ep_b->SetAcceptEnabled(false);
  ASSERT_TRUE(static_cast<bool>(h.mgr_a().RegisterEndpoint("b", h.ma_b)));
  ASSERT_TRUE(static_cast<bool>(h.mgr_a().RegisterEndpoint("b2", h.ma_b)));
  h.mgr_a().EnsureAssociation("b", {});
  EXPECT_EQ(h.CountLinks(HarnessSide::A), 1);

  bool dial_done = false;
  bool dial_ok = true;
  pp::amp::PeerLinkManager::Err dial_err = pp::amp::PeerLinkManager::Err::Ok;
  h.mgr_a().EnsureAssociation("b2", [&](pp::amp::PeerLinkManager::LinkRoe result) {
    dial_ok = result.isOk();
    if (!result) {
      dial_err = result.error().GetCode();
    }
    dial_done = true;
  });
  EXPECT_TRUE(dial_done);
  EXPECT_FALSE(dial_ok);
  EXPECT_EQ(dial_err, pp::amp::PeerLinkManager::Err::MaxLinksReached);
  EXPECT_EQ(h.CountLinks(HarnessSide::A), 1);

  h.ep_b->SetAcceptEnabled(true);
  ASSERT_TRUE(h.Associate());
  EXPECT_EQ(h.CountLinks(HarnessSide::B), 1);

  pp::adp::OpenParams params;
  params.key = pp::amp::PreSessionPeerKey();
  params.mint_id = true;
  params.peer = h.addr_b;
  h.AdvanceMs(1);
  auto extra = h.ep_a->Open(params);
  ASSERT_TRUE(static_cast<bool>(extra));
  (void)(*extra)->Send(pp::adp::QosClass::Reliable, std::vector<uint8_t>{0xDE, 0xAD});
  h.PumpBoth();
  EXPECT_EQ(h.CountLinks(HarnessSide::B), 1);
}

TEST_F(AmpIntegrationTest, AdversarialGarbageMshMidHandshakeAdv03) {
  auto created = MakeAmpIntegrationHarness();
  ASSERT_TRUE(static_cast<bool>(created));
  auto& h = **created;
  h.ep_b->SetAcceptEnabled(true);
  ASSERT_TRUE(static_cast<bool>(h.mgr_a().RegisterEndpoint("b", h.ma_b)));

  bool done = false;
  bool ok = false;
  h.mgr_a().EnsureAssociation("b", [&](pp::amp::PeerLinkManager::LinkRoe result) {
    ok = result.isOk();
    done = true;
  });
  for (size_t i = 0; i < 500; ++i) {
    if (h.mgr_a().FindLink("b") != nullptr) {
      h.InjectMshGarbage(HarnessSide::A, "b");
    }
    h.InjectRawDatagram(HarnessSide::A, h.addr_b, std::vector<uint8_t>(32, 0xCC));
    h.PumpBoth();
    if (done && h.mgr_a().IsConnected("b")) {
      break;
    }
  }
  ASSERT_TRUE(done);
  EXPECT_TRUE(ok);
  EXPECT_TRUE(h.mgr_a().IsConnected("b"));
}

TEST_F(AmpIntegrationTest, AdversarialSealedGarbageFloodAdv06) {
  auto created = MakeAmpIntegrationHarness();
  ASSERT_TRUE(static_cast<bool>(created));
  auto& h = **created;
  ASSERT_TRUE(h.Associate());

  const auto ch = h.OpenChannel(HarnessSide::A, "b", "/pp-browser/chat/1.0.0", pp::amp::ControlJsonChannelPolicy());
  ASSERT_TRUE(ch.has_value());

  for (uint32_t i = 0; i < 500; ++i) {
    h.InjectSealedGarbage(HarnessSide::A, "b", *ch, i + 1000);
  }
  h.PumpBudget(30);
  EXPECT_TRUE(h.mgr_a().IsConnected("b"));
  EXPECT_TRUE(h.mgr_b().FindLinkByPeerId(h.peer_id_a) != nullptr);

  std::vector<uint8_t> received;
  auto* inbound = h.mgr_b().FindLinkByPeerId(h.peer_id_a);
  ASSERT_NE(inbound, nullptr);
  inbound->Mux()->SetDataHandler(*ch, [&](uint32_t, std::vector<uint8_t> payload) {
    received = std::move(payload);
  });

  const std::vector<uint8_t> msg = {'f', 'l', 'o', 'o', 'd'};
  ASSERT_TRUE(h.SendMuxData(HarnessSide::A, "b", *ch, msg));
  ASSERT_TRUE(h.PumpUntilReceived(received, [&] { return received == msg; }));
}

TEST_F(AmpIntegrationTest, AdversarialFragPartialBombAdv08) {
  auto created = MakeAmpIntegrationHarness();
  ASSERT_TRUE(static_cast<bool>(created));
  auto& h = **created;
  ASSERT_TRUE(h.Associate());

  const auto ch = h.OpenChannel(HarnessSide::A, "b", "/pp-browser/chat-blob/1.0.0", RealtimeBulkPolicy());
  ASSERT_TRUE(ch.has_value());

  for (uint64_t msg_id = 1; msg_id <= 25; ++msg_id) {
    ASSERT_TRUE(h.InjectPartialFrag(HarnessSide::A, "b", *ch, 1, msg_id, 0, 50, 50'000,
                                    std::vector<uint8_t>(500, 0xBB)));
  }
  h.PumpBudget(10);
  EXPECT_TRUE(h.mgr_a().IsConnected("b"));

  std::vector<uint8_t> received;
  auto* inbound = h.mgr_b().FindLinkByPeerId(h.peer_id_a);
  ASSERT_NE(inbound, nullptr);
  inbound->Mux()->SetDataHandler(*ch, [&](uint32_t, std::vector<uint8_t> payload) {
    received = std::move(payload);
  });

  const std::vector<uint8_t> msg = {'b', 'o', 'm', 'b'};
  ASSERT_TRUE(h.SendMuxData(HarnessSide::A, "b", *ch, msg));
  ASSERT_TRUE(h.PumpUntilReceived(received, [&] { return received == msg; }));

  h.mgr_a().MarkWarm("b");
  h.mgr_b().MarkWarm("a");
  h.AdvanceMs(pp::amp::kDefaultFragAssemblyTimeoutMs + 100);
  h.PumpBudget(5);
  EXPECT_TRUE(h.mgr_a().IsConnected("b"));
}

TEST_F(AmpIntegrationTest, ColdLinkEvictedAfterIdle) {
  auto created = MakeAmpIntegrationHarness();
  ASSERT_TRUE(static_cast<bool>(created));
  auto& h = **created;
  ASSERT_TRUE(h.Associate());
  EXPECT_TRUE(h.mgr_a().IsConnected("b"));
  h.AdvanceMs(pp::adp::kAliveTimeoutMs + 500);
  h.PumpBudget(10);
  EXPECT_FALSE(h.mgr_a().IsConnected("b"));
}

TEST_F(AmpIntegrationTest, WarmLinkSurvivesIdle) {
  auto created = MakeAmpIntegrationHarness();
  ASSERT_TRUE(static_cast<bool>(created));
  auto& h = **created;
  ASSERT_TRUE(h.Associate());
  h.mgr_a().MarkWarm("b");
  h.mgr_b().MarkWarm("a");
  h.AdvanceMs(pp::adp::kAliveTimeoutMs + 500);
  h.PumpBudget(10);
  EXPECT_TRUE(h.mgr_a().IsConnected("b"));
}

// Only the dialer marks the link hot (relay reservation pattern). The cold inbound end learns the
// cadence from keepalives and must not evict after kAliveTimeoutMs (dogfood 2026-09-24).
TEST_F(AmpIntegrationTest, HotDialerKeepsColdInboundAlive) {
  auto created = MakeAmpIntegrationHarness();
  ASSERT_TRUE(static_cast<bool>(created));
  auto& h = **created;
  ASSERT_TRUE(h.Associate());
  h.mgr_a().MarkHot("b");
  for (int i = 0; i < 120; ++i) {  // 60 s idle in 500 ms steps
    h.AdvanceMs(500);
    h.PumpBudget(2);
  }
  EXPECT_TRUE(h.mgr_a().IsConnected("b"));
  EXPECT_TRUE(h.mgr_b().IsConnectedToPeerId(h.peer_id_a)) << "cold inbound end must honour the cadence";
}

// Dead-peer detection: a hot link whose peer goes silent is evicted after its widened window,
// no longer kept forever because it is warm/hot.
TEST_F(AmpIntegrationTest, HotLinkEvictedWhenPeerGoesSilent) {
  auto created = MakeAmpIntegrationHarness();
  ASSERT_TRUE(static_cast<bool>(created));
  auto& h = **created;
  ASSERT_TRUE(h.Associate());
  h.mgr_a().MarkHot("b");
  h.AdvanceMs(100);
  h.PumpBudget(4);
  ASSERT_TRUE(h.mgr_a().IsConnected("b"));

  std::vector<pp::amp::LinkEvent> dropped;
  h.mgr_a().AddLinkEventListener([&](const pp::amp::LinkEvent& event) {
    if (event.kind == pp::amp::LinkEvent::Kind::Dropped) {
      dropped.push_back(event);
    }
  });
  h.io_b->SetDropRate(1.0);  // B's sends (echoes) vanish
  const int64_t window = h.mgr_a().FindLink("b")->ConnectionOrNull()->LivenessWindowMs();
  ASSERT_GT(window, pp::adp::kAliveTimeoutMs);
  for (int64_t t = 0; t <= window + 2'000 && h.mgr_a().IsConnected("b"); t += 500) {
    h.AdvanceMs(500);
    h.PumpBudget(1);
  }
  EXPECT_FALSE(h.mgr_a().IsConnected("b"));
  ASSERT_FALSE(dropped.empty());
  EXPECT_EQ(dropped[0].reason, pp::amp::LinkDropReason::ConnectionDead);
  EXPECT_TRUE(dropped[0].was_connected);
}

TEST_F(AmpIntegrationTest, OutboundWarmKeepaliveRefreshesAssociation) {
  auto created = MakeAmpIntegrationHarness();
  ASSERT_TRUE(static_cast<bool>(created));
  auto& h = **created;
  ASSERT_TRUE(h.Associate());
  // Both ends must be warm: cold idle DropLink now sends ADP Close (B21), and B25
  // evicts a warm link whose Connection is already closed.
  h.mgr_a().MarkWarm("b");
  h.mgr_b().MarkWarm("a");
  auto* outbound = h.mgr_a().FindLink("b");
  ASSERT_NE(outbound, nullptr);
  auto* conn = outbound->ConnectionOrNull();
  ASSERT_NE(conn, nullptr);
  h.AdvanceMs(25'000);
  h.PumpBudget(20);
  EXPECT_TRUE(conn->LooksAlive(h.clock->NowMs()));
  EXPECT_TRUE(h.mgr_a().IsConnected("b"));
}


} // namespace
} // namespace pbr::test
