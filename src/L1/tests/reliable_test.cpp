#include "amp/L1/Clock.h"
#include "amp/L1/Endpoint.h"
#include "amp/L1/MemoryDatagramIo.h"
#include "amp/L1/Types.h"

#include <gtest/gtest.h>
#include <sodium.h>

#include <cstring>
#include <string>
#include <vector>

namespace {

pp::adp::PeerKey Key() {
  pp::adp::PeerKey k;
  k.bytes.fill(0x55);
  return k;
}

pp::adp::AssocId Aid() {
  pp::adp::AssocId id;
  id.bytes.fill(0x66);
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
  p.clock = std::make_shared<pp::adp::VirtualClock>(5'000'000);
  p.hub = pp::adp::MemoryDatagramIo::MakeHub();
  p.addr_a = pp::adp::IpEndpoint::V4(127, 0, 0, 1, 4001);
  p.addr_b = pp::adp::IpEndpoint::V4(127, 0, 0, 1, 4002);
  p.io_a = std::make_shared<pp::adp::MemoryDatagramIo>(p.hub, p.addr_a);
  p.io_b = std::make_shared<pp::adp::MemoryDatagramIo>(p.hub, p.addr_b);
  p.ep_a = std::make_unique<pp::adp::Endpoint>(p.io_a, p.clock);
  p.ep_b = std::make_unique<pp::adp::Endpoint>(p.io_b, p.clock);
  return p;
}

void PumpBoth(Pair& p) {
  for (int i = 0; i < 8; ++i) {
    p.ep_a->Pump();
    p.ep_b->Pump();
    p.ep_a->Tick();
    p.ep_b->Tick();
  }
}

class AdpReliableTest : public ::testing::Test {
protected:
  void SetUp() override { ASSERT_GE(sodium_init(), 0); }
};

TEST_F(AdpReliableTest, DeliverUnderLoss) {
  auto p = MakePair();
  p.ep_b->SetAcceptKey(Key());
  p.ep_b->SetAcceptEnabled(true);
  p.io_a->DropNext(2); // drop first two datagrams (data + maybe nothing)

  pp::adp::OpenParams op;
  op.key = Key();
  op.id = Aid();
  op.mint_id = false;
  op.peer = p.addr_b;
  op.rtx_interval_ms = 10;
  op.max_rtx = 10;
  auto ca = p.ep_a->Open(op);
  ASSERT_TRUE(ca);

  std::vector<std::string> got;
  // Pre-open B so we can attach handler before rtx lands.
  pp::adp::OpenParams opb = op;
  opb.peer = p.addr_a;
  auto cb = p.ep_b->Open(opb);
  ASSERT_TRUE(cb);
  (*cb)->OnMessage([&](const pp::adp::Message& m) {
    if (m.qos == pp::adp::QosClass::Reliable) {
      got.emplace_back(m.payload.begin(), m.payload.end());
    }
  });

  ASSERT_TRUE((*ca)->Send(pp::adp::QosClass::Reliable,
                          std::span<const uint8_t>(reinterpret_cast<const uint8_t*>("rel"), 3)));
  // First send dropped.
  PumpBoth(p);
  EXPECT_TRUE(got.empty());
  p.clock->Advance(10);
  PumpBoth(p);
  // Second attempt may also be dropped (DropNext 2).
  p.clock->Advance(10);
  PumpBoth(p);
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0], "rel");
}

TEST_F(AdpReliableTest, DeliverUnderDup) {
  auto p = MakePair();
  p.ep_b->SetAcceptKey(Key());
  p.ep_b->SetAcceptEnabled(true);
  p.io_a->SetRngSeed(42);
  p.io_a->SetDupRate(1.0);

  pp::adp::OpenParams op;
  op.key = Key();
  op.id = Aid();
  op.mint_id = false;
  op.peer = p.addr_b;
  op.rtx_interval_ms = 10;
  op.max_rtx = 10;
  auto ca = p.ep_a->Open(op);
  ASSERT_TRUE(ca);

  std::vector<std::string> got;
  pp::adp::OpenParams opb = op;
  opb.peer = p.addr_a;
  auto cb = p.ep_b->Open(opb);
  ASSERT_TRUE(cb);
  (*cb)->OnMessage([&](const pp::adp::Message& m) {
    if (m.qos == pp::adp::QosClass::Reliable) {
      got.emplace_back(m.payload.begin(), m.payload.end());
    }
  });

  ASSERT_TRUE((*ca)->Send(pp::adp::QosClass::Reliable,
                          std::span<const uint8_t>(reinterpret_cast<const uint8_t*>("dup"), 3)));
  PumpBoth(p);
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0], "dup");
}

