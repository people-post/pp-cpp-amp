#include "amp/L1/Clock.h"
#include "amp/L1/Endpoint.h"
#include "amp/L1/LossyDatagramIo.h"
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
  /** Set when created lossy: drop / reorder on each side's sends (off until the test turns it on). */
  std::shared_ptr<adp::LossyDatagramIo> lossy_a;
  std::shared_ptr<adp::LossyDatagramIo> lossy_b;
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

  static Roe<MeshLinkFixture> Create(const bool lossy = false) {
    MeshLinkFixture f;
    f.clock = std::make_shared<adp::VirtualClock>(1'000'000);
    f.hub = adp::MemoryDatagramIo::MakeHub();
    f.addr_a = adp::IpEndpoint::V4(10, 0, 0, 1, 1000);
    f.addr_b = adp::IpEndpoint::V4(10, 0, 0, 2, 2000);
    f.io_a = std::make_shared<adp::MemoryDatagramIo>(f.hub, f.addr_a);
    f.io_b = std::make_shared<adp::MemoryDatagramIo>(f.hub, f.addr_b);
    if (lossy) {
      f.lossy_a = std::make_shared<adp::LossyDatagramIo>(f.io_a);
      f.lossy_b = std::make_shared<adp::LossyDatagramIo>(f.io_b);
      f.ep_a = std::make_unique<adp::Endpoint>(f.lossy_a, f.clock);
      f.ep_b = std::make_unique<adp::Endpoint>(f.lossy_b, f.clock);
    } else {
      f.ep_a = std::make_unique<adp::Endpoint>(f.io_a, f.clock);
      f.ep_b = std::make_unique<adp::Endpoint>(f.io_b, f.clock);
    }
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
  // Bob serves the protocol (links refuse opens nobody handles).
  std::vector<uint8_t> received;
  fixture->mgr_b->SetProtocolHandler("/pp-browser/chat/1.0.0", [&](LinkHandle, const std::string&, uint32_t ch) {
    auto* inbound = fixture->mgr_b->FindConnectedInboundLink();
    ASSERT_NE(inbound, nullptr);
    inbound->Mux()->SetDataHandler(ch, [&](uint32_t, std::vector<uint8_t> payload) { received = std::move(payload); });
  });

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

  auto* outbound = fixture->mgr_a->FindLink("bob");
  ASSERT_NE(outbound, nullptr);
  const std::vector<uint8_t> msg = {'h', 'i'};
  ASSERT_TRUE(static_cast<bool>(outbound->Mux()->SendData(channel_id, msg)));
  fixture->PumpBoth();
  EXPECT_EQ(received, msg);
}

// A peer that does not serve a protocol refuses the open: the opener's WhenChannelOpen fails at
// once instead of seeing an "open" channel whose requests vanish until its own timeout.
TEST(MeshLinkTest, UnservedProtocolOpenIsRefusedPromptly) {
  ASSERT_GE(sodium_init(), 0);
  auto fixture = MeshLinkFixture::Create();
  ASSERT_TRUE(static_cast<bool>(fixture));
  auto bob_addr = FormatAdpMultiaddr(fixture->addr_b, "QmBob");
  ASSERT_TRUE(static_cast<bool>(bob_addr));
  ASSERT_TRUE(static_cast<bool>(fixture->mgr_a->RegisterEndpoint("bob", *bob_addr)));
  fixture->mgr_b->SetRefuseUnhandledOpens(true);
  bool served_called = false;
  fixture->mgr_b->SetProtocolHandler("/served/1", [&](LinkHandle, const std::string&, uint32_t) { served_called = true; });

  const auto open_and_wait = [&](const std::string& protocol) -> std::optional<bool> {
    std::optional<uint32_t> id;
    bool done = false;
    fixture->mgr_a->OpenChannel("bob", protocol, ControlJsonChannelPolicy(), [&](PeerLinkManager::ChannelRoe ch) {
      if (ch.isOk()) {
        id = ch.value();
      }
      done = true;
    });
    fixture->PumpUntil([&] { return done; });
    if (!id) {
      return std::nullopt;
    }
    std::optional<bool> opened;
    fixture->mgr_a->WhenChannelOpenIn("bob", *id, std::chrono::seconds(30), [&](bool ok) { opened = ok; });
    // Far fewer rounds than the 30 s deadline: only a refusal (or an open) settles this.
    fixture->PumpUntil([&] { return opened.has_value(); }, 200);
    return opened;
  };

  const auto unserved = open_and_wait("/not-served/1");
  ASSERT_TRUE(unserved.has_value()) << "waited for the deadline instead of failing on the refusal";
  EXPECT_FALSE(*unserved);
  const auto served = open_and_wait("/served/1");
  ASSERT_TRUE(served.has_value());
  EXPECT_TRUE(*served);
  EXPECT_TRUE(served_called);
  EXPECT_NE(fixture->mgr_a->FindLink("bob"), nullptr) << "a refused channel does not cost the link";
}

// Dogfood SIGSEGV: far end resets the carrier while the nested handshake is in flight. The failure
// runs inside the nested link's carrier closed-callback → establish_cb_; the drop must be deferred to
// Tick, not free the PeerLink (and the running callbacks) synchronously.
TEST(MeshLinkTest, NestedCarrierResetDuringHandshakeDefersDrop) {
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

  std::optional<uint32_t> channel_id;
  fixture->mgr_a->OpenChannel("bob", "/pp-test/carrier/1.0.0", CircuitCarrierChannelPolicy(),
                              [&](PeerLinkManager::ChannelRoe ch) {
                                if (ch.isOk()) {
                                  channel_id = ch.value();
                                }
                              });
  fixture->PumpUntil([&] {
    auto* outbound = fixture->mgr_a->FindLink("bob");
    return channel_id && outbound && outbound->Mux() &&
           outbound->Mux()->State(*channel_id) == ChannelState::Open;
  });
  ASSERT_TRUE(channel_id.has_value());

  auto* outbound = fixture->mgr_a->FindLink("bob");
  ASSERT_NE(outbound, nullptr);
  auto carrier = std::make_shared<ChannelSession>();
  carrier->Bind(*outbound->Mux(), *channel_id, CircuitCarrierChannelPolicy(),
                [](Roe<std::vector<uint8_t>>) { return true; });

  std::vector<LinkEvent> events;
  fixture->mgr_a->AddLinkEventListener([&](const LinkEvent& event) { events.push_back(event); });

  // Bob never answers the nested handshake, so it stays Handshaking until the reset lands.
  std::optional<PeerLinkManager::LinkRoe> nested;
  fixture->mgr_a->EstablishNestedOverCarrier("nested:bob", carrier, true,
                                             [&](PeerLinkManager::LinkRoe result) { nested = result; });
  carrier.reset();
  auto* nested_link = fixture->mgr_a->FindLink("nested:bob");
  ASSERT_NE(nested_link, nullptr);
  EXPECT_EQ(nested_link->Phase(), PeerLinkPhase::Handshaking);

  auto* inbound = fixture->mgr_b->FindConnectedInboundLink();
  ASSERT_NE(inbound, nullptr);
  ASSERT_TRUE(static_cast<bool>(inbound->Mux()->ResetChannel(*channel_id)));
  fixture->pump_b->Pump();
  fixture->pump_a->Pump();

  ASSERT_TRUE(nested.has_value());
  EXPECT_FALSE(nested->isOk());
  // Still present until Tick drains the scheduled drop.
  EXPECT_NE(fixture->mgr_a->FindLink("nested:bob"), nullptr);

  fixture->pump_a->Tick();
  EXPECT_EQ(fixture->mgr_a->FindLink("nested:bob"), nullptr);
  EXPECT_TRUE(fixture->mgr_a->IsConnected("bob"));

  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].kind, LinkEvent::Kind::Dropped);
  EXPECT_EQ(events[0].dial_key, "nested:bob");
  EXPECT_EQ(events[0].transport, TransportClass::Carrier);
  EXPECT_EQ(events[0].reason, LinkDropReason::CarrierClosed);
  EXPECT_FALSE(events[0].was_connected);
}

