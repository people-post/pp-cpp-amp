#include "amp/L1/Clock.h"
#include "amp/L1/Endpoint.h"
#include "amp/L1/MemoryDatagramIo.h"
#include "crypto/MlDsa.h"
#include "amp/L3/ChannelPolicy.h"
#include "amp/link/AdpMultiaddr.h"
#include "amp/link/AmpStack.h"
#include "amp/link/DialBook.h"
#include "amp/link/MeshPump.h"
#include "amp/link/MeshRuntime.h"
#include "amp/link/PeerLinkManager.h"
#include "support/mesh_test_harness.h"
#include "support/mesh_harness_support.h"

#include <gtest/gtest.h>
#include <sodium.h>

#include <array>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace pp::amp {
namespace {

struct MeshLinkFixture {
  std::shared_ptr<adp::VirtualClock> clock;
  std::shared_ptr<adp::MemoryDatagramHub> hub;
  std::shared_ptr<adp::MemoryDatagramIo> io_a;
  std::shared_ptr<adp::MemoryDatagramIo> io_b;
  std::unique_ptr<adp::Endpoint> ep_a;
  std::unique_ptr<adp::Endpoint> ep_b;
  adp::IpEndpoint addr_a;
  adp::IpEndpoint addr_b;
  MshIdentity alice;
  MshIdentity bob;
  std::unique_ptr<PeerLinkManager> mgr_a;
  std::unique_ptr<PeerLinkManager> mgr_b;
  std::unique_ptr<MeshPump> pump_a;
  std::unique_ptr<MeshPump> pump_b;

  static Roe<MeshLinkFixture> Create() {
    MeshLinkFixture f;
    f.clock = std::make_shared<adp::VirtualClock>(1'000'000);
    f.hub = adp::MemoryDatagramIo::MakeHub();
    f.addr_a = adp::IpEndpoint::V4(10, 0, 0, 1, 1000);
    f.addr_b = adp::IpEndpoint::V4(10, 0, 0, 2, 2000);
    f.io_a = std::make_shared<adp::MemoryDatagramIo>(f.hub, f.addr_a);
    f.io_b = std::make_shared<adp::MemoryDatagramIo>(f.hub, f.addr_b);
    f.ep_a = std::make_unique<adp::Endpoint>(f.io_a, f.clock);
    f.ep_b = std::make_unique<adp::Endpoint>(f.io_b, f.clock);
    f.ep_b->SetAcceptEnabled(true);

    auto alice_keys = pp::MlDsa::GenerateKeyPair();
    auto bob_keys = pp::MlDsa::GenerateKeyPair();
    if (!alice_keys || !bob_keys) {
      return Error("mesh link test: keygen failed");
    }
    f.alice.ml_dsa_secret_key = std::move(alice_keys->secret_key);
    f.alice.ml_dsa_public_key = std::move(alice_keys->public_key);
    f.bob.ml_dsa_secret_key = std::move(bob_keys->secret_key);
    f.bob.ml_dsa_public_key = std::move(bob_keys->public_key);

    f.mgr_a = std::make_unique<PeerLinkManager>(*f.ep_a, f.alice, "QmAlice");
    f.mgr_b = std::make_unique<PeerLinkManager>(*f.ep_b, f.bob, "QmBob");
    f.pump_a = std::make_unique<MeshPump>(*f.ep_a, *f.mgr_a);
    f.pump_b = std::make_unique<MeshPump>(*f.ep_b, *f.mgr_b);
    return f;
  }

  void PumpBoth() {
    pump_a->Pump();
    pump_b->Pump();
    pump_a->Tick();
    pump_b->Tick();
  }

