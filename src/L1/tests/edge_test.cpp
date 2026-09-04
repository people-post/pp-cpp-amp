#include "amp/L1/Clock.h"
#include "amp/L1/Endpoint.h"
#include "amp/L1/HmacBinder.h"
#include "amp/L1/MemoryDatagramIo.h"
#include "amp/L1/Types.h"
#include "amp/L1/WireCodec.h"

#include <gtest/gtest.h>
#include <sodium.h>

#include <string>
#include <vector>

namespace {

pp::adp::PeerKey Key(uint8_t v = 9) {
  pp::adp::PeerKey k;
  k.bytes.fill(v);
  return k;
}

pp::adp::AssocId Aid(uint8_t v = 3) {
  pp::adp::AssocId id;
  id.bytes.fill(v);
  return id;
}

struct Pair {
  std::shared_ptr<pp::adp::VirtualClock> clock;
  std::shared_ptr<pp::adp::MemoryDatagramHub> hub;
  std::shared_ptr<pp::adp::MemoryDatagramIo> io_a;
  std::shared_ptr<pp::adp::MemoryDatagramIo> io_b;
  std::unique_ptr<pp::adp::Endpoint> ep_a;
  std::unique_ptr<pp::adp::Endpoint> ep_b;
  pp::adp::IpEndpoint addr_a;
  pp::adp::IpEndpoint addr_b;
};

Pair MakePair() {
  Pair p;
  p.clock = std::make_shared<pp::adp::VirtualClock>(1'000'000);
  p.hub = pp::adp::MemoryDatagramIo::MakeHub();
  p.addr_a = pp::adp::IpEndpoint::V4(10, 0, 0, 1, 1000);
  p.addr_b = pp::adp::IpEndpoint::V4(10, 0, 0, 2, 2000);
  p.io_a = std::make_shared<pp::adp::MemoryDatagramIo>(p.hub, p.addr_a);
  p.io_b = std::make_shared<pp::adp::MemoryDatagramIo>(p.hub, p.addr_b);
  p.ep_a = std::make_unique<pp::adp::Endpoint>(p.io_a, p.clock);
  p.ep_b = std::make_unique<pp::adp::Endpoint>(p.io_b, p.clock);
  return p;
}

std::vector<uint8_t> SealPacket(const pp::adp::PeerKey& key, const pp::adp::WirePacket& pkt) {
  auto enc = pp::adp::WireCodec::Encode(pkt);
  EXPECT_TRUE(enc);
  auto sealed = pp::adp::HmacBinder(key).Seal(*enc);
  EXPECT_TRUE(sealed);
  return *sealed;
}

class AdpEdgeTest : public ::testing::Test {
protected:
  void SetUp() override { ASSERT_GE(sodium_init(), 0); }
};

TEST_F(AdpEdgeTest, ReplayRejectOnWireBestEffort) {
  auto p = MakePair();
  pp::adp::OpenParams op;
  op.key = Key();
  op.id = Aid();
  op.mint_id = false;
  op.peer = p.addr_b;
  auto ca = p.ep_a->Open(op);
  pp::adp::OpenParams opb = op;
  opb.peer = p.addr_a;
  auto cb = p.ep_b->Open(opb);
  ASSERT_TRUE(ca);
  ASSERT_TRUE(cb);

  size_t got = 0;
  (*cb)->OnMessage([&](const pp::adp::Message&) { ++got; });

  pp::adp::WirePacket pkt;
  pkt.type = pp::adp::PacketType::DataBestEffort;
  pkt.assoc = Aid();
  pkt.seq = 1;
  pkt.timestamp_ms = static_cast<uint32_t>(p.clock->NowMs() & 0xffffffffull);
  pkt.payload = {'x'};
  const auto sealed = SealPacket(Key(), pkt);

  (*cb)->HandleDatagram(p.addr_a, sealed, p.clock->NowMs());
  (*cb)->HandleDatagram(p.addr_a, sealed, p.clock->NowMs());
  EXPECT_EQ(got, 1u);
}

TEST_F(AdpEdgeTest, ReliableDupAckOnlyOnce) {
  auto p = MakePair();
  pp::adp::OpenParams op;
  op.key = Key();
  op.id = Aid();
  op.mint_id = false;
  op.peer = p.addr_b;
  auto ca = p.ep_a->Open(op);
  pp::adp::OpenParams opb = op;
  opb.peer = p.addr_a;
  auto cb = p.ep_b->Open(opb);
  ASSERT_TRUE(ca);
  ASSERT_TRUE(cb);

  size_t got = 0;
  (*cb)->OnMessage([&](const pp::adp::Message&) { ++got; });

  pp::adp::WirePacket pkt;
  pkt.type = pp::adp::PacketType::DataReliable;
  pkt.assoc = Aid();
  pkt.seq = 1;
  pkt.timestamp_ms = static_cast<uint32_t>(p.clock->NowMs() & 0xffffffffull);
  pkt.payload = {'r'};
  const auto sealed = SealPacket(Key(), pkt);

  (*cb)->HandleDatagram(p.addr_a, sealed, p.clock->NowMs());
  (*cb)->HandleDatagram(p.addr_a, sealed, p.clock->NowMs());
  EXPECT_EQ(got, 1u);
}

TEST_F(AdpEdgeTest, LooksAliveTimeout) {
  auto p = MakePair();
  pp::adp::OpenParams op;
  op.key = Key();
  op.id = Aid();
  op.mint_id = false;
  op.peer = p.addr_b;
  auto ca = p.ep_a->Open(op);
  pp::adp::OpenParams opb = op;
  opb.peer = p.addr_a;
  auto cb = p.ep_b->Open(opb);
  ASSERT_TRUE(ca);
  ASSERT_TRUE(cb);

  EXPECT_FALSE((*cb)->LooksAlive(p.clock->NowMs()));

  ASSERT_TRUE((*ca)->Send(pp::adp::QosClass::BestEffort,
                          std::span<const uint8_t>(reinterpret_cast<const uint8_t*>("z"), 1)));
  p.ep_b->Pump();
  EXPECT_TRUE((*cb)->LooksAlive(p.clock->NowMs()));

  p.clock->Advance(pp::adp::kAliveTimeoutMs + 1);
  EXPECT_FALSE((*cb)->LooksAlive(p.clock->NowMs()));
}

TEST_F(AdpEdgeTest, UpgradeBinderRejectsOldKey) {
  auto p = MakePair();
  pp::adp::OpenParams op;
  op.key = Key(1);
  op.id = Aid();
  op.mint_id = false;
  op.peer = p.addr_b;
  auto cb = p.ep_b->Open(op);
  ASSERT_TRUE(cb);

  size_t got = 0;
  (*cb)->OnMessage([&](const pp::adp::Message&) { ++got; });

  pp::adp::WirePacket old_pkt;
  old_pkt.type = pp::adp::PacketType::DataBestEffort;
  old_pkt.assoc = Aid();
  old_pkt.seq = 1;
  old_pkt.timestamp_ms = static_cast<uint32_t>(p.clock->NowMs() & 0xffffffffull);
  old_pkt.payload = {'a'};
  const auto old_sealed = SealPacket(Key(1), old_pkt);

  (*cb)->HandleDatagram(p.addr_a, old_sealed, p.clock->NowMs());
  ASSERT_EQ(got, 1u);

  (*cb)->UpgradeBinder(Key(2));
  (*cb)->HandleDatagram(p.addr_a, old_sealed, p.clock->NowMs());
  EXPECT_EQ(got, 1u);

  pp::adp::WirePacket new_pkt = old_pkt;
  new_pkt.seq = 2;
  new_pkt.payload = {'b'};
  const auto new_sealed = SealPacket(Key(2), new_pkt);
  (*cb)->HandleDatagram(p.addr_a, new_sealed, p.clock->NowMs());
  EXPECT_EQ(got, 2u);
}

TEST_F(AdpEdgeTest, MintAssocIdUnique) {
  auto p = MakePair();
  pp::adp::OpenParams op;
  op.key = Key();
  op.mint_id = true;
  op.peer = p.addr_b;
  auto c1 = p.ep_a->Open(op);
  ASSERT_TRUE(c1);
  EXPECT_NE((*c1)->Id().bytes, pp::adp::AssocId{}.bytes);

  p.clock->Advance(1);
  auto c2 = p.ep_a->Open(op);
  ASSERT_TRUE(c2);
  EXPECT_NE((*c1)->Id().bytes, (*c2)->Id().bytes);
}

TEST_F(AdpEdgeTest, PayloadTooLargeOnSend) {
  auto p = MakePair();
  pp::adp::OpenParams op;
  op.key = Key();
  op.id = Aid();
  op.mint_id = false;
  op.peer = p.addr_b;
  auto ca = p.ep_a->Open(op);
  ASSERT_TRUE(ca);
  std::vector<uint8_t> big(static_cast<size_t>(pp::adp::kMaxPayload) + 1, 0xab);
  const auto err = (*ca)->Send(pp::adp::QosClass::BestEffort, big);
  EXPECT_FALSE(err);
  EXPECT_NE(err.error().message.find("payload too large"), std::string::npos);
}

TEST_F(AdpEdgeTest, ReliableWindowFull) {
  auto p = MakePair();
  pp::adp::OpenParams op;
  op.key = Key();
  op.id = Aid();
  op.mint_id = false;
  op.peer = p.addr_b;
  op.reliable_window = 4;
  auto ca = p.ep_a->Open(op);
  ASSERT_TRUE(ca);

  for (int i = 0; i < 4; ++i) {
    const uint8_t b = static_cast<uint8_t>('0' + i);
    ASSERT_TRUE((*ca)->Send(pp::adp::QosClass::Reliable, std::span<const uint8_t>(&b, 1)));
  }
  EXPECT_EQ((*ca)->ReliableCreditsRemaining(), 0u);
  const uint8_t extra = 'x';
  const auto err = (*ca)->Send(pp::adp::QosClass::Reliable, std::span<const uint8_t>(&extra, 1));
  EXPECT_FALSE(err);
  EXPECT_NE(err.error().message.find("reliable window full"), std::string::npos);
}

TEST_F(AdpEdgeTest, AcceptDisabledDropsInbound) {
  auto p = MakePair();
  p.ep_b->SetAcceptKey(Key());
  p.ep_b->SetAcceptEnabled(false);

  pp::adp::OpenParams op;
  op.key = Key();
  op.id = Aid();
  op.mint_id = false;
  op.peer = p.addr_b;
  auto ca = p.ep_a->Open(op);
  ASSERT_TRUE(ca);
  ASSERT_TRUE((*ca)->Send(pp::adp::QosClass::BestEffort,
                          std::span<const uint8_t>(reinterpret_cast<const uint8_t*>("x"), 1)));
  p.ep_b->Pump();
  EXPECT_EQ(p.ep_b->Find(Aid()), nullptr);
}

TEST_F(AdpEdgeTest, AcceptWrongKeyDropsInbound) {
  auto p = MakePair();
  p.ep_b->SetAcceptKey(Key(2));
  p.ep_b->SetAcceptEnabled(true);

  pp::adp::OpenParams op;
  op.key = Key(1);
  op.id = Aid();
  op.mint_id = false;
  op.peer = p.addr_b;
  auto ca = p.ep_a->Open(op);
  ASSERT_TRUE(ca);
  ASSERT_TRUE((*ca)->Send(pp::adp::QosClass::BestEffort,
                          std::span<const uint8_t>(reinterpret_cast<const uint8_t*>("x"), 1)));
  p.ep_b->Pump();
  EXPECT_EQ(p.ep_b->Find(Aid()), nullptr);
}

TEST_F(AdpEdgeTest, AcceptHandlerFiresOnce) {
  auto p = MakePair();
  p.ep_b->SetAcceptKey(Key());
  p.ep_b->SetAcceptEnabled(true);
  int accepts = 0;
  p.ep_b->SetAcceptHandler([&](std::shared_ptr<pp::adp::Connection>) { ++accepts; });

  pp::adp::OpenParams op;
  op.key = Key();
  op.id = Aid();
  op.mint_id = false;
  op.peer = p.addr_b;
  auto ca = p.ep_a->Open(op);
  ASSERT_TRUE(ca);
  ASSERT_TRUE((*ca)->Send(pp::adp::QosClass::BestEffort,
                          std::span<const uint8_t>(reinterpret_cast<const uint8_t*>("x"), 1)));
  p.ep_b->Pump();
  EXPECT_EQ(accepts, 1);
  EXPECT_NE(p.ep_b->Find(Aid()), nullptr);
}

TEST_F(AdpEdgeTest, AcceptOrCreateReturnsExisting) {
  auto p = MakePair();
  const auto peer = p.addr_a;
  auto c1 = p.ep_b->AcceptOrCreate(Aid(), Key(), peer);
  auto c2 = p.ep_b->AcceptOrCreate(Aid(), Key(), peer);
  ASSERT_TRUE(c1);
  ASSERT_TRUE(c2);
  EXPECT_EQ(c1->get(), c2->get());
}

TEST_F(AdpEdgeTest, DuplicateOpenSameAssocFails) {
  auto p = MakePair();
  pp::adp::OpenParams op;
  op.key = Key();
  op.id = Aid();
  op.mint_id = false;
  op.peer = p.addr_b;
  auto c1 = p.ep_a->Open(op);
  ASSERT_TRUE(c1);
  const auto c2 = p.ep_a->Open(op);
  EXPECT_FALSE(c2);
  EXPECT_NE(c2.error().message.find("assoc already open"), std::string::npos);
}

} // namespace