// k1 (call-path-resilience): a drop scheduled for a failed link must not hit the link that took
// its dial key before Tick — and the failed one, no longer holding a key, must still be dropped.
TEST(MeshLinkTest, ScheduledDropHitsTheFailedLinkNotItsReplacement) {
  ASSERT_GE(sodium_init(), 0);
  auto fixture = MeshLinkFixture::Create();
  ASSERT_TRUE(static_cast<bool>(fixture));
  auto bob_addr = FormatAdpMultiaddr(fixture->addr_b, "QmBob");
  ASSERT_TRUE(static_cast<bool>(bob_addr));
  ASSERT_TRUE(static_cast<bool>(fixture->mgr_a->RegisterEndpoint("bob", *bob_addr)));
  bool associated = false;
  fixture->mgr_a->EnsureAssociation("bob", [&](PeerLinkManager::LinkRoe result) { associated = static_cast<bool>(result); });
  fixture->PumpUntil([&] { return associated && fixture->mgr_b->FindConnectedInboundLink() != nullptr; });
  ASSERT_TRUE(associated);
  const LinkHandle outer_b = fixture->mgr_b->FindConnectedInboundLink()->Handle();

  const auto open_carrier = [&]() -> std::shared_ptr<ChannelSession> {
    std::optional<uint32_t> channel_id;
    fixture->mgr_a->OpenChannel("bob", "/pp-test/carrier/1.0.0", CircuitCarrierChannelPolicy(),
                                [&](PeerLinkManager::ChannelRoe ch) {
                                  if (ch.isOk()) {
                                    channel_id = ch.value();
                                  }
                                });
    fixture->PumpUntil([&] {
      auto* outbound = fixture->mgr_a->FindLink("bob");
      return channel_id && outbound && outbound->Mux() && outbound->Mux()->State(*channel_id) == ChannelState::Open;
    });
    if (!channel_id) {
      return nullptr;
    }
    auto carrier = std::make_shared<ChannelSession>();
    carrier->Bind(*fixture->mgr_a->FindLink("bob")->Mux(), *channel_id, CircuitCarrierChannelPolicy(),
                  [](Roe<std::vector<uint8_t>>) { return true; });
    return carrier;
  };

  // Both carriers up front: opening one pumps (and Ticks), which would drain the drop too early.
  auto first_carrier = open_carrier();
  ASSERT_NE(first_carrier, nullptr);
  auto second_carrier = open_carrier();
  ASSERT_NE(second_carrier, nullptr);
  // First nested attempt (never answered), then its carrier resets: failed, drop scheduled.
  const uint32_t first_channel = first_carrier->ChannelId();
  fixture->mgr_a->EstablishNestedOverCarrier("nested:bob", first_carrier, true, [](PeerLinkManager::LinkRoe) {});
  first_carrier.reset();
  const LinkHandle first = fixture->mgr_a->FindLink("nested:bob")->Handle();
  ASSERT_TRUE(fixture->mgr_b->WithLiveLink(outer_b, [&](PeerLink& link) {
    (void)link.Mux()->ResetChannel(first_channel);
  }));
  fixture->pump_b->Pump();
  fixture->pump_a->Pump();

  // Before Tick drains that drop, a new attempt takes the same key.
  fixture->mgr_a->EstablishNestedOverCarrier("nested:bob", second_carrier, true, [](PeerLinkManager::LinkRoe) {});
  auto* replacement = fixture->mgr_a->FindLink("nested:bob");
  ASSERT_NE(replacement, nullptr);
  const LinkHandle second = replacement->Handle();
  ASSERT_NE(second.id, first.id);

  fixture->pump_a->Tick();
  EXPECT_FALSE(fixture->mgr_a->WithLiveLink(first, [](PeerLink&) {})) << "the failed link is gone, not orphaned";
  EXPECT_TRUE(fixture->mgr_a->WithLiveLink(second, [](PeerLink&) {})) << "its replacement survives the drop";
  auto* now = fixture->mgr_a->FindLink("nested:bob");
  ASSERT_NE(now, nullptr);
  EXPECT_EQ(now->Id(), second.id);
}

// pp-browser COLD dual-NAT: circuit reach aborts the direct ADP dial to the target's PeerId and
// establishes the nested link under that same key (A024: both may exist). The aborted ADP link's
// drop is still pending when the nested link takes the key: at Tick it must hit the ADP link — not
// "whatever holds the key" — and must not take the key's index with it.
TEST(MeshLinkTest, FailedAdpDialUnderASharedKeyLeavesTheNestedLinkAlone) {
  ASSERT_GE(sodium_init(), 0);
  auto fixture = MeshLinkFixture::Create();
  ASSERT_TRUE(static_cast<bool>(fixture));
  fixture->mgr_b->EnableNestedCarrierAccept(true);
  auto bob_addr = FormatAdpMultiaddr(fixture->addr_b, "QmBob");
  ASSERT_TRUE(static_cast<bool>(bob_addr));
  ASSERT_TRUE(static_cast<bool>(fixture->mgr_a->RegisterEndpoint("bob", *bob_addr)));
  bool associated = false;
  fixture->mgr_a->EnsureAssociation("bob", [&](PeerLinkManager::LinkRoe result) { associated = static_cast<bool>(result); });
  fixture->PumpUntil([&] { return associated; });
  ASSERT_TRUE(associated);

  // Shared key: an ADP dial to an address that never answers (the NAT'd target) …
  const std::string key = "QmTarget";
  auto dead = FormatAdpMultiaddr(adp::IpEndpoint::V4(10, 9, 9, 9, 999), "QmTarget");
  ASSERT_TRUE(static_cast<bool>(dead));
  ASSERT_TRUE(static_cast<bool>(fixture->mgr_a->RegisterEndpoint(key, *dead)));
  std::optional<bool> adp_ok;
  fixture->mgr_a->EnsureAssociation(key, [&](PeerLinkManager::LinkRoe result) { adp_ok = static_cast<bool>(result); });
  auto* adp_link = fixture->mgr_a->FindLink(key);
  ASSERT_NE(adp_link, nullptr);
  const LinkHandle adp_handle = adp_link->Handle();
  fixture->mgr_a->AbortInflightDial(key);  // failed; its drop waits for Tick
  ASSERT_TRUE(fixture->mgr_a->WithLiveLink(adp_handle, [](PeerLink&) {}));

  // … and, meanwhile, the nested link over a carrier under the same key.
  std::optional<uint32_t> channel_id;
  fixture->mgr_a->OpenChannel("bob", kAmpCircuitCarrierProtocolId, CircuitCarrierChannelPolicy(),
                              [&](PeerLinkManager::ChannelRoe ch) {
                                if (ch.isOk()) {
                                  channel_id = ch.value();
                                }
                              });
  const auto carrier_open = [&] {
    auto* outbound = fixture->mgr_a->FindLink("bob");
    return channel_id && outbound && outbound->Mux() && outbound->Mux()->State(*channel_id) == ChannelState::Open;
  };
  for (int i = 0; i < 40 && !carrier_open(); ++i) {  // Pump only: a Tick would drain the ADP drop now
    fixture->pump_a->Pump();
    fixture->pump_b->Pump();
  }
  ASSERT_TRUE(carrier_open());
  ASSERT_TRUE(channel_id.has_value());
  auto carrier = std::make_shared<ChannelSession>();
  carrier->Bind(*fixture->mgr_a->FindLink("bob")->Mux(), *channel_id, CircuitCarrierChannelPolicy(),
                [](Roe<std::vector<uint8_t>>) { return true; });
  bool nested_ok = false;
  fixture->mgr_a->EstablishNestedOverCarrier(key, carrier, true,
                                             [&](PeerLinkManager::LinkRoe result) { nested_ok = static_cast<bool>(result); });
  auto* nested_link = fixture->mgr_a->FindLink(key);
  ASSERT_NE(nested_link, nullptr);
  ASSERT_TRUE(nested_link->IsCarrierBacked());
  const LinkHandle nested_handle = nested_link->Handle();
  ASSERT_TRUE(fixture->mgr_a->WithLiveLink(adp_handle, [](PeerLink&) {})) << "drop still pending";

  fixture->pump_a->Tick();  // drains the aborted dial's drop while the nested link holds the key
  EXPECT_FALSE(fixture->mgr_a->WithLiveLink(adp_handle, [](PeerLink&) {})) << "the aborted dial is gone";
  EXPECT_TRUE(fixture->mgr_a->WithLiveLink(nested_handle, [](PeerLink&) {})) << "the nested link survives";
  fixture->PumpUntil([&] { return nested_ok; });
  EXPECT_TRUE(nested_ok) << "and completes";
  auto* holder = fixture->mgr_a->FindLink(key);
  ASSERT_NE(holder, nullptr) << "the key still names the nested link";
  EXPECT_TRUE(holder->IsCarrierBacked());
}

