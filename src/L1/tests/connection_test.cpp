#include "amp/L1/Clock.h"
#include "amp/L1/Endpoint.h"
#include "amp/L1/MemoryDatagramIo.h"
#include "amp/L1/Types.h"
#include "amp/L1/ReplayWindow.h"

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

class AdpConnTest : public ::testing::Test {
protected:
  void SetUp() override { ASSERT_GE(sodium_init(), 0); }
};

TEST_F(AdpConnTest, ReplayWindowBasics) {
  pp::adp::ReplayWindow w(8);
  EXPECT_TRUE(w.Accept(1));
  EXPECT_FALSE(w.Accept(1));
  EXPECT_TRUE(w.Accept(3));
  EXPECT_TRUE(w.Accept(2));
  EXPECT_EQ(w.LastContiguous(), 3u);
  EXPECT_FALSE(w.Accept(2));
}

TEST_F(AdpConnTest, BestEffortDeliver) {
  auto p = MakePair();
  p.ep_b->SetAcceptKey(Key());
  p.ep_b->SetAcceptEnabled(true);

  pp::adp::OpenParams op;
  op.key = Key();
  op.id = Aid();
  op.mint_id = false;
  op.peer = p.addr_b;
  auto ca = p.ep_a->Open(op);
  ASSERT_TRUE(ca);

  std::vector<std::string> got;
  // Accept path creates conn on B; attach handler after first pump via Find.
  ASSERT_TRUE((*ca)->Send(pp::adp::QosClass::BestEffort, std::span<const uint8_t>(
                                                               reinterpret_cast<const uint8_t*>("hi"),
                                                               2)));
  p.ep_b->Pump();
  auto cb = p.ep_b->Find(Aid());
  ASSERT_NE(cb, nullptr);
  cb->OnMessage([&](const pp::adp::Message& m) {
    got.emplace_back(m.payload.begin(), m.payload.end());
  });
  // Message already delivered before handler — send again.
  ASSERT_TRUE((*ca)->Send(pp::adp::QosClass::BestEffort,
                          std::span<const uint8_t>(reinterpret_cast<const uint8_t*>("yo"), 2)));
  p.ep_b->Pump();
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0], "yo");
}

TEST_F(AdpConnTest, BestEffortNoRtxOnDrop) {
  auto p = MakePair();
  p.ep_b->SetAcceptKey(Key());
  p.ep_b->SetAcceptEnabled(true);
  p.io_a->DropNext(1);

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
  p.clock->Advance(1000);
  p.ep_a->Tick();
  p.ep_b->Pump();
  EXPECT_EQ(p.ep_b->Find(Aid()), nullptr);
}

TEST_F(AdpConnTest, PathMigrate) {
  auto p = MakePair();

  pp::adp::OpenParams op;
  op.key = Key();
  op.id = Aid();
  op.mint_id = false;
  op.peer = p.addr_b;
  auto ca = p.ep_a->Open(op);
  ASSERT_TRUE(ca);

  pp::adp::OpenParams opb = op;
  opb.peer = p.addr_a;
  auto cb = p.ep_b->Open(opb);
  ASSERT_TRUE(cb);

  std::vector<pp::adp::IpEndpoint> paths;
  (*cb)->OnPathChange([&](const pp::adp::IpEndpoint&, const pp::adp::IpEndpoint& to) {
    paths.push_back(to);
  });

  // New client path (same assoc) — B should migrate peer endpoint.
  auto addr_c = pp::adp::IpEndpoint::V4(10, 0, 0, 1, 1001);
  auto io_c = std::make_shared<pp::adp::MemoryDatagramIo>(p.hub, addr_c);
  auto ep_c = std::make_unique<pp::adp::Endpoint>(io_c, p.clock);
  pp::adp::OpenParams opc = op;
  opc.peer = p.addr_b;
  auto cc = ep_c->Open(opc);
  ASSERT_TRUE(cc);
  ASSERT_TRUE((*cc)->Send(pp::adp::QosClass::BestEffort,
                          std::span<const uint8_t>(reinterpret_cast<const uint8_t*>("b"), 1)));
  p.ep_b->Pump();
  ASSERT_FALSE(paths.empty());
  EXPECT_EQ(paths.back(), addr_c);
  EXPECT_EQ((*cb)->PeerEndpoint(), addr_c);
}