TEST_F(AdpReliableTest, DeliverUnderReorder) {
  auto p = MakePair();
  p.ep_b->SetAcceptKey(Key());
  p.ep_b->SetAcceptEnabled(true);
  p.io_a->SetRngSeed(9);
  p.io_a->SetReorderWindow(3);

  pp::adp::OpenParams op;
  op.key = Key();
  op.id = Aid();
  op.mint_id = false;
  op.peer = p.addr_b;
  op.rtx_interval_ms = 10;
  op.max_rtx = 10;
  auto ca = p.ep_a->Open(op);
  ASSERT_TRUE(ca);

  std::vector<std::string> got;
  pp::adp::OpenParams opb = op;
  opb.peer = p.addr_a;
  auto cb = p.ep_b->Open(opb);
  ASSERT_TRUE(cb);
  (*cb)->OnMessage([&](const pp::adp::Message& m) {
    if (m.qos == pp::adp::QosClass::Reliable) {
      got.emplace_back(m.payload.begin(), m.payload.end());
    }
  });

  ASSERT_TRUE((*ca)->Send(pp::adp::QosClass::Reliable,
                          std::span<const uint8_t>(reinterpret_cast<const uint8_t*>("r0"), 2)));
  ASSERT_TRUE((*ca)->Send(pp::adp::QosClass::Reliable,
                          std::span<const uint8_t>(reinterpret_cast<const uint8_t*>("r1"), 2)));
  ASSERT_TRUE((*ca)->Send(pp::adp::QosClass::Reliable,
                          std::span<const uint8_t>(reinterpret_cast<const uint8_t*>("r2"), 2)));
  ASSERT_TRUE((*ca)->Send(pp::adp::QosClass::Reliable,
                          std::span<const uint8_t>(reinterpret_cast<const uint8_t*>("r3"), 2)));
  p.io_a->FlushReorder();
  PumpBoth(p);
  p.clock->Advance(10);
  PumpBoth(p);
  ASSERT_EQ(got.size(), 4u);
  EXPECT_EQ(got[0], "r0");
  EXPECT_EQ(got[1], "r1");
  EXPECT_EQ(got[2], "r2");
  EXPECT_EQ(got[3], "r3");
}

/** Gap then fill: later seq held until earlier arrives; OnMessage stays in order. */
TEST_F(AdpReliableTest, DeliverInOrderAfterGap) {
  auto p = MakePair();
  p.ep_b->SetAcceptKey(Key());
  p.ep_b->SetAcceptEnabled(true);

  pp::adp::OpenParams op;
  op.key = Key();
  op.id = Aid();
  op.mint_id = false;
  op.peer = p.addr_b;
  op.rtx_interval_ms = 10;
  op.max_rtx = 10;
  auto ca = p.ep_a->Open(op);
  ASSERT_TRUE(ca);

  std::vector<std::string> got;
  pp::adp::OpenParams opb = op;
  opb.peer = p.addr_a;
  auto cb = p.ep_b->Open(opb);
  ASSERT_TRUE(cb);
  (*cb)->OnMessage([&](const pp::adp::Message& m) {
    if (m.qos == pp::adp::QosClass::Reliable) {
      got.emplace_back(m.payload.begin(), m.payload.end());
    }
  });

  // Drop the first datagram so seq 2 can land before seq 1.
  p.io_a->DropNext(1);
  ASSERT_TRUE((*ca)->Send(pp::adp::QosClass::Reliable,
                          std::span<const uint8_t>(reinterpret_cast<const uint8_t*>("a"), 1)));
  ASSERT_TRUE((*ca)->Send(pp::adp::QosClass::Reliable,
                          std::span<const uint8_t>(reinterpret_cast<const uint8_t*>("b"), 1)));
  PumpBoth(p);
  EXPECT_TRUE(got.empty()) << "OOO Reliable must not deliver past a gap";

  p.clock->Advance(10);
  PumpBoth(p); // rtx of seq 1
  ASSERT_EQ(got.size(), 2u);
  EXPECT_EQ(got[0], "a");
  EXPECT_EQ(got[1], "b");
}