// A024: a nested establish to a key must not wait on an ADP dial under the same key. Hard lab
// (B-HARD-CALL-NAT-COLD): a cold ADP dial to the NAT'd answerer ran its full dial timeout and the
// nested link over the relay — which would answer at once — failed with it.
TEST(MeshLinkTest, NestedEstablishDoesNotWaitOnAnAdpDialUnderTheSameKey) {
  ASSERT_GE(sodium_init(), 0);
  auto fixture = MeshLinkFixture::Create();
  ASSERT_TRUE(static_cast<bool>(fixture));
  fixture->mgr_b->EnableNestedCarrierAccept(true);
  auto bob_addr = FormatAdpMultiaddr(fixture->addr_b, "QmBob");
  ASSERT_TRUE(static_cast<bool>(bob_addr));
  ASSERT_TRUE(static_cast<bool>(fixture->mgr_a->RegisterEndpoint("bob", *bob_addr)));
  bool associated = false;
  fixture->mgr_a->EnsureAssociation("bob", [&](PeerLinkManager::LinkRoe result) { associated = static_cast<bool>(result); });
  fixture->PumpUntil([&] { return associated; });
  ASSERT_TRUE(associated);

  // An ADP dial to an address that never answers stays Handshaking …
  const std::string key = "QmTarget";
  auto dead = FormatAdpMultiaddr(adp::IpEndpoint::V4(10, 9, 9, 9, 999), "QmTarget");
  ASSERT_TRUE(static_cast<bool>(dead));
  ASSERT_TRUE(static_cast<bool>(fixture->mgr_a->RegisterEndpoint(key, *dead)));
  std::optional<bool> adp_ok;
  fixture->mgr_a->EnsureAssociation(key, [&](PeerLinkManager::LinkRoe result) { adp_ok = static_cast<bool>(result); });
  auto* adp_link = fixture->mgr_a->FindLink(key);
  ASSERT_NE(adp_link, nullptr);
  const LinkHandle adp_handle = adp_link->Handle();

  // … while the nested link over a carrier to the same key is established.
  std::optional<uint32_t> channel_id;
  fixture->mgr_a->OpenChannel("bob", kAmpCircuitCarrierProtocolId, CircuitCarrierChannelPolicy(),
                              [&](PeerLinkManager::ChannelRoe ch) {
                                if (ch.isOk()) {
                                  channel_id = ch.value();
                                }
                              });
  fixture->PumpUntil([&] {
    auto* outbound = fixture->mgr_a->FindLink("bob");
    return channel_id && outbound && outbound->Mux() && outbound->Mux()->State(*channel_id) == ChannelState::Open;
  });
  ASSERT_TRUE(channel_id.has_value());
  auto carrier = std::make_shared<ChannelSession>();
  carrier->Bind(*fixture->mgr_a->FindLink("bob")->Mux(), *channel_id, CircuitCarrierChannelPolicy(),
                [](Roe<std::vector<uint8_t>>) { return true; });
  std::optional<bool> nested_ok;
  fixture->mgr_a->EstablishNestedOverCarrier(key, carrier, true,
                                             [&](PeerLinkManager::LinkRoe result) { nested_ok = static_cast<bool>(result); });
  fixture->PumpUntil([&] { return nested_ok.has_value(); });

  ASSERT_TRUE(nested_ok.has_value()) << "the nested handshake finished on its own, not with the ADP dial";
  EXPECT_TRUE(*nested_ok);
  EXPECT_FALSE(adp_ok.has_value()) << "the ADP dial's waiter waits for the ADP dial";
  EXPECT_TRUE(fixture->mgr_a->WithLiveLink(adp_handle, [](PeerLink& link) {
    EXPECT_EQ(link.Phase(), PeerLinkPhase::Handshaking);
  })) << "and that dial is still in flight";
}

/** Associate A→B and bring up a Connected nested link over a carrier channel (B accepts). */
struct ConnectedNested {
  uint32_t carrier_channel = 0;
  LinkHandle outer_b;  // B's end of the outer (ADP) link carrying the carrier
  std::string nested_key = "nested:bob";
};

std::optional<ConnectedNested> BringUpConnectedNested(MeshLinkFixture& f, const std::string& nested_key = "nested:bob") {
  f.mgr_b->EnableNestedCarrierAccept(true);
  auto bob_addr = FormatAdpMultiaddr(f.addr_b, "QmBob");
  if (!bob_addr || !f.mgr_a->RegisterEndpoint("bob", *bob_addr)) {
    return std::nullopt;
  }
  bool associated = false;
  f.mgr_a->EnsureAssociation("bob", [&](PeerLinkManager::LinkRoe result) { associated = static_cast<bool>(result); });
  f.PumpUntil([&] { return associated && f.mgr_b->FindConnectedInboundLink() != nullptr; });
  if (!associated) {
    return std::nullopt;
  }
  const LinkHandle outer_b = f.mgr_b->FindConnectedInboundLink()->Handle();
  std::optional<uint32_t> channel_id;
  f.mgr_a->OpenChannel("bob", kAmpCircuitCarrierProtocolId, CircuitCarrierChannelPolicy(),
                       [&](PeerLinkManager::ChannelRoe ch) {
                         if (ch.isOk()) {
                           channel_id = ch.value();
                         }
                       });
  f.PumpUntil([&] {
    auto* outbound = f.mgr_a->FindLink("bob");
    return channel_id && outbound && outbound->Mux() && outbound->Mux()->State(*channel_id) == ChannelState::Open;
  });
  if (!channel_id) {
    return std::nullopt;
  }
  auto carrier = std::make_shared<ChannelSession>();
  carrier->Bind(*f.mgr_a->FindLink("bob")->Mux(), *channel_id, CircuitCarrierChannelPolicy(),
                [](Roe<std::vector<uint8_t>>) { return true; });
  ConnectedNested out;
  out.carrier_channel = *channel_id;
  out.outer_b = outer_b;
  out.nested_key = nested_key;
  bool nested_ok = false;
  f.mgr_a->EstablishNestedOverCarrier(out.nested_key, carrier, true,
                                      [&](PeerLinkManager::LinkRoe result) { nested_ok = static_cast<bool>(result); });
  f.PumpUntil([&] { return nested_ok; });
  if (!nested_ok) {
    return std::nullopt;
  }
  return out;
}

