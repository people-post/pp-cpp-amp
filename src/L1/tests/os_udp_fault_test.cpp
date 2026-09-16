#include "amp/L1/Clock.h"
#include "amp/L1/Endpoint.h"
#include "amp/L1/LossyDatagramIo.h"
#include "amp/L1/OsUdpDatagramIo.h"
#include "amp/L1/Types.h"

#include <gtest/gtest.h>
#include <sodium.h>

#include <optional>
#include <string>
#include <vector>

namespace {

pp::adp::PeerKey Key() {
  pp::adp::PeerKey k;
  k.bytes.fill(0xA5);
  return k;
}

pp::adp::AssocId Aid() {
  pp::adp::AssocId id;
  id.bytes.fill(0x5A);
  return id;
}

struct OsUdpLossyPair {
  std::shared_ptr<pp::adp::VirtualClock> clock;
  std::shared_ptr<pp::adp::LossyDatagramIo> lossy_a;
  std::shared_ptr<pp::adp::LossyDatagramIo> lossy_b;
  std::unique_ptr<pp::adp::Endpoint> ep_a;
  std::unique_ptr<pp::adp::Endpoint> ep_b;
  pp::adp::IpEndpoint addr_a;
  pp::adp::IpEndpoint addr_b;
};

std::optional<OsUdpLossyPair> TryMakeOsUdpLossyPair() {
  auto bound_a = pp::adp::OsUdpDatagramIo::Bind(pp::adp::IpEndpoint::V4(127, 0, 0, 1, 0));
  auto bound_b = pp::adp::OsUdpDatagramIo::Bind(pp::adp::IpEndpoint::V4(127, 0, 0, 1, 0));
  if (!bound_a || !bound_b) {
    return std::nullopt;
  }
  OsUdpLossyPair p;
  p.clock = std::make_shared<pp::adp::VirtualClock>(3'000'000);
  auto raw_a = std::shared_ptr<pp::adp::DatagramIo>(std::move(*bound_a));
  auto raw_b = std::shared_ptr<pp::adp::DatagramIo>(std::move(*bound_b));
  p.lossy_a = std::make_shared<pp::adp::LossyDatagramIo>(std::move(raw_a));
  p.lossy_b = std::make_shared<pp::adp::LossyDatagramIo>(std::move(raw_b));
  p.ep_a = std::make_unique<pp::adp::Endpoint>(p.lossy_a, p.clock);
  p.ep_b = std::make_unique<pp::adp::Endpoint>(p.lossy_b, p.clock);
  p.addr_a = p.ep_a->Io().LocalEndpoint();
  p.addr_b = p.ep_b->Io().LocalEndpoint();
  return p;
}

void PumpBoth(OsUdpLossyPair& p) {
  for (int i = 0; i < 8; ++i) {
    p.ep_a->Pump();
    p.ep_b->Pump();
    p.ep_a->Tick();
    p.ep_b->Tick();
  }
}

class AdpOsUdpFaultTest : public ::testing::Test {
protected:
  void SetUp() override { ASSERT_GE(sodium_init(), 0); }
};

TEST_F(AdpOsUdpFaultTest, ReliableSurvivesDropNextOnOsUdp) {
  auto maybe = TryMakeOsUdpLossyPair();
  if (!maybe) {
    GTEST_SKIP() << "OsUdp Bind failed — skip in restricted environments";
  }
  auto& p = *maybe;

  pp::adp::OpenParams op;
  op.key = Key();
  op.id = Aid();
  op.mint_id = false;
  op.peer = p.addr_b;
  op.rtx_interval_ms = 10;
  op.max_rtx = 30;
  auto ca = p.ep_a->Open(op);
  ASSERT_TRUE(ca);

  std::string got;
  pp::adp::OpenParams opb = op;
  opb.peer = p.addr_a;
  auto cb = p.ep_b->Open(opb);
  ASSERT_TRUE(cb);
  (*cb)->OnMessage([&](const pp::adp::Message& m) {
    if (m.qos == pp::adp::QosClass::Reliable) {
      got.assign(m.payload.begin(), m.payload.end());
    }
  });

  p.lossy_a->DropNext(2);
  ASSERT_TRUE((*ca)->Send(pp::adp::QosClass::Reliable,
                          std::span<const uint8_t>(reinterpret_cast<const uint8_t*>("os"), 2)));
  for (int i = 0; i < 80 && got.empty(); ++i) {
    PumpBoth(p);
    p.clock->Advance(10);
  }
  EXPECT_EQ(got, "os");
}

TEST_F(AdpOsUdpFaultTest, ReliableSurvivesDropRateOnOsUdp) {
  auto maybe = TryMakeOsUdpLossyPair();
  if (!maybe) {
    GTEST_SKIP() << "OsUdp Bind failed — skip in restricted environments";
  }
  auto& p = *maybe;
  p.lossy_a->SetRngSeed(77);
  p.lossy_a->SetDropRate(0.15);

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

  constexpr int kCount = 24;
  for (int i = 0; i < kCount; ++i) {
    const char c = static_cast<char>('a' + (i % 26));
    ASSERT_TRUE((*ca)->Send(pp::adp::QosClass::Reliable,
                            std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&c), 1)));
  }
  for (int round = 0; round < 300 && static_cast<int>(got.size()) < kCount; ++round) {
    PumpBoth(p);
    p.clock->Advance(10);
  }
  ASSERT_EQ(got.size(), static_cast<size_t>(kCount));
  for (int i = 0; i < kCount; ++i) {
    EXPECT_EQ(got[static_cast<size_t>(i)], std::string(1, static_cast<char>('a' + (i % 26))));
  }
}

TEST_F(AdpOsUdpFaultTest, ReliableSurvivesReorderOnOsUdp) {
  auto maybe = TryMakeOsUdpLossyPair();
  if (!maybe) {
    GTEST_SKIP() << "OsUdp Bind failed — skip in restricted environments";
  }
  auto& p = *maybe;
  p.lossy_a->SetRngSeed(13);
  p.lossy_a->SetReorderWindow(2);

  pp::adp::OpenParams op;
  op.key = Key();
  op.id = Aid();
  op.mint_id = false;
  op.peer = p.addr_b;
  op.rtx_interval_ms = 10;
  op.max_rtx = 30;
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

  for (const char* s : {"w0", "w1", "w2", "w3", "w4"}) {
    ASSERT_TRUE((*ca)->Send(pp::adp::QosClass::Reliable,
                            std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(s), 2)));
  }
  p.lossy_a->FlushReorder();
  p.lossy_a->SetReorderWindow(0);
  for (int round = 0; round < 100 && got.size() < 5; ++round) {
    PumpBoth(p);
    p.clock->Advance(10);
  }
  ASSERT_EQ(got.size(), 5u);
  EXPECT_EQ(got[0], "w0");
  EXPECT_EQ(got[1], "w1");
  EXPECT_EQ(got[2], "w2");
  EXPECT_EQ(got[3], "w3");
  EXPECT_EQ(got[4], "w4");
}

} // namespace