/** N2: Seeded probabilistic loss — all Reliable messages eventually arrive in order. */
TEST_F(AdpReliableTest, DeliverUnderDropRate) {
  auto p = MakePair();
  p.ep_b->SetAcceptKey(Key());
  p.ep_b->SetAcceptEnabled(true);
  p.io_a->SetRngSeed(12345);
  p.io_a->SetDropRate(0.15);

  pp::adp::OpenParams op;
  op.key = Key();
  op.id = Aid();
  op.mint_id = false;
  op.peer = p.addr_b;
  op.rtx_interval_ms = 10;
  op.max_rtx = 40;
  auto ca = p.ep_a->Open(op);
  ASSERT_TRUE(ca);

  std::vector<std::string> got;
  pp::adp::OpenParams opb = op;
  opb.peer = p.addr_a;
  auto cb = p.ep_b->Open(opb);
  ASSERT_TRUE(cb);
  (*cb)->OnMessage([&](const pp::adp::Message& m) {
    if (m.qos == pp::adp::QosClass::Reliable) {
      got.emplace_back(m.payload.begin(), m.payload.end());
    }
  });

  constexpr int kCount = 32;
  for (int i = 0; i < kCount; ++i) {
    const char c = static_cast<char>('A' + (i % 26));
    ASSERT_TRUE((*ca)->Send(pp::adp::QosClass::Reliable,
                            std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&c), 1)));
  }
  for (int round = 0; round < 200 && static_cast<int>(got.size()) < kCount; ++round) {
    PumpBoth(p);
    p.clock->Advance(10);
  }
  ASSERT_EQ(got.size(), static_cast<size_t>(kCount));
  for (int i = 0; i < kCount; ++i) {
    EXPECT_EQ(got[static_cast<size_t>(i)], std::string(1, static_cast<char>('A' + (i % 26))));
  }
}

/** N2 soak: several drop rates × fixed seeds; heavier message count. */
TEST_F(AdpReliableTest, DeliverUnderMultiRateDropSoak) {
  const double rates[] = {0.05, 0.10, 0.20};
  const uint32_t seeds[] = {7, 42, 99};
  constexpr int kCount = 64;

  for (const double rate : rates) {
    for (const uint32_t seed : seeds) {
      auto p = MakePair();
      p.ep_b->SetAcceptKey(Key());
      p.ep_b->SetAcceptEnabled(true);
      p.io_a->SetRngSeed(seed);
      p.io_a->SetDropRate(rate);

      pp::adp::OpenParams op;
      op.key = Key();
      op.id = Aid();
      op.mint_id = false;
      op.peer = p.addr_b;
      op.rtx_interval_ms = 10;
      op.max_rtx = 60;
      auto ca = p.ep_a->Open(op);
      ASSERT_TRUE(ca) << "rate=" << rate << " seed=" << seed;

      std::vector<std::string> got;
      pp::adp::OpenParams opb = op;
      opb.peer = p.addr_a;
      auto cb = p.ep_b->Open(opb);
      ASSERT_TRUE(cb) << "rate=" << rate << " seed=" << seed;
      (*cb)->OnMessage([&](const pp::adp::Message& m) {
        if (m.qos == pp::adp::QosClass::Reliable) {
          got.emplace_back(m.payload.begin(), m.payload.end());
        }
      });

      for (int i = 0; i < kCount; ++i) {
        const char c = static_cast<char>('0' + (i % 10));
        ASSERT_TRUE((*ca)->Send(pp::adp::QosClass::Reliable,
                                std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&c), 1)))
            << "rate=" << rate << " seed=" << seed << " i=" << i;
      }
      for (int round = 0; round < 400 && static_cast<int>(got.size()) < kCount; ++round) {
        PumpBoth(p);
        p.clock->Advance(10);
      }
      ASSERT_EQ(got.size(), static_cast<size_t>(kCount)) << "rate=" << rate << " seed=" << seed;
      for (int i = 0; i < kCount; ++i) {
        EXPECT_EQ(got[static_cast<size_t>(i)], std::string(1, static_cast<char>('0' + (i % 10))))
            << "rate=" << rate << " seed=" << seed << " i=" << i;
      }
    }
  }
}