/** The carrier-backed link on A under `key`, if any. */
PeerLink* FindNestedLink(MeshLinkFixture& f, const std::string& key) {
  auto* link = f.mgr_a->FindLink(key);
  return link && link->IsCarrierBacked() ? link : nullptr;
}

/** Pump both sides while virtual time moves, so resend timers fire. */
void PumpAdvancing(MeshLinkFixture& f, const std::function<bool()>& done, const int rounds = 2000,
                   const int64_t step_ms = 10) {
  for (int i = 0; i < rounds && !done(); ++i) {
    f.clock->Advance(step_ms);
    f.PumpBoth();
  }
}

/**
 * PumpAdvancing for a lossy fixture: each round also releases what the reorder windows hold, so
 * datagrams are reordered within a round (10 ms) but never parked until later traffic pushes them
 * out — a quiet link would otherwise hold its last few datagrams indefinitely.
 */
void PumpLossy(MeshLinkFixture& f, const std::function<bool()>& done, const int rounds = 2000) {
  for (int i = 0; i < rounds && !done(); ++i) {
    f.clock->Advance(10);
    f.PumpBoth();
    f.lossy_a->FlushReorder();
    f.lossy_b->FlushReorder();
  }
}

/** Reliable channel on A's nested link to B; B records every message it gets. */
struct NestedReliableChannel {
  uint32_t channel_id = 0;
  std::shared_ptr<std::vector<std::string>> received = std::make_shared<std::vector<std::string>>();
  /** B's end of the nested link (the channel's open handler names it). */
  std::shared_ptr<LinkHandle> nested_b = std::make_shared<LinkHandle>();
};

constexpr const char* kNestedReliableProtocol = "/test/nested-reliable/1";

ChannelPolicy NestedReliablePolicy() {
  ChannelPolicy policy = ControlJsonChannelPolicy();
  policy.read_once = false;
  return policy;
}

std::optional<NestedReliableChannel> OpenNestedReliableChannel(MeshLinkFixture& f, const std::string& nested_key) {
  NestedReliableChannel out;
  f.mgr_b->SetProtocolHandler(kNestedReliableProtocol, [&f, received = out.received, nested_b = out.nested_b](
                                                          LinkHandle link, const std::string&, uint32_t ch) {
    *nested_b = link;
    f.mgr_b->WithLiveLink(link, [&](PeerLink& nested_b) {
      nested_b.Mux()->SetDataHandler(ch, [received](uint32_t, std::vector<uint8_t> payload) {
        received->emplace_back(payload.begin(), payload.end());
      });
    });
  });
  std::optional<uint32_t> id;
  f.mgr_a->OpenChannel(nested_key, kNestedReliableProtocol, NestedReliablePolicy(),
                       [&](PeerLinkManager::ChannelRoe ch) {
                         if (ch.isOk()) {
                           id = ch.value();
                         }
                       });
  PumpAdvancing(f, [&] {
    auto* nested = FindNestedLink(f, nested_key);
    return id && nested && nested->Mux()->State(*id) == ChannelState::Open;
  });
  if (!id) {
    return std::nullopt;
  }
  out.channel_id = *id;
  return out;
}

// Reliable delivery over a nested link (ADR_LINK_PLANE §11): the relay splices a best-effort
// carrier, so without the lane one lost or reordered frame wedged a Reliable channel for good
// (call control / chat / hello over a relay — lab `delay 120ms 30ms`).
TEST(MeshLinkTest, NestedReliableChannelSurvivesLossAndReorderingOnTheCarrier) {
  ASSERT_GE(sodium_init(), 0);
  auto fixture = MeshLinkFixture::Create(/*lossy=*/true);
  ASSERT_TRUE(static_cast<bool>(fixture));
  auto nested = BringUpConnectedNested(*fixture);
  ASSERT_TRUE(nested.has_value());
  auto channel = OpenNestedReliableChannel(*fixture, nested->nested_key);
  ASSERT_TRUE(channel.has_value());
  auto* nested_a = FindNestedLink(*fixture, nested->nested_key);
  ASSERT_NE(nested_a, nullptr);
  EXPECT_TRUE(nested_a->CarrierLaneActive()) << "both ends announced the lane during the handshake";

  // The outer link is hot, as a relay link holding a circuit is in the product, and both ends have
  // heard each other's cadence before the loss starts (liveness window 5/2 × cadence): what is
  // tested is the lane, not a cold outer link timing out.
  fixture->mgr_a->MarkHot("bob");
  fixture->mgr_b->MarkHot("QmAlice");
  for (int i = 0; i < 20; ++i) {
    fixture->clock->Advance(10);
    fixture->PumpBoth();
  }
  for (auto* lossy : {fixture->lossy_a.get(), fixture->lossy_b.get()}) {
    lossy->SetRngSeed(7);
    lossy->SetDropRate(0.2);
    lossy->SetReorderWindow(3);
  }
  std::vector<std::string> sent;
  for (int i = 0; i < 40; ++i) {
    sent.push_back("msg-" + std::to_string(i));
    auto* link = FindNestedLink(*fixture, nested->nested_key);
    ASSERT_NE(link, nullptr);
    const std::vector<uint8_t> bytes(sent.back().begin(), sent.back().end());
    ASSERT_TRUE(static_cast<bool>(link->Mux()->SendData(channel->channel_id, bytes))) << i;
    fixture->clock->Advance(5);
    fixture->PumpBoth();
  }
  PumpLossy(*fixture, [&] { return channel->received->size() >= sent.size(); });
  EXPECT_EQ(*channel->received, sent) << "every message, once, in order";
  auto* link = FindNestedLink(*fixture, nested->nested_key);
  ASSERT_NE(link, nullptr) << "the link survived";
  EXPECT_FALSE(link->CarrierLaneFailed());
}