  void PumpUntil(const std::function<bool()>& done, const size_t max_rounds = 500) {
    for (size_t i = 0; i < max_rounds && !done(); ++i) {
      PumpBoth();
    }
  }
};

TEST(MeshLinkTest, EnsureAssociationOverMemoryIo) {
  ASSERT_GE(sodium_init(), 0);
  auto fixture = MeshLinkFixture::Create();
  ASSERT_TRUE(static_cast<bool>(fixture));

  auto bob_addr = FormatAdpMultiaddr(fixture->addr_b, "QmBob");
  ASSERT_TRUE(static_cast<bool>(bob_addr));
  ASSERT_TRUE(static_cast<bool>(fixture->mgr_a->RegisterEndpoint("bob", *bob_addr)));

  bool associated = false;
  std::string assoc_error;
  fixture->mgr_a->EnsureAssociation("bob", [&](PeerLinkManager::LinkRoe result) {
    associated = result.isOk();
    if (!associated) {
      assoc_error = result.error().message;
    }
  });

  fixture->PumpUntil([&] {
    return associated && fixture->mgr_b->FindConnectedInboundLink() != nullptr;
  });

  EXPECT_TRUE(associated) << assoc_error;
  EXPECT_TRUE(fixture->mgr_a->IsConnected("bob"));
  ASSERT_NE(fixture->mgr_b->FindConnectedInboundLink(), nullptr);
}

TEST(MeshLinkTest, OpenChannelDataRoundTrip) {
  ASSERT_GE(sodium_init(), 0);
  auto fixture = MeshLinkFixture::Create();
  ASSERT_TRUE(static_cast<bool>(fixture));

  auto bob_addr = FormatAdpMultiaddr(fixture->addr_b, "QmBob");
  ASSERT_TRUE(static_cast<bool>(bob_addr));
  ASSERT_TRUE(static_cast<bool>(fixture->mgr_a->RegisterEndpoint("bob", *bob_addr)));

  bool associated = false;
  fixture->mgr_a->EnsureAssociation("bob", [&](PeerLinkManager::LinkRoe result) { associated = static_cast<bool>(result); });
  fixture->PumpUntil([&] {
    return associated && fixture->mgr_b->FindConnectedInboundLink() != nullptr;
  });
  ASSERT_TRUE(associated);

  uint32_t channel_id = 0;
  bool channel_done = false;
  std::optional<uint32_t> channel_id_result;
  std::string channel_error;
  fixture->mgr_a->OpenChannel("bob", "/pp-browser/chat/1.0.0", ControlJsonChannelPolicy(),
                              [&](PeerLinkManager::ChannelRoe ch) {
                                if (ch.isOk()) {
                                  channel_id_result = ch.value();
                                } else {
                                  channel_error = ch.error().message;
                                }
                                channel_done = true;
                              });
  fixture->PumpUntil([&] { return channel_done; });
  ASSERT_TRUE(channel_id_result.has_value()) << channel_error;
  channel_id = *channel_id_result;

  fixture->PumpUntil([&] {
    auto* outbound = fixture->mgr_a->FindLink("bob");
    return outbound && outbound->Mux() && outbound->Mux()->State(channel_id) == ChannelState::Open;
  });

  std::vector<uint8_t> received;
  auto* inbound = fixture->mgr_b->FindConnectedInboundLink();
  ASSERT_NE(inbound, nullptr);
  inbound->Mux()->SetDataHandler(channel_id, [&](uint32_t, std::vector<uint8_t> payload) {
    received = std::move(payload);
  });

  auto* outbound = fixture->mgr_a->FindLink("bob");
  ASSERT_NE(outbound, nullptr);
  const std::vector<uint8_t> msg = {'h', 'i'};
  ASSERT_TRUE(static_cast<bool>(outbound->Mux()->SendData(channel_id, msg)));
  fixture->PumpBoth();
  EXPECT_EQ(received, msg);
}

TEST(MeshRuntimeTest, PumpDrivesAssociationRoundTrip) {
  ASSERT_GE(sodium_init(), 0);
  auto created = pbr::test::AmpMeshHarness::Create();
  ASSERT_TRUE(static_cast<bool>(created));
  auto harness = std::move(*created);

  ASSERT_TRUE(static_cast<bool>(harness->mgr_a().RegisterEndpoint("b", harness->ma_b)));
  ASSERT_TRUE(static_cast<bool>(harness->mgr_b().RegisterEndpoint("a", harness->ma_a)));

  bool associated = false;
  harness->mgr_a().EnsureAssociation("b", [&](PeerLinkManager::LinkRoe result) { associated = static_cast<bool>(result); });
  harness->PumpUntil([&] {
    return associated && harness->mgr_a().IsConnected("b") && harness->mgr_b().FindLinkByPeerId(harness->peer_id_a);
  });

  EXPECT_TRUE(associated);
  EXPECT_TRUE(harness->mgr_a().IsConnected("b"));
  auto* inbound_on_b = harness->mgr_b().FindLinkByPeerId(harness->peer_id_a);
  ASSERT_NE(inbound_on_b, nullptr);
  EXPECT_EQ(inbound_on_b->RemotePeerId(), harness->peer_id_a);
  EXPECT_EQ(inbound_on_b->PeerKey(), "a");
}

TEST(MeshLinkTest, InboundLinkRekeysToRegisteredAlias) {
  ASSERT_GE(sodium_init(), 0);
  auto created = pbr::test::AmpMeshHarness::Create();
  ASSERT_TRUE(static_cast<bool>(created));
  auto harness = std::move(*created);

  pbr::test::AmpMeshHarness& h = *harness;

  ASSERT_TRUE(static_cast<bool>(h.mgr_a().RegisterEndpoint("b", h.ma_b)));
  ASSERT_TRUE(static_cast<bool>(h.mgr_b().RegisterEndpoint("a", h.ma_a)));

  bool associated = false;
  h.mgr_a().EnsureAssociation("b", [&](PeerLinkManager::LinkRoe result) { associated = static_cast<bool>(result); });
  h.PumpUntil([&] { return associated; });
  ASSERT_TRUE(associated);

  auto* outbound = h.mgr_a().FindLink("b");
  ASSERT_NE(outbound, nullptr);
  EXPECT_EQ(outbound->RemotePeerId(), h.peer_id_b);

  auto* inbound = h.mgr_b().FindLink("a");
  ASSERT_NE(inbound, nullptr);
  EXPECT_EQ(inbound->RemotePeerId(), h.peer_id_a);
  EXPECT_FALSE(inbound->IsOutbound());

  EXPECT_EQ(h.mgr_b().FindLinkByPeerId(h.peer_id_a), inbound);
  EXPECT_EQ(h.mgr_a().FindLinkByPeerId(h.peer_id_b), outbound);
}

TEST(MeshLinkTest, CapabilityExchangeAfterAssociation) {
  ASSERT_GE(sodium_init(), 0);
  auto created = pbr::test::AmpMeshHarness::Create();
  ASSERT_TRUE(static_cast<bool>(created));
  auto harness = std::move(*created);
  pbr::test::AmpMeshHarness& h = *harness;

  h.mgr_a().SetLocalListenMultiaddrs({h.ma_a});
  h.mgr_a().SetAdvertisedProtocols({"/pp-browser/chat/1.0.0", "/pp-browser/circuit-relay/1.0.0"});
  h.mgr_b().SetLocalListenMultiaddrs({h.ma_b});
  h.mgr_b().SetAdvertisedProtocols({"/pp-browser/chat/1.0.0", "/pp-browser/media-relay/1.0.0"});

  int caps_a = 0;
  int caps_b = 0;
  CapabilityPayload seen_on_a;
  CapabilityPayload seen_on_b;
  h.mgr_a().SetCapabilityHandler([&](LinkHandle, const std::string&, const CapabilityPayload& remote) {
    ++caps_a;
    seen_on_a = remote;
  });
  h.mgr_b().SetCapabilityHandler([&](LinkHandle, const std::string&, const CapabilityPayload& remote) {
    ++caps_b;
    seen_on_b = remote;
  });

  ASSERT_TRUE(static_cast<bool>(h.mgr_a().RegisterEndpoint("b", h.ma_b)));
  ASSERT_TRUE(static_cast<bool>(h.mgr_b().RegisterEndpoint("a", h.ma_a)));

  bool associated = false;
  h.mgr_a().EnsureAssociation("b", [&](PeerLinkManager::LinkRoe result) { associated = static_cast<bool>(result); });
  h.PumpUntil([&] {
    return associated && caps_a > 0 && caps_b > 0 && h.mgr_a().FindLink("b") &&
           h.mgr_a().FindLink("b")->RemoteCapability() && h.mgr_b().FindLink("a") &&
           h.mgr_b().FindLink("a")->RemoteCapability();
  });

  ASSERT_TRUE(associated);
  EXPECT_EQ(caps_a, 1);
  EXPECT_EQ(caps_b, 1);

  auto* outbound = h.mgr_a().FindLink("b");
  ASSERT_NE(outbound, nullptr);
  ASSERT_NE(outbound->RemoteCapability(), nullptr);
  EXPECT_EQ(outbound->RemoteCapability()->local_peer_id, h.peer_id_b);
  EXPECT_EQ(outbound->RemoteCapability()->listen_multiaddrs, std::vector<std::string>{h.ma_b});
  EXPECT_EQ(outbound->RemoteCapability()->protocols,
            (std::vector<std::string>{"/pp-browser/chat/1.0.0", "/pp-browser/media-relay/1.0.0"}));

  auto* inbound = h.mgr_b().FindLink("a");
  ASSERT_NE(inbound, nullptr);
  ASSERT_NE(inbound->RemoteCapability(), nullptr);
  EXPECT_EQ(inbound->RemoteCapability()->local_peer_id, h.peer_id_a);
  EXPECT_EQ(inbound->RemoteCapability()->listen_multiaddrs, std::vector<std::string>{h.ma_a});
  EXPECT_EQ(inbound->RemoteCapability()->protocols,
            (std::vector<std::string>{"/pp-browser/chat/1.0.0", "/pp-browser/circuit-relay/1.0.0"}));

  EXPECT_EQ(seen_on_a.local_peer_id, h.peer_id_b);
  EXPECT_EQ(seen_on_b.local_peer_id, h.peer_id_a);
}

TEST(MeshLinkTest, CapabilityIngestEnablesPeerIdDial) {
  ASSERT_GE(sodium_init(), 0);
  auto created = pbr::test::AmpMeshHarness::Create();
  ASSERT_TRUE(static_cast<bool>(created));
  auto harness = std::move(*created);
  pbr::test::AmpMeshHarness& h = *harness;

  // Only A knows how to dial B initially; B learns A's listen addr from ch0.
  h.mgr_a().SetLocalListenMultiaddrs({h.ma_a});
  h.mgr_b().SetLocalListenMultiaddrs({h.ma_b});
  h.ep_a->SetAcceptEnabled(true);

  ASSERT_TRUE(static_cast<bool>(h.mgr_a().RegisterEndpoint("b", h.ma_b)));
  // Intentionally do not RegisterEndpoint(peer_id_a) on B before caps.

  bool associated = false;
  h.mgr_a().EnsureAssociation("b", [&](PeerLinkManager::LinkRoe result) { associated = static_cast<bool>(result); });
  h.PumpUntil([&] {
    return associated && h.mgr_b().PreferredMultiaddr(h.peer_id_a).has_value() &&
           h.mgr_a().PreferredMultiaddr(h.peer_id_b).has_value();
  });
  ASSERT_TRUE(associated);

  auto learned_a = h.mgr_b().PreferredMultiaddr(h.peer_id_a);
  ASSERT_TRUE(learned_a.has_value());
  EXPECT_EQ(*learned_a, h.ma_a);

  auto learned_b = h.mgr_a().PreferredMultiaddr(h.peer_id_b);
  ASSERT_TRUE(learned_b.has_value());
  EXPECT_EQ(*learned_b, h.ma_b);

  // B can now EnsureAssociation by authenticated PeerId without a prior alias registration.
  bool b_assoc = false;
  std::string b_err;
  h.mgr_b().EnsureAssociation(h.peer_id_a, [&](PeerLinkManager::LinkRoe result) {
    b_assoc = static_cast<bool>(result);
    if (!result) {
      b_err = result.error().message;
    }
  });
  h.PumpUntil([&] { return b_assoc || !b_err.empty(); });
  // Already connected via inbound adopt/rekey — EnsureAssociation should succeed immediately.
  EXPECT_TRUE(b_assoc) << b_err;
  EXPECT_TRUE(h.mgr_b().IsConnected(h.peer_id_a));
}

TEST(MeshLinkTest, DualDialElectsOneConnectedLinkPerPeerId) {
  ASSERT_GE(sodium_init(), 0);
  auto created = pbr::test::AmpMeshHarness::Create();
  ASSERT_TRUE(static_cast<bool>(created));
  auto harness = std::move(*created);
  pbr::test::AmpMeshHarness& h = *harness;

  // Both peers accept so simultaneous A↔B dials can complete.
  h.ep_a->SetAcceptEnabled(true);
  h.ep_b->SetAcceptEnabled(true);

  ASSERT_TRUE(static_cast<bool>(h.mgr_a().RegisterEndpoint("b", h.ma_b)));
  ASSERT_TRUE(static_cast<bool>(h.mgr_b().RegisterEndpoint("a", h.ma_a)));

  bool assoc_a = false;
  bool assoc_b = false;
  int done_a = 0;
  int done_b = 0;
  std::string err_a;
  std::string err_b;
  h.mgr_a().EnsureAssociation("b", [&](PeerLinkManager::LinkRoe result) {
    assoc_a = result.isOk();
    if (!assoc_a) {
      err_a = result.error().message;
    }
    ++done_a;
  });
  h.mgr_b().EnsureAssociation("a", [&](PeerLinkManager::LinkRoe result) {
    assoc_b = result.isOk();
    if (!assoc_b) {
      err_b = result.error().message;
    }
    ++done_b;
  });

  h.PumpUntil([&] {
    return done_a > 0 && done_b > 0 && h.mgr_a().CountConnectedLinksForPeerId(h.peer_id_b) == 1 &&
           h.mgr_b().CountConnectedLinksForPeerId(h.peer_id_a) == 1 &&
           h.mgr_a().FindLinkByPeerId(h.peer_id_b) != nullptr &&
           h.mgr_b().FindLinkByPeerId(h.peer_id_a) != nullptr;
  });

  EXPECT_EQ(done_a, 1);
  EXPECT_EQ(done_b, 1);
  EXPECT_TRUE(assoc_a) << err_a;
  EXPECT_TRUE(assoc_b) << err_b;
  EXPECT_EQ(h.mgr_a().CountConnectedLinksForPeerId(h.peer_id_b), 1u);
  EXPECT_EQ(h.mgr_b().CountConnectedLinksForPeerId(h.peer_id_a), 1u);

  auto* link_a = h.mgr_a().FindLinkByPeerId(h.peer_id_b);
  auto* link_b = h.mgr_b().FindLinkByPeerId(h.peer_id_a);
  ASSERT_NE(link_a, nullptr);
  ASSERT_NE(link_b, nullptr);
  EXPECT_EQ(link_a->Phase(), PeerLinkPhase::Connected);
  EXPECT_EQ(link_b->Phase(), PeerLinkPhase::Connected);

  // FindLinkByPeerId remains stable across extra pumps (alias may already be dial key).
  for (int i = 0; i < 10; ++i) {
    h.PumpBoth();
  }
  EXPECT_EQ(h.mgr_a().FindLinkByPeerId(h.peer_id_b), link_a);
  EXPECT_EQ(h.mgr_b().FindLinkByPeerId(h.peer_id_a), link_b);
  EXPECT_EQ(h.mgr_a().CountConnectedLinksForPeerId(h.peer_id_b), 1u);
  EXPECT_EQ(h.mgr_b().CountConnectedLinksForPeerId(h.peer_id_a), 1u);
  EXPECT_TRUE(h.mgr_a().IsConnected("b"));
  EXPECT_TRUE(h.mgr_b().IsConnected("a"));
}

TEST(AmpStackTest, CreateAndAssociateViaStacks) {
  ASSERT_GE(sodium_init(), 0);

  auto clock = std::make_shared<adp::VirtualClock>(1'000'000);
  auto hub = adp::MemoryDatagramIo::MakeHub();
  const auto addr_a = adp::IpEndpoint::V4(10, 0, 0, 1, 1000);
  const auto addr_b = adp::IpEndpoint::V4(10, 0, 0, 2, 2000);
  auto io_a = std::make_shared<adp::MemoryDatagramIo>(hub, addr_a);
  auto io_b = std::make_shared<adp::MemoryDatagramIo>(hub, addr_b);

  auto alice_keys = pp::MlDsa::GenerateKeyPair();
  auto bob_keys = pp::MlDsa::GenerateKeyPair();
  ASSERT_TRUE(static_cast<bool>(alice_keys));
  ASSERT_TRUE(static_cast<bool>(bob_keys));

  MshIdentity alice;
  alice.ml_dsa_secret_key = std::move(alice_keys->secret_key);
  alice.ml_dsa_public_key = std::move(alice_keys->public_key);
  MshIdentity bob;
  bob.ml_dsa_secret_key = std::move(bob_keys->secret_key);
  bob.ml_dsa_public_key = std::move(bob_keys->public_key);

  auto peer_a = pbr::test::DeriveTestPeerId(alice.ml_dsa_public_key);
  auto peer_b = pbr::test::DeriveTestPeerId(bob.ml_dsa_public_key);
  ASSERT_TRUE(static_cast<bool>(peer_a));
  ASSERT_TRUE(static_cast<bool>(peer_b));

  AmpStack::Config cfg_a;
  cfg_a.identity = alice;
  cfg_a.local_peer_id = *peer_a;
  cfg_a.link_config = pbr::test::AmpMeshTestLinkConfig();
  AmpStack::Config cfg_b;
  cfg_b.identity = bob;
  cfg_b.local_peer_id = *peer_b;
  cfg_b.link_config = pbr::test::AmpMeshTestLinkConfig();

  auto stack_a = AmpStack::Create(io_a, clock, cfg_a);
  auto stack_b = AmpStack::Create(io_b, clock, cfg_b);
  ASSERT_TRUE(static_cast<bool>(stack_a));
  ASSERT_TRUE(static_cast<bool>(stack_b));
  (*stack_a)->Start();
  (*stack_b)->Start();
  (*stack_b)->GetEndpoint().SetAcceptEnabled(true);

  auto ma_b = FormatAdpMultiaddr(addr_b, *peer_b);
  ASSERT_TRUE(static_cast<bool>(ma_b));
  ASSERT_TRUE(static_cast<bool>((*stack_a)->Links().RegisterEndpoint("b", *ma_b)));

  bool associated = false;
  (*stack_a)->Links().EnsureAssociation("b", [&](PeerLinkManager::LinkRoe result) { associated = static_cast<bool>(result); });
  for (size_t i = 0; i < 500 && !associated; ++i) {
    (*stack_a)->Pump();
    (*stack_b)->Pump();
    (*stack_a)->Tick();
    (*stack_b)->Tick();
  }
  EXPECT_TRUE(associated);
  EXPECT_TRUE((*stack_a)->Links().IsConnected("b"));
}

TEST(MeshLinkTest, EnsureAssociationOverMemoryIoIpv6) {
  ASSERT_GE(sodium_init(), 0);

  auto clock = std::make_shared<adp::VirtualClock>(1'000'000);
  auto hub = adp::MemoryDatagramIo::MakeHub();
  // Distinct documentation-prefix globals so Format/Parse round-trip is non-loopback.
  std::array<uint8_t, 16> bytes_a{};
  bytes_a[0] = 0x20;
  bytes_a[1] = 0x01;
  bytes_a[2] = 0x0d;
  bytes_a[3] = 0xb8;
  bytes_a[15] = 0x01;
  std::array<uint8_t, 16> bytes_b = bytes_a;
  bytes_b[15] = 0x02;
  const auto addr_a = adp::IpEndpoint::V6(bytes_a, 1000);
  const auto addr_b = adp::IpEndpoint::V6(bytes_b, 2000);

  auto io_a = std::make_shared<adp::MemoryDatagramIo>(hub, addr_a);
  auto io_b = std::make_shared<adp::MemoryDatagramIo>(hub, addr_b);
  auto ep_a = std::make_unique<adp::Endpoint>(io_a, clock);
  auto ep_b = std::make_unique<adp::Endpoint>(io_b, clock);
  ep_b->SetAcceptEnabled(true);

  auto alice_keys = pp::MlDsa::GenerateKeyPair();
  auto bob_keys = pp::MlDsa::GenerateKeyPair();
  ASSERT_TRUE(static_cast<bool>(alice_keys));
  ASSERT_TRUE(static_cast<bool>(bob_keys));
  MshIdentity alice;
  MshIdentity bob;
  alice.ml_dsa_secret_key = std::move(alice_keys->secret_key);
  alice.ml_dsa_public_key = std::move(alice_keys->public_key);
  bob.ml_dsa_secret_key = std::move(bob_keys->secret_key);
  bob.ml_dsa_public_key = std::move(bob_keys->public_key);

  PeerLinkManager mgr_a(*ep_a, alice, "QmAlice6");
  PeerLinkManager mgr_b(*ep_b, bob, "QmBob6");
  MeshPump pump_a(*ep_a, mgr_a);
  MeshPump pump_b(*ep_b, mgr_b);

  auto bob_addr = FormatAdpMultiaddr(addr_b, "QmBob6");
  ASSERT_TRUE(static_cast<bool>(bob_addr));
  EXPECT_NE(bob_addr->find("/ip6/"), std::string::npos);
  ASSERT_TRUE(static_cast<bool>(mgr_a.RegisterEndpoint("bob", *bob_addr)));

  bool associated = false;
  std::string assoc_error;
  mgr_a.EnsureAssociation("bob", [&](PeerLinkManager::LinkRoe result) {
    associated = result.isOk();
    if (!associated) {
      assoc_error = result.error().message;
    }
  });

  for (size_t i = 0; i < 500 && !(associated && mgr_b.FindConnectedInboundLink() != nullptr); ++i) {
    pump_a.Pump();
    pump_b.Pump();
    pump_a.Tick();
    pump_b.Tick();
  }

  EXPECT_TRUE(associated) << assoc_error;
  EXPECT_TRUE(mgr_a.IsConnected("bob"));
  ASSERT_NE(mgr_b.FindConnectedInboundLink(), nullptr);
}

TEST(DialBookTest, RegisterEndpointsKeepsOrderedCandidates) {
  DialBook book({});
  const std::string peer = "QmCand";
  const std::string bad = "/ip4/10.0.0.99/udp/1/adp/1.0.0/p2p/" + peer;
  const std::string good = "/ip4/10.0.0.1/udp/2/adp/1.0.0/p2p/" + peer;
  ASSERT_TRUE(static_cast<bool>(book.RegisterEndpoints("k", {bad, good})));
  ASSERT_EQ(book.Find("k")->candidates.size(), 2u);
  EXPECT_EQ(book.Find("k")->multiaddr, bad);
  EXPECT_TRUE(book.AdvanceDialCandidate("k"));
  EXPECT_EQ(book.Find("k")->multiaddr, good);
  EXPECT_FALSE(book.AdvanceDialCandidate("k"));
  book.PromoteDialWinner("k", good);
  EXPECT_EQ(book.Find("k")->multiaddr, good);
  EXPECT_EQ(book.Find("k")->candidates.front(), good);
}

TEST(DialBookTest, RegisterEndpointPromotesWithoutDroppingPrior) {
  DialBook book({});
  const std::string peer = "QmProm";
  const std::string a = "/ip4/10.0.0.1/udp/1/adp/1.0.0/p2p/" + peer;
  const std::string b = "/ip4/10.0.0.2/udp/2/adp/1.0.0/p2p/" + peer;
  ASSERT_TRUE(static_cast<bool>(book.RegisterEndpoint("k", a)));
  ASSERT_TRUE(static_cast<bool>(book.RegisterEndpoint("k", b)));
  ASSERT_EQ(book.Find("k")->candidates.size(), 2u);
  EXPECT_EQ(book.Find("k")->multiaddr, b);
  EXPECT_EQ(book.Find("k")->candidates[1], a);
}

TEST(MeshLinkTest, EnsureAssociationFallsBackToSecondCandidate) {
  ASSERT_GE(sodium_init(), 0);
  auto fixture = MeshLinkFixture::Create();
  ASSERT_TRUE(static_cast<bool>(fixture));

  auto good = FormatAdpMultiaddr(fixture->addr_b, "QmBob");
  ASSERT_TRUE(static_cast<bool>(good));
  const std::string blackhole = "/ip4/203.0.113.1/udp/59999/adp/1.0.0/p2p/QmBob";
  ASSERT_TRUE(static_cast<bool>(fixture->mgr_a->RegisterEndpoints("bob", {blackhole, *good})));

  fixture->mgr_a->Book().Config().dial_attempt_timeout = std::chrono::milliseconds(30);
  fixture->mgr_a->Book().Config().dial_timeout = std::chrono::milliseconds(5000);

  bool associated = false;
  std::string assoc_error;
  fixture->mgr_a->EnsureAssociation("bob", [&](PeerLinkManager::LinkRoe result) {
    associated = result.isOk();
    if (!associated) {
      assoc_error = result.error().message;
    }
  });

  for (size_t i = 0; i < 2000 && !associated; ++i) {
    fixture->PumpBoth();
    fixture->clock->Advance(5);
  }

  EXPECT_TRUE(associated) << assoc_error;
  EXPECT_TRUE(fixture->mgr_a->IsConnected("bob"));
}

TEST(MeshRuntimeDriveTest, PostDeferredRunsAfterWorkLane) {
  ASSERT_GE(sodium_init(), 0);
  auto created = pbr::test::AmpMeshHarness::Create();
  ASSERT_TRUE(static_cast<bool>(created)) << created.error().message;
  auto harness = std::move(*created);

  std::vector<int> order;
  harness->runtime_a->PostToIo([&] {
    order.push_back(1);
    harness->runtime_a->PostDeferred([&] { order.push_back(3); });
    order.push_back(2);
  });
  harness->runtime_a->Drive();
  ASSERT_EQ(order.size(), 3u);
  EXPECT_EQ(order[0], 1);
  EXPECT_EQ(order[1], 2);
  EXPECT_EQ(order[2], 3);
}

TEST(MeshRuntimeDriveTest, NestedDriveIsRefused) {
  ASSERT_GE(sodium_init(), 0);
  auto created = pbr::test::AmpMeshHarness::Create();
  ASSERT_TRUE(static_cast<bool>(created)) << created.error().message;
  auto harness = std::move(*created);

  int nested_drives = 0;
  int outer_steps = 0;
  harness->runtime_a->PostToIo([&] {
    ++outer_steps;
    harness->runtime_a->Drive(); // must no-op (exclusive driver)
    if (harness->runtime_a->IsDriving()) {
      ++nested_drives; // still in outer Drive — nested refused, flag still true
    }
  });
  harness->runtime_a->Drive();
  EXPECT_EQ(outer_steps, 1);
  EXPECT_EQ(nested_drives, 1);
  EXPECT_FALSE(harness->runtime_a->IsDriving());
}

TEST(MeshRuntimeDriveTest, PostAfterFiresOnAmpClock) {
  ASSERT_GE(sodium_init(), 0);
  auto created = pbr::test::AmpMeshHarness::Create();
  ASSERT_TRUE(static_cast<bool>(created)) << created.error().message;
  auto harness = std::move(*created);

  bool fired = false;
  harness->runtime_a->PostAfter(std::chrono::milliseconds(50), [&] { fired = true; });
  harness->runtime_a->Drive();
  EXPECT_FALSE(fired);
  harness->clock->Advance(49);
  harness->runtime_a->Drive();
  EXPECT_FALSE(fired);
  harness->clock->Advance(1);
  harness->runtime_a->Drive();
  EXPECT_TRUE(fired);
}

TEST(MeshRuntimeDriveTest, BurstDialBlackholeExpiresOnAmpClock) {
  ASSERT_GE(sodium_init(), 0);
  auto created = pbr::test::AmpMeshHarness::Create();
  ASSERT_TRUE(static_cast<bool>(created)) << created.error().message;
  auto harness = std::move(*created);

  const std::string blackhole =
      "/ip4/127.0.0.1/udp/1/adp/1.0.0/p2p/" + harness->peer_id_b;
  std::optional<BurstDialResult> done;
  harness->runtime_a->BurstDial({blackhole}, std::chrono::milliseconds(100),
                                [&](BurstDialResult r) { done = std::move(r); });

  for (size_t i = 0; i < 40 && !done; ++i) {
    harness->runtime_a->Drive();
    harness->clock->Advance(5);
  }
  ASSERT_TRUE(done.has_value());
  EXPECT_FALSE(done->ok);
  EXPECT_FALSE(done->error.empty());
}

TEST(MeshRuntimeDriveTest, BurstDialConnectsPeer) {
  ASSERT_GE(sodium_init(), 0);
  auto created = pbr::test::AmpMeshHarness::Create();
  ASSERT_TRUE(static_cast<bool>(created)) << created.error().message;
  auto harness = std::move(*created);

  std::optional<BurstDialResult> done;
  harness->runtime_a->BurstDial({harness->ma_b}, std::chrono::milliseconds(2000),
                                [&](BurstDialResult r) { done = std::move(r); });

  for (size_t i = 0; i < 500 && !done; ++i) {
    harness->PumpBoth();
    harness->clock->Advance(5);
  }
  ASSERT_TRUE(done.has_value()) << "BurstDial did not settle";
  EXPECT_TRUE(done->ok) << done->error;
  EXPECT_TRUE(harness->runtime_a->IsConnectedToPeerId(harness->peer_id_b));
}

TEST(MeshRuntimeDriveTest, IsConnectedToPeerIdIgnoresCarrierOnly) {
  ASSERT_GE(sodium_init(), 0);
  auto created = pbr::test::AmpMeshHarness::Create();
  ASSERT_TRUE(static_cast<bool>(created)) << created.error().message;
  auto harness = std::move(*created);

  // With no ADP association, carrier-only presence must not look "connected" to BurstDial.
  EXPECT_FALSE(harness->runtime_a->IsConnectedToPeerId(harness->peer_id_b));
  EXPECT_FALSE(harness->mgr_a().IsConnectedToPeerId(harness->peer_id_b));
}

} // namespace
} // namespace pp::amp