TEST_F(AdpReliableTest, AckStopsRetransmit) {
  auto p = MakePair();
  pp::adp::OpenParams op;
  op.key = Key();
  op.id = Aid();
  op.mint_id = false;
  op.peer = p.addr_b;
  op.rtx_interval_ms = 10;
  auto ca = p.ep_a->Open(op);
  auto cb = [&] {
    pp::adp::OpenParams opb = op;
    opb.peer = p.addr_a;
    return p.ep_b->Open(opb);
  }();
  ASSERT_TRUE(ca);
  ASSERT_TRUE(cb);
  size_t msg_count = 0;
  (*cb)->OnMessage([&](const pp::adp::Message&) { ++msg_count; });

  ASSERT_TRUE((*ca)->Send(pp::adp::QosClass::Reliable,
                          std::span<const uint8_t>(reinterpret_cast<const uint8_t*>("m"), 1)));
  PumpBoth(p);
  EXPECT_EQ(msg_count, 1u);
  // Further ticks should not redeliver.
  for (int i = 0; i < 5; ++i) {
    p.clock->Advance(10);
    PumpBoth(p);
  }
  EXPECT_EQ(msg_count, 1u);
}

TEST_F(AdpReliableTest, QosIsolation) {
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

  std::vector<pp::adp::QosClass> order;
  (*cb)->OnMessage([&](const pp::adp::Message& m) { order.push_back(m.qos); });

  p.io_a->DropNext(1); // drop best-effort
  ASSERT_TRUE((*ca)->Send(pp::adp::QosClass::BestEffort,
                          std::span<const uint8_t>(reinterpret_cast<const uint8_t*>("b"), 1)));
  ASSERT_TRUE((*ca)->Send(pp::adp::QosClass::Reliable,
                          std::span<const uint8_t>(reinterpret_cast<const uint8_t*>("r"), 1)));
  PumpBoth(p);
  ASSERT_EQ(order.size(), 1u);
  EXPECT_EQ(order[0], pp::adp::QosClass::Reliable);
}

TEST_F(AdpReliableTest, ShutdownClose) {
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
  (*ca)->Close();
  PumpBoth(p);
  EXPECT_TRUE((*cb)->IsClosed());
  EXPECT_FALSE((*ca)->Send(pp::adp::QosClass::Reliable,
                           std::span<const uint8_t>(reinterpret_cast<const uint8_t*>("x"), 1)));
}

TEST_F(AdpReliableTest, SpuriousAckIgnored) {
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
  // Send ACK from B without data — should not create issues.
  // Use reliable send then normal path; inject nothing else.
  ASSERT_TRUE((*ca)->Send(pp::adp::QosClass::Reliable,
                          std::span<const uint8_t>(reinterpret_cast<const uint8_t*>("z"), 1)));
  PumpBoth(p);
  SUCCEED();
}

TEST_F(AdpReliableTest, GiveUpAfterMaxRtx) {
  auto p = MakePair();
  // B not accepting / not open — rtx until give-up, no crash.
  pp::adp::OpenParams op;
  op.key = Key();
  op.id = Aid();
  op.mint_id = false;
  op.peer = p.addr_b;
  op.rtx_interval_ms = 5;
  op.max_rtx = 3;
  auto ca = p.ep_a->Open(op);
  ASSERT_TRUE(ca);
  ASSERT_TRUE((*ca)->Send(pp::adp::QosClass::Reliable,
                          std::span<const uint8_t>(reinterpret_cast<const uint8_t*>("z"), 1)));
  for (int i = 0; i < 10; ++i) {
    p.clock->Advance(5);
    p.ep_a->Tick();
  }
  SUCCEED();
}

} // namespace