// The relay stops forwarding while both outer links stay up: no resend of a reliable frame gets
// through, so the end-to-end path is dead — the nested link is dropped (connection-dead) instead
// of holding a wedged channel while its carrier looks fine.
TEST(MeshLinkTest, NestedLinkIsDroppedWhenItsReliableLaneGivesUp) {
  ASSERT_GE(sodium_init(), 0);
  auto fixture = MeshLinkFixture::Create();
  ASSERT_TRUE(static_cast<bool>(fixture));
  auto nested = BringUpConnectedNested(*fixture);
  ASSERT_TRUE(nested.has_value());
  auto channel = OpenNestedReliableChannel(*fixture, nested->nested_key);
  ASSERT_TRUE(channel.has_value());
  auto* nested_a = FindNestedLink(*fixture, nested->nested_key);
  ASSERT_NE(nested_a, nullptr);
  const LinkHandle handle = nested_a->Handle();
  // Outer links stay alive (keepalives both ways) …
  fixture->mgr_a->MarkHot("bob");
  fixture->mgr_b->MarkHot("QmAlice");
  // … while B's end of the carrier silently swallows everything (a relay that stopped splicing).
  ASSERT_TRUE(fixture->mgr_b->WithLiveLink(*channel->nested_b, [](PeerLink& nested_b) {
    nested_b.Carrier()->SetFrameHandler([](Roe<std::vector<uint8_t>>) { return true; });
  }));
  std::vector<LinkEvent> events;
  fixture->mgr_a->AddLinkEventListener([&](const LinkEvent& event) { events.push_back(event); });

  const std::vector<uint8_t> bytes = {'x'};
  ASSERT_TRUE(static_cast<bool>(nested_a->Mux()->SendData(channel->channel_id, bytes)));
  PumpAdvancing(*fixture, [&] { return !fixture->mgr_a->WithLiveLink(handle, [](PeerLink&) {}); }, 6000);
  EXPECT_FALSE(fixture->mgr_a->WithLiveLink(handle, [](PeerLink&) {}));
  EXPECT_TRUE(fixture->mgr_a->IsConnected("bob")) << "the outer link is fine";
  fixture->PumpBoth();  // listeners run off-strand
  bool dead = false;
  for (const auto& event : events) {
    dead |= event.kind == LinkEvent::Kind::Dropped && event.handle == handle &&
            event.reason == LinkDropReason::ConnectionDead;
  }
  EXPECT_TRUE(dead);
}

// A024: an ADP and a nested link to one peer coexist; asking for a transport class gets that one.
TEST(MeshLinkTest, ConnectedLinkByPeerIdHonoursTheTransportClass) {
  ASSERT_GE(sodium_init(), 0);
  auto fixture = MeshLinkFixture::Create();
  ASSERT_TRUE(static_cast<bool>(fixture));
  auto nested = BringUpConnectedNested(*fixture);
  ASSERT_TRUE(nested.has_value());
  auto* nested_a = FindNestedLink(*fixture, nested->nested_key);
  ASSERT_NE(nested_a, nullptr);
  const std::string bob = nested_a->RemotePeerId();
  ASSERT_FALSE(bob.empty());
  auto* adp = fixture->mgr_a->FindConnectedLinkByPeerId(bob, TransportClass::Adp);
  auto* carrier = fixture->mgr_a->FindConnectedLinkByPeerId(bob, TransportClass::Carrier);
  ASSERT_NE(adp, nullptr);
  ASSERT_NE(carrier, nullptr);
  EXPECT_FALSE(adp->IsCarrierBacked());
  EXPECT_TRUE(carrier->IsCarrierBacked());
  EXPECT_EQ(carrier, nested_a);
  EXPECT_EQ(fixture->mgr_a->FindConnectedLinkByPeerId("QmNobody", TransportClass::Adp), nullptr);
}

// A024: a nested link comes up under a key whose ADP link is already Connected — a call on a
// direct link that wants a relayed one beside it. It used to report OK at once (the key was
// "connected") without building the nested link, orphaning the carrier.
TEST(MeshLinkTest, NestedLinkComesUpBesideAConnectedAdpLinkUnderTheSameKey) {
  ASSERT_GE(sodium_init(), 0);
  auto fixture = MeshLinkFixture::Create();
  ASSERT_TRUE(static_cast<bool>(fixture));
  auto nested = BringUpConnectedNested(*fixture, /*nested_key=*/"bob");  // same key as the ADP link
  ASSERT_TRUE(nested.has_value());
  auto* adp = fixture->mgr_a->FindLink("bob");
  ASSERT_NE(adp, nullptr);
  const std::string bob = adp->RemotePeerId().empty() ? std::string("QmBob") : adp->RemotePeerId();
  auto* carrier = fixture->mgr_a->FindConnectedLinkByPeerId(bob, TransportClass::Carrier);
  auto* direct = fixture->mgr_a->FindConnectedLinkByPeerId(bob, TransportClass::Adp);
  ASSERT_NE(carrier, nullptr) << "the nested link exists";
  ASSERT_NE(direct, nullptr) << "and the ADP link is still there";
  EXPECT_TRUE(carrier->IsCarrierBacked());
  EXPECT_FALSE(direct->IsCarrierBacked());
}

// k1 (call-path-resilience): a Connected nested link whose carrier closes used to drop to Backoff
// and linger there forever — Tick only evicted Connected carrier links. It must be dropped with
// reason CarrierClosed.
TEST(MeshLinkTest, ConnectedNestedLinkIsDroppedWhenItsCarrierCloses) {
  ASSERT_GE(sodium_init(), 0);
  auto fixture = MeshLinkFixture::Create();
  ASSERT_TRUE(static_cast<bool>(fixture));
  auto nested = BringUpConnectedNested(*fixture);
  ASSERT_TRUE(nested.has_value());
  auto* nested_link = FindNestedLink(*fixture, nested->nested_key);
  ASSERT_NE(nested_link, nullptr);
  ASSERT_EQ(nested_link->Phase(), PeerLinkPhase::Connected);
  const LinkHandle nested_handle = nested_link->Handle();

  std::vector<LinkEvent> events;
  fixture->mgr_a->AddLinkEventListener([&](const LinkEvent& event) { events.push_back(event); });
  // B's outer (ADP) link carries the carrier channel; its nested inbound link is another link.
  bool reset = false;
  ASSERT_TRUE(fixture->mgr_b->WithLiveLink(nested->outer_b, [&](PeerLink& outer_b) {
    reset = !outer_b.IsCarrierBacked() && static_cast<bool>(outer_b.Mux()->ResetChannel(nested->carrier_channel));
  }));
  ASSERT_TRUE(reset);
  for (int i = 0; i < 5; ++i) {
    fixture->PumpBoth();
  }

  EXPECT_FALSE(fixture->mgr_a->WithLiveLink(nested_handle, [](PeerLink&) {})) << "no Backoff linger";
  bool dropped = false;
  for (const auto& event : events) {
    if (event.kind == LinkEvent::Kind::Dropped && event.reason == LinkDropReason::CarrierClosed && event.was_connected) {
      dropped = true;
    }
  }
  EXPECT_TRUE(dropped) << "one Dropped(CarrierClosed) for the connected nested link";
  EXPECT_NE(fixture->mgr_a->FindLink("bob"), nullptr) << "the outer link is untouched";
}