TEST_F(AdpConnTest, NatReplyUsesObservedAddr) {
  auto p = MakePair();
  p.ep_b->SetAcceptKey(Key());
  p.ep_b->SetAcceptEnabled(true);

  pp::adp::OpenParams op;
  op.key = Key();
  op.id = Aid();
  op.mint_id = false;
  op.peer = p.addr_b;
  auto ca = p.ep_a->Open(op);
  ASSERT_TRUE(ca);

  std::string reply;
  (*ca)->OnMessage([&](const pp::adp::Message& m) {
    reply.assign(m.payload.begin(), m.payload.end());
  });

  ASSERT_TRUE((*ca)->Send(pp::adp::QosClass::BestEffort,
                          std::span<const uint8_t>(reinterpret_cast<const uint8_t*>("ping"), 4)));
  p.ep_b->Pump();
  auto cb = p.ep_b->Find(Aid());
  ASSERT_NE(cb, nullptr);
  EXPECT_EQ(cb->PeerEndpoint(), p.addr_a);
  ASSERT_TRUE(cb->Send(pp::adp::QosClass::BestEffort,
                       std::span<const uint8_t>(reinterpret_cast<const uint8_t*>("pong"), 4)));
  p.ep_a->Pump();
  EXPECT_EQ(reply, "pong");
}

TEST_F(AdpConnTest, TimestampSkewReject) {
  auto p = MakePair();
  p.ep_b->SetAcceptKey(Key());
  p.ep_b->SetAcceptEnabled(true);

  pp::adp::OpenParams op;
  op.key = Key();
  op.id = Aid();
  op.mint_id = false;
  op.peer = p.addr_b;
  op.skew_ms = 1000;
  auto ca = p.ep_a->Open(op);
  ASSERT_TRUE(ca);

  ASSERT_TRUE((*ca)->Send(pp::adp::QosClass::BestEffort,
                          std::span<const uint8_t>(reinterpret_cast<const uint8_t*>("z"), 1)));
  p.clock->Advance(120'000); // > kDefaultSkewMs on accept path
  p.ep_b->Pump();
  EXPECT_EQ(p.ep_b->Find(Aid()), nullptr);
}

TEST_F(AdpConnTest, SendAfterCloseFails) {
  auto p = MakePair();
  pp::adp::OpenParams op;
  op.key = Key();
  op.id = Aid();
  op.mint_id = false;
  op.peer = p.addr_b;
  auto ca = p.ep_a->Open(op);
  ASSERT_TRUE(ca);
  (*ca)->Close();
  EXPECT_FALSE((*ca)->Send(pp::adp::QosClass::BestEffort,
                           std::span<const uint8_t>(reinterpret_cast<const uint8_t*>("z"), 1)));
}

TEST_F(AdpConnTest, DemuxMultipleAssocs) {
  auto p = MakePair();
  p.ep_b->SetAcceptKey(Key());
  p.ep_b->SetAcceptEnabled(true);

  auto open = [&](uint8_t idv) {
    pp::adp::OpenParams op;
    op.key = Key();
    op.id = Aid(idv);
    op.mint_id = false;
    op.peer = p.addr_b;
    return p.ep_a->Open(op);
  };
  auto c1 = open(1);
  auto c2 = open(2);
  ASSERT_TRUE(c1);
  ASSERT_TRUE(c2);
  ASSERT_TRUE((*c1)->Send(pp::adp::QosClass::BestEffort,
                          std::span<const uint8_t>(reinterpret_cast<const uint8_t*>("1"), 1)));
  ASSERT_TRUE((*c2)->Send(pp::adp::QosClass::BestEffort,
                          std::span<const uint8_t>(reinterpret_cast<const uint8_t*>("2"), 1)));
  p.ep_b->Pump();
  EXPECT_NE(p.ep_b->Find(Aid(1)), nullptr);
  EXPECT_NE(p.ep_b->Find(Aid(2)), nullptr);
}

} // namespace
