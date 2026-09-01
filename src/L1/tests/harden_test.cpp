#include "amp/L1/Clock.h"
#include "amp/L1/Endpoint.h"
#include "amp/L1/HmacBinder.h"
#include "amp/L1/MemoryDatagramIo.h"
#include "amp/L1/OsUdpDatagramIo.h"
#include "amp/L1/Types.h"
#include "amp/L1/WireCodec.h"

#include <gtest/gtest.h>
#include <sodium.h>

#include <random>
#include <string>
#include <vector>

namespace {

pp::adp::PeerKey Key() {
  pp::adp::PeerKey k;
  k.bytes.fill(0x77);
  return k;
}

pp::adp::AssocId Aid() {
  pp::adp::AssocId id;
  id.bytes.fill(0x88);
  return id;
}

class AdpHardenTest : public ::testing::Test {
protected:
  void SetUp() override { ASSERT_GE(sodium_init(), 0); }
};

TEST_F(AdpHardenTest, PacketMutilatorNeverCrashes) {
  pp::adp::WirePacket pkt;
  pkt.type = pp::adp::PacketType::DataBestEffort;
  pkt.assoc = Aid();
  pkt.seq = 1;
  pkt.timestamp_ms = 42;
  pkt.payload = {9, 8, 7};
  auto enc = pp::adp::WireCodec::Encode(pkt);
  ASSERT_TRUE(enc);
  auto sealed = pp::adp::HmacBinder(Key()).Seal(*enc);
  ASSERT_TRUE(sealed);

  std::mt19937 rng(12345);
  for (int i = 0; i < 200; ++i) {
    auto mut = *sealed;
    const size_t nflip = 1 + (rng() % 8);
    for (size_t f = 0; f < nflip; ++f) {
      mut[rng() % mut.size()] ^= static_cast<uint8_t>(1 + (rng() % 255));
    }
    if (rng() % 2 == 0 && mut.size() > 4) {
      mut.resize(rng() % mut.size());
    }
    (void)pp::adp::WireCodec::Decode(mut);
    (void)pp::adp::HmacBinder(Key()).Verify(mut);
  }
}

TEST_F(AdpHardenTest, OsUdpLoopbackSmoke) {
  auto bound_a = pp::adp::OsUdpDatagramIo::Bind(pp::adp::IpEndpoint::V4(127, 0, 0, 1, 0));
  auto bound_b = pp::adp::OsUdpDatagramIo::Bind(pp::adp::IpEndpoint::V4(127, 0, 0, 1, 0));
  ASSERT_TRUE(bound_a);
  ASSERT_TRUE(bound_b);
  std::shared_ptr<pp::adp::DatagramIo> io_a(std::move(*bound_a));
  std::shared_ptr<pp::adp::DatagramIo> io_b(std::move(*bound_b));
  auto clock = std::make_shared<pp::adp::VirtualClock>(9'000'000);
  auto ep_a = std::make_unique<pp::adp::Endpoint>(io_a, clock);
  auto ep_b = std::make_unique<pp::adp::Endpoint>(io_b, clock);
  const auto addr_a = ep_a->Io().LocalEndpoint();
  const auto addr_b = ep_b->Io().LocalEndpoint();

  pp::adp::OpenParams op;
  op.key = Key();
  op.id = Aid();
  op.mint_id = false;
  op.peer = addr_b;
  auto ca = ep_a->Open(op);
  ASSERT_TRUE(ca);

  std::string got;
  pp::adp::OpenParams opb = op;
  opb.peer = addr_a;
  auto cb = ep_b->Open(opb);
  ASSERT_TRUE(cb);
  (*cb)->OnMessage([&](const pp::adp::Message& m) {
    got.assign(m.payload.begin(), m.payload.end());
  });

  ASSERT_TRUE((*ca)->Send(pp::adp::QosClass::Reliable,
                          std::span<const uint8_t>(reinterpret_cast<const uint8_t*>("udp"), 3)));
  for (int i = 0; i < 50; ++i) {
    ep_a->Pump();
    ep_b->Pump();
    ep_a->Tick();
    ep_b->Tick();
    if (!got.empty()) {
      break;
    }
    clock->Advance(5);
  }
  EXPECT_EQ(got, "udp");
}

TEST_F(AdpHardenTest, MultiConnectionStressMemory) {
  auto clock = std::make_shared<pp::adp::VirtualClock>(1);
  auto hub = pp::adp::MemoryDatagramIo::MakeHub();
  auto addr_a = pp::adp::IpEndpoint::V4(10, 1, 1, 1, 1);
  auto addr_b = pp::adp::IpEndpoint::V4(10, 1, 1, 2, 2);
  auto io_a = std::make_shared<pp::adp::MemoryDatagramIo>(hub, addr_a);
  auto io_b = std::make_shared<pp::adp::MemoryDatagramIo>(hub, addr_b);
  auto ep_a = std::make_unique<pp::adp::Endpoint>(io_a, clock);
  auto ep_b = std::make_unique<pp::adp::Endpoint>(io_b, clock);
  ep_b->SetAcceptKey(Key());
  ep_b->SetAcceptEnabled(true);

  constexpr int N = 32;
  std::vector<std::shared_ptr<pp::adp::Connection>> cons;
  size_t received = 0;
  for (int i = 0; i < N; ++i) {
    pp::adp::OpenParams op;
    op.key = Key();
    op.id = Aid();
    op.id.bytes[0] = static_cast<uint8_t>(i);
    op.mint_id = false;
    op.peer = addr_b;
    auto c = ep_a->Open(op);
    ASSERT_TRUE(c);
    cons.push_back(*c);
    pp::adp::OpenParams opb = op;
    opb.peer = addr_a;
    auto cb = ep_b->Open(opb);
    ASSERT_TRUE(cb);
    (*cb)->OnMessage([&](const pp::adp::Message&) { ++received; });
  }
  for (int i = 0; i < N; ++i) {
    const uint8_t b = static_cast<uint8_t>(i);
    ASSERT_TRUE(cons[static_cast<size_t>(i)]->Send(pp::adp::QosClass::BestEffort,
                                                   std::span<const uint8_t>(&b, 1)));
  }
  ep_b->Pump();
  EXPECT_EQ(received, static_cast<size_t>(N));
}

} // namespace