// k1 (call-path-resilience): an inbound link whose handshake failed stayed in Backoff under its
// `inbound:` key until something displaced it. It must be dropped (HandshakeFailed).
TEST(MeshLinkTest, InboundLinkIsDroppedWhenItsHandshakeFails) {
  ASSERT_GE(sodium_init(), 0);
  auto fixture = MeshLinkFixture::Create();
  ASSERT_TRUE(static_cast<bool>(fixture));
  // A signs with a secret key that does not match the public key it presents: B's verify fails.
  MshIdentity forged = fixture->alice;
  forged.ml_dsa_public_key = fixture->bob.ml_dsa_public_key;
  fixture->pump_a.reset();
  fixture->mgr_a = std::make_unique<PeerLinkManager>(*fixture->ep_a, forged, "QmAlice");
  fixture->pump_a = std::make_unique<MeshPump>(*fixture->ep_a, *fixture->mgr_a);

  std::vector<LinkEvent> events_b;
  fixture->mgr_b->AddLinkEventListener([&](const LinkEvent& event) { events_b.push_back(event); });
  auto bob_addr = FormatAdpMultiaddr(fixture->addr_b, "QmBob");
  ASSERT_TRUE(static_cast<bool>(bob_addr));
  ASSERT_TRUE(static_cast<bool>(fixture->mgr_a->RegisterEndpoint("bob", *bob_addr)));
  std::optional<bool> associated;
  fixture->mgr_a->EnsureAssociation("bob", [&](PeerLinkManager::LinkRoe result) { associated = static_cast<bool>(result); });
  const auto inbound_failed = [&] {
    for (const auto& event : events_b) {
      if (event.kind == LinkEvent::Kind::Dropped && !event.was_connected &&
          event.reason == LinkDropReason::HandshakeFailed) {
        return true;
      }
    }
    return false;
  };
  fixture->PumpUntil(inbound_failed, 200);
  EXPECT_TRUE(inbound_failed()) << "B drops the inbound link its handshake failed on";
  EXPECT_EQ(fixture->mgr_b->FindConnectedInboundLink(), nullptr);
}

// k1 snapshot fields: kind (Direct / Punched / Carrier), the live remote endpoint and RX age.
TEST(MeshLinkTest, SnapshotReportsPathKindRemoteAndRxAge) {
  ASSERT_GE(sodium_init(), 0);
  auto fixture = MeshLinkFixture::Create();
  ASSERT_TRUE(static_cast<bool>(fixture));
  auto nested = BringUpConnectedNested(*fixture);
  ASSERT_TRUE(nested.has_value());

  fixture->clock->Advance(300);
  const auto direct = fixture->mgr_a->GetSnapshotByDialKey("bob");
  EXPECT_EQ(direct.path_kind, LinkPathKind::Direct) << "dialed, not punched";
  ASSERT_TRUE(direct.remote.has_value());
  EXPECT_EQ(*direct.remote, fixture->addr_b);
  EXPECT_GE(direct.last_rx_age_ms, 300);

  const auto carrier = fixture->mgr_a->GetSnapshotByDialKey(nested->nested_key);
  EXPECT_EQ(carrier.path_kind, LinkPathKind::Carrier);
  EXPECT_FALSE(carrier.remote.has_value());
}

// k1 (call-path-resilience, #215 B39 a): the OS reports no route to the peer — drop the link at
// once (TransportFailed) instead of after the liveness window, so a redial can pick another path.
TEST(MeshLinkTest, LinkIsDroppedAtOnceWhenTheOsReportsThePeerUnreachable) {
  ASSERT_GE(sodium_init(), 0);
  auto fixture = MeshLinkFixture::Create();
  ASSERT_TRUE(static_cast<bool>(fixture));
  auto bob_addr = FormatAdpMultiaddr(fixture->addr_b, "QmBob");
  ASSERT_TRUE(static_cast<bool>(bob_addr));
  ASSERT_TRUE(static_cast<bool>(fixture->mgr_a->RegisterEndpoint("bob", *bob_addr)));
  bool associated = false;
  fixture->mgr_a->EnsureAssociation("bob", [&](PeerLinkManager::LinkRoe result) { associated = static_cast<bool>(result); });
  fixture->PumpUntil([&] { return associated; });
  ASSERT_TRUE(associated);
  std::vector<LinkEvent> events;
  fixture->mgr_a->AddLinkEventListener([&](const LinkEvent& event) { events.push_back(event); });

  fixture->io_a->SetUnreachable(fixture->addr_b, true);
  std::optional<PeerLinkManager::ChannelRoe> opened;  // any send on the link hits the route error
  fixture->mgr_a->OpenChannel("bob", "/pp-test/any/1.0.0", ControlJsonChannelPolicy(),
                              [&](PeerLinkManager::ChannelRoe ch) { opened = std::move(ch); });
  fixture->pump_a->Pump();
  fixture->pump_a->Tick();  // no clock advance: well inside the liveness window

  EXPECT_EQ(fixture->mgr_a->FindLink("bob"), nullptr);
  bool dropped = false;
  for (const auto& event : events) {
    dropped = dropped || (event.kind == LinkEvent::Kind::Dropped && event.reason == LinkDropReason::TransportFailed);
  }
  EXPECT_TRUE(dropped);
}

// Product warms a peer before dialing it (chat foreground); the tier must not be lost.
// pp-browser B39: product drops a link it knows is stale; both ends evict it, reason "requested".
TEST(MeshLinkTest, RequestDropLinkEvictsBothEnds) {
  ASSERT_GE(sodium_init(), 0);
  auto fixture = MeshLinkFixture::Create();
  ASSERT_TRUE(static_cast<bool>(fixture));
  auto bob_addr = FormatAdpMultiaddr(fixture->addr_b, "QmBob");
  ASSERT_TRUE(static_cast<bool>(bob_addr));
  ASSERT_TRUE(static_cast<bool>(fixture->mgr_a->RegisterEndpoint("bob", *bob_addr)));
  bool associated = false;
  fixture->mgr_a->EnsureAssociation("bob", [&](PeerLinkManager::LinkRoe result) { associated = static_cast<bool>(result); });
  fixture->PumpUntil([&] { return associated && fixture->mgr_b->FindConnectedInboundLink() != nullptr; });
  ASSERT_TRUE(associated);
  const std::string bob_peer_id = fixture->mgr_a->FindLink("bob")->RemotePeerId();

  std::vector<LinkEvent> dropped;
  fixture->mgr_a->AddLinkEventListener([&](const LinkEvent& event) {
    if (event.kind == LinkEvent::Kind::Dropped) {
      dropped.push_back(event);
    }
  });
  // By PeerId (what the call bridge knows), not the dial alias.
  EXPECT_EQ(fixture->mgr_a->RequestDropLink(bob_peer_id), 1u);
  EXPECT_TRUE(fixture->mgr_a->IsConnected("bob")) << "scheduled, not inline";
  fixture->PumpBoth();
  EXPECT_FALSE(fixture->mgr_a->IsConnected("bob"));
  ASSERT_EQ(dropped.size(), 1u);
  EXPECT_EQ(dropped[0].reason, LinkDropReason::Requested);
  fixture->PumpUntil([&] { return fixture->mgr_b->FindConnectedInboundLink() == nullptr; });
  EXPECT_EQ(fixture->mgr_b->FindConnectedInboundLink(), nullptr) << "peer evicts on our Close";
  EXPECT_EQ(fixture->mgr_a->RequestDropLink("nobody"), 0u);
}