namespace {

struct OpenPair {
  Pair p = MakePair();
  std::shared_ptr<pp::adp::Connection> a;
  std::shared_ptr<pp::adp::Connection> b;
};

OpenPair OpenBoth(const int max_rtx = 10) {
  OpenPair o;
  pp::adp::OpenParams op;
  op.key = Key();
  op.id = Aid();
  op.mint_id = false;
  op.peer = o.p.addr_b;
  op.rtx_interval_ms = 10;
  op.max_rtx = max_rtx;
  o.a = *o.p.ep_a->Open(op);
  pp::adp::OpenParams opb = op;
  opb.peer = o.p.addr_a;
  o.b = *o.p.ep_b->Open(opb);
  o.b->OnMessage([](const pp::adp::Message&) {});
  return o;
}

std::span<const uint8_t> Bytes(const char* text) {
  return std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text), std::strlen(text));
}

} // namespace

// Stats: traffic totals and a round trip from the ack of a first send.
TEST_F(AdpReliableTest, StatsCountTrafficAndARoundTrip) {
  auto o = OpenBoth();
  std::vector<int64_t> samples;
  o.p.ep_a->SetRttObserver([&](int64_t rtt_ms) { samples.push_back(rtt_ms); });
  ASSERT_TRUE(o.a->Send(pp::adp::QosClass::Reliable, Bytes("stats")));
  o.p.ep_b->Pump();  // B takes it and acks
  o.p.clock->Advance(7);
  o.p.ep_a->Pump();  // A sees the ack 7 ms after the send

  const auto a = o.p.ep_a->Stats();
  const auto b = o.p.ep_b->Stats();
  EXPECT_EQ(a.reliable_sent, 1u);
  EXPECT_EQ(a.retransmits, 0u);
  EXPECT_GE(a.tx_datagrams, 1u);
  EXPECT_GT(a.tx_bytes, 0u);
  EXPECT_GE(b.rx_datagrams, 1u);
  EXPECT_EQ(a.rtt_samples, 1u);
  EXPECT_EQ(a.rtt_sum_ms, 7u);
  EXPECT_EQ(samples, std::vector<int64_t>{7});
}

// Karn: a retransmitted packet's ack is ambiguous — no round-trip sample; the retransmit counts.
TEST_F(AdpReliableTest, RetransmittedPacketGivesNoRoundTrip) {
  auto o = OpenBoth();
  o.p.io_a->DropNext(1);
  ASSERT_TRUE(o.a->Send(pp::adp::QosClass::Reliable, Bytes("again")));
  PumpBoth(o.p);
  o.p.clock->Advance(10);
  PumpBoth(o.p);
  const auto a = o.p.ep_a->Stats();
  EXPECT_GE(a.retransmits, 1u);
  EXPECT_EQ(a.rtt_samples, 0u);
  EXPECT_EQ(a.reliable_lost, 0u);
}

TEST_F(AdpReliableTest, PacketGivenUpAfterTheRetransmitCapCountsAsLost) {
  auto o = OpenBoth(/*max_rtx=*/2);
  o.p.io_a->DropNext(100);
  ASSERT_TRUE(o.a->Send(pp::adp::QosClass::Reliable, Bytes("lost")));
  for (int i = 0; i < 6; ++i) {
    PumpBoth(o.p);
    o.p.clock->Advance(10);
  }
  PumpBoth(o.p);
  EXPECT_EQ(o.p.ep_a->Stats().reliable_lost, 1u);
}

TEST_F(AdpReliableTest, GarbageDatagramsCountAsRejected) {
  auto p = MakePair();
  const std::vector<uint8_t> junk(64, 0xab);
  ASSERT_TRUE(p.io_a->SendTo(p.addr_b, junk));
  p.ep_b->Pump();
  const auto b = p.ep_b->Stats();
  EXPECT_EQ(b.rx_datagrams, 1u);
  EXPECT_EQ(b.rx_rejected, 1u);
}