// A dropped link destroys its mux: channel sessions bound to it must hear "link-dropped" and forget
// the mux — they used to keep a dangling pointer and write / unbind through it (UAF, SIGSEGV).
TEST(MeshLinkTest, DroppedLinkClosesItsChannelSessions) {
  ASSERT_GE(sodium_init(), 0);
  auto fixture = MeshLinkFixture::Create();
  ASSERT_TRUE(static_cast<bool>(fixture));
  auto bob_addr = FormatAdpMultiaddr(fixture->addr_b, "QmBob");
  ASSERT_TRUE(static_cast<bool>(bob_addr));
  ASSERT_TRUE(static_cast<bool>(fixture->mgr_a->RegisterEndpoint("bob", *bob_addr)));
  fixture->mgr_b->SetProtocolHandler("/served/1", [](LinkHandle, const std::string&, uint32_t) {});
  bool associated = false;
  fixture->mgr_a->EnsureAssociation("bob", [&](PeerLinkManager::LinkRoe result) { associated = static_cast<bool>(result); });
  fixture->PumpUntil([&] { return associated && fixture->mgr_b->FindConnectedInboundLink() != nullptr; });
  ASSERT_TRUE(associated);

  std::optional<uint32_t> id;
  fixture->mgr_a->OpenChannel("bob", "/served/1", ControlJsonChannelPolicy(), [&](PeerLinkManager::ChannelRoe ch) {
    if (ch.isOk()) {
      id = ch.value();
    }
  });
  fixture->PumpUntil([&] {
    auto* link = fixture->mgr_a->FindLink("bob");
    return id && link && link->Mux() && link->Mux()->State(*id) == ChannelState::Open;
  });
  ASSERT_TRUE(id.has_value());
  std::string closed_reason;
  auto session = fixture->mgr_a->BindChannel("bob", *id, ControlJsonChannelPolicy(),
                                             [](Roe<std::vector<uint8_t>>) { return true; },
                                             [&](const char* reason) { closed_reason = reason ? reason : ""; });
  ASSERT_NE(session, nullptr);

  const std::string bob_peer_id = fixture->mgr_a->FindLink("bob")->RemotePeerId();
  ASSERT_EQ(fixture->mgr_a->RequestDropLink(bob_peer_id), 1u);
  fixture->PumpBoth();
  ASSERT_FALSE(fixture->mgr_a->IsConnected("bob"));
  EXPECT_EQ(closed_reason, ChannelMux::kLinkDroppedReason);
  EXPECT_TRUE(session->IsClosed());
  EXPECT_FALSE(session->EnqueueOutbound({1, 2, 3})) << "no write through the destroyed mux";
  EXPECT_EQ(session->Mux(), nullptr) << "the session forgot the destroyed mux";
  session.reset();  // destruction must not unbind through the destroyed mux either
}

// BurstDial leaves amp:burst:* records in the DialBook; inbound adopt must not take them as the new
// link's alias (pp-browser dogfood 2026-09-24: an aborted punch key named a relay carrier link).
TEST(MeshLinkTest, InboundAdoptSkipsEphemeralBurstAlias) {
  ASSERT_GE(sodium_init(), 0);
  auto fixture = MeshLinkFixture::Create();
  ASSERT_TRUE(static_cast<bool>(fixture));
  auto bob_addr = FormatAdpMultiaddr(fixture->addr_b, "QmBob");
  ASSERT_TRUE(static_cast<bool>(bob_addr));
  ASSERT_TRUE(static_cast<bool>(fixture->mgr_a->RegisterEndpoint("bob", *bob_addr)));
  bool associated = false;
  fixture->mgr_a->EnsureAssociation("bob", [&](PeerLinkManager::LinkRoe result) { associated = static_cast<bool>(result); });
  fixture->PumpUntil([&] { return associated && fixture->mgr_b->FindConnectedInboundLink() != nullptr; });
  ASSERT_TRUE(associated);
  const std::string alice_id = fixture->mgr_b->FindConnectedInboundLink()->RemotePeerId();
  ASSERT_FALSE(alice_id.empty());

  // Drop, then leave an aborted-punch style record for Alice in Bob's book.
  ASSERT_EQ(fixture->mgr_a->RequestDropLink("bob"), 1u);
  fixture->PumpUntil([&] { return fixture->mgr_b->FindConnectedInboundLink() == nullptr; });
  auto alice_ma = FormatAdpMultiaddr(fixture->addr_a, alice_id);
  ASSERT_TRUE(static_cast<bool>(alice_ma));
  ASSERT_TRUE(static_cast<bool>(
      fixture->mgr_b->RegisterEndpoint(std::string(kBurstDialKeyPrefix) + "0:" + alice_id.substr(0, 12), *alice_ma)));

  associated = false;
  fixture->mgr_a->EnsureAssociation("bob", [&](PeerLinkManager::LinkRoe result) { associated = static_cast<bool>(result); });
  fixture->PumpUntil([&] { return associated && fixture->mgr_b->FindConnectedInboundLink() != nullptr; });
  ASSERT_TRUE(associated);
  auto* inbound = fixture->mgr_b->FindConnectedInboundLink();
  ASSERT_NE(inbound, nullptr);
  EXPECT_FALSE(IsEphemeralDialKey(inbound->PeerKey())) << inbound->PeerKey();
}

TEST(MeshLinkTest, MarkWarmBeforeAssociationAppliesOnConnect) {
  ASSERT_GE(sodium_init(), 0);
  auto fixture = MeshLinkFixture::Create();
  ASSERT_TRUE(static_cast<bool>(fixture));
  fixture->mgr_a->MarkWarm("bob");
  ASSERT_EQ(fixture->mgr_a->FindLink("bob"), nullptr);

  auto bob_addr = FormatAdpMultiaddr(fixture->addr_b, "QmBob");
  ASSERT_TRUE(static_cast<bool>(bob_addr));
  ASSERT_TRUE(static_cast<bool>(fixture->mgr_a->RegisterEndpoint("bob", *bob_addr)));
  bool associated = false;
  fixture->mgr_a->EnsureAssociation("bob", [&](PeerLinkManager::LinkRoe result) { associated = static_cast<bool>(result); });
  fixture->PumpUntil([&] { return associated; });
  ASSERT_TRUE(associated);
  auto* link = fixture->mgr_a->FindLink("bob");
  ASSERT_NE(link, nullptr);
  EXPECT_EQ(link->GetKeepaliveTier(), KeepaliveTier::Warm);
}

TEST(MeshLinkTest, LinkEventsConnectedThenDeadDrop) {
  ASSERT_GE(sodium_init(), 0);
  auto fixture = MeshLinkFixture::Create();
  ASSERT_TRUE(static_cast<bool>(fixture));

  std::vector<LinkEvent> events_a;
  std::vector<LinkEvent> events_b;
  fixture->mgr_a->AddLinkEventListener([&](const LinkEvent& event) { events_a.push_back(event); });
  const auto id_b =
      fixture->mgr_b->AddLinkEventListener([&](const LinkEvent& event) { events_b.push_back(event); });

  auto bob_addr = FormatAdpMultiaddr(fixture->addr_b, "QmBob");
  ASSERT_TRUE(static_cast<bool>(bob_addr));
  ASSERT_TRUE(static_cast<bool>(fixture->mgr_a->RegisterEndpoint("bob", *bob_addr)));
  bool associated = false;
  fixture->mgr_a->EnsureAssociation("bob", [&](PeerLinkManager::LinkRoe result) { associated = static_cast<bool>(result); });
  fixture->PumpUntil([&] {
    return associated && fixture->mgr_b->FindConnectedInboundLink() != nullptr;
  });
  ASSERT_TRUE(associated);

  ASSERT_EQ(events_a.size(), 1u);
  EXPECT_EQ(events_a[0].kind, LinkEvent::Kind::Connected);
  EXPECT_EQ(events_a[0].dial_key, "bob");
  EXPECT_TRUE(events_a[0].outbound);
  EXPECT_EQ(events_a[0].transport, TransportClass::Adp);
  EXPECT_FALSE(events_a[0].peer_id.empty());
  ASSERT_TRUE(events_a[0].remote.has_value());
  EXPECT_EQ(*events_a[0].remote, fixture->addr_b);
  ASSERT_EQ(events_b.size(), 1u);
  EXPECT_EQ(events_b[0].kind, LinkEvent::Kind::Connected);
  EXPECT_FALSE(events_b[0].outbound);
  // Inbound dial key = "inbound:" + assoc id as lowercase hex (was '0'+nibble → ":;<=>?").
  ASSERT_EQ(events_b[0].dial_key.size(), std::string("inbound:").size() + 32);
  EXPECT_EQ(events_b[0].dial_key.rfind("inbound:", 0), 0u);
  EXPECT_EQ(events_b[0].dial_key.find_first_not_of("0123456789abcdef", 8), std::string::npos)
      << events_b[0].dial_key;
  fixture->mgr_b->RemoveLinkEventListener(id_b);

  // Cold link, no traffic past kAliveTimeoutMs → Tick evicts it as dead.
  fixture->clock->Advance(adp::kAliveTimeoutMs + 1000);
  fixture->pump_a->Tick();
  ASSERT_EQ(events_a.size(), 2u);
  EXPECT_EQ(events_a[1].kind, LinkEvent::Kind::Dropped);
  EXPECT_EQ(events_a[1].reason, LinkDropReason::ConnectionDead);
  EXPECT_TRUE(events_a[1].was_connected);
  EXPECT_EQ(events_a[1].handle, events_a[0].handle);
  EXPECT_GE(events_a[1].last_rx_age_ms, adp::kAliveTimeoutMs);
  EXPECT_EQ(events_b.size(), 1u) << "removed listener must not fire";
}

namespace {

/** A hot A→B link on a lossy fixture (losses off). Returns A's link handle. */
LinkHandle ConnectHotLink(MeshLinkFixture& f) {
  auto bob_addr = FormatAdpMultiaddr(f.addr_b, "QmBob");
  EXPECT_TRUE(static_cast<bool>(bob_addr));
  EXPECT_TRUE(static_cast<bool>(f.mgr_a->RegisterEndpoint("bob", *bob_addr)));
  bool associated = false;
  f.mgr_a->EnsureAssociation("bob", [&](PeerLinkManager::LinkRoe result) { associated = static_cast<bool>(result); });
  f.PumpUntil([&] { return associated && f.mgr_b->FindConnectedInboundLink() != nullptr; });
  EXPECT_TRUE(associated);
  f.mgr_a->MarkHot("bob");
  for (int i = 0; i < 10; ++i) {  // settle: nothing of the handshake still in flight
    f.clock->Advance(100);
    f.PumpBoth();
  }
  auto* link = f.mgr_a->FindLink("bob");
  EXPECT_NE(link, nullptr);
  return link ? link->Handle() : LinkHandle{};
}

} // namespace

// A network change probes every ADP link; one whose peer no longer answers is evicted after the
// grace (2 s) — not after its hot liveness window (50 s here).
TEST(MeshLinkTest, NetworkChangeEvictsASilentLinkWithinTheGrace) {
  ASSERT_GE(sodium_init(), 0);
  auto fixture = MeshLinkFixture::Create(/*lossy=*/true);
  ASSERT_TRUE(static_cast<bool>(fixture));
  const LinkHandle handle = ConnectHotLink(*fixture);
  std::vector<LinkEvent> events;
  fixture->mgr_a->AddLinkEventListener([&](const LinkEvent& event) { events.push_back(event); });

  fixture->lossy_b->SetDropRate(1.0);  // B's replies no longer reach A
  // Without a network change, 10 s of silence is well inside the hot window.
  for (int i = 0; i < 20; ++i) {
    fixture->clock->Advance(500);
    fixture->PumpBoth();
  }
  ASSERT_NE(fixture->mgr_a->FindLink("bob"), nullptr) << "hot link survives 10 s of silence";

  EXPECT_EQ(fixture->mgr_a->OnNetworkChanged(), 1u);
  bool dropped = false;
  int elapsed_ms = 0;
  for (; elapsed_ms <= 3000 && !dropped; elapsed_ms += 100) {
    fixture->clock->Advance(100);
    fixture->PumpBoth();
    for (const auto& event : events) {
      dropped = dropped || (event.kind == LinkEvent::Kind::Dropped && event.handle == handle);
    }
  }
  ASSERT_TRUE(dropped) << "evicted within the grace";
  EXPECT_GE(elapsed_ms, 2000);
  EXPECT_LE(elapsed_ms, 2200);
  EXPECT_EQ(events.back().reason, LinkDropReason::NetworkChanged);
  EXPECT_STREQ(LinkDropReasonName(events.back().reason), "network-changed");
}

// A link whose peer answers the probe stays; it is no longer suspect.
TEST(MeshLinkTest, NetworkChangeKeepsALinkThatAnswers) {
  ASSERT_GE(sodium_init(), 0);
  auto fixture = MeshLinkFixture::Create(/*lossy=*/true);
  ASSERT_TRUE(static_cast<bool>(fixture));
  const LinkHandle handle = ConnectHotLink(*fixture);

  EXPECT_EQ(fixture->mgr_a->OnNetworkChanged(), 1u);
  auto* link = fixture->mgr_a->FindLink("bob");
  ASSERT_NE(link, nullptr);
  EXPECT_NE(link->SuspectSinceMs(), 0);
  for (int i = 0; i < 40; ++i) {  // 4 s: twice the grace
    fixture->clock->Advance(100);
    fixture->PumpBoth();
  }
  link = fixture->mgr_a->FindLink("bob");
  ASSERT_NE(link, nullptr) << "the peer echoed the probe";
  EXPECT_EQ(link->Handle(), handle);
  EXPECT_EQ(link->SuspectSinceMs(), 0);
  EXPECT_EQ(link->Phase(), PeerLinkPhase::Connected);
}

// Only the probe's echo clears suspicion: a lost first probe is resent within the grace.
TEST(MeshLinkTest, NetworkChangeResendsALostProbe) {
  ASSERT_GE(sodium_init(), 0);
  auto fixture = MeshLinkFixture::Create(/*lossy=*/true);
  ASSERT_TRUE(static_cast<bool>(fixture));
  ConnectHotLink(*fixture);

  fixture->lossy_a->SetDropRate(1.0);  // the first probe is lost
  EXPECT_EQ(fixture->mgr_a->OnNetworkChanged(), 1u);
  fixture->clock->Advance(100);
  fixture->PumpBoth();
  fixture->lossy_a->SetDropRate(0.0);
  for (int i = 0; i < 40; ++i) {
    fixture->clock->Advance(100);
    fixture->PumpBoth();
  }
  auto* link = fixture->mgr_a->FindLink("bob");
  ASSERT_NE(link, nullptr) << "a resent probe got through";
  EXPECT_EQ(link->SuspectSinceMs(), 0);
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
  // k1 snapshot fields: a link that came up through the burst is labelled Punched.
  const auto snap = harness->runtime_a->SnapshotByPeerId(harness->peer_id_b);
  EXPECT_EQ(snap.path_kind, LinkPathKind::Punched);
  EXPECT_TRUE(snap.remote.has_value());
  EXPECT_GE(snap.last_rx_age_ms, 0);
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
