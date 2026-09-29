#include "amp/L1/Clock.h"
#include "amp/L1/Endpoint.h"
#include "amp/L1/HmacBinder.h"
#include "amp/L1/MemoryDatagramIo.h"
#include "amp/L1/OsUdpDatagramIo.h"
#include "amp/L1/Types.h"
#include "amp/L1/WireCodec.h"

#include <gtest/gtest.h>
#include <sodium.h>

#include <array>
#include <chrono>
#include <random>
#include <string>
#include <thread>
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
  op.rtx_interval_ms = 10;
  op.max_rtx = 30;
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
  // OS UDP needs wall time for kernel delivery / RTX; virtual Advance alone can finish in 0ms.
  for (int i = 0; i < 80 && got.empty(); ++i) {
    for (int j = 0; j < 8; ++j) {
      ep_a->Pump();
      ep_b->Pump();
      ep_a->Tick();
      ep_b->Tick();
    }
    clock->Advance(10);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
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

// Regression: Reliable payloads must not accumulate in the out-of-order hold when nobody will
// ever drain it (no OnMessage handler wired, e.g. a link the manager has already rejected).
TEST_F(AdpHardenTest, ReliableHoldStaysEmptyWithoutHandler) {
  auto clock = std::make_shared<pp::adp::VirtualClock>(1);
  auto hub = pp::adp::MemoryDatagramIo::MakeHub();
  auto addr_a = pp::adp::IpEndpoint::V4(10, 2, 2, 1, 1);
  auto addr_b = pp::adp::IpEndpoint::V4(10, 2, 2, 2, 2);
  auto io_a = std::make_shared<pp::adp::MemoryDatagramIo>(hub, addr_a);
  auto io_b = std::make_shared<pp::adp::MemoryDatagramIo>(hub, addr_b);
  auto ep_a = std::make_unique<pp::adp::Endpoint>(io_a, clock);
  auto ep_b = std::make_unique<pp::adp::Endpoint>(io_b, clock);
  ep_b->SetAcceptKey(Key());
  ep_b->SetAcceptEnabled(true);

  pp::adp::OpenParams op;
  op.key = Key();
  op.id = Aid();
  op.mint_id = false;
  op.peer = addr_b;
  auto ca = ep_a->Open(op);
  ASSERT_TRUE(ca);
  pp::adp::OpenParams opb = op;
  opb.peer = addr_a;
  auto cb = ep_b->Open(opb);
  ASSERT_TRUE(cb);
  // No OnMessage on B: DeliverReliableInOrder can never drain rx_rel_hold_.

  // Drop the first Reliable send so every later one arrives out of order on B.
  io_a->DropNext(1);
  for (int i = 0; i < 20; ++i) {
    const uint8_t b = static_cast<uint8_t>(i);
    ASSERT_TRUE((*ca)->Send(pp::adp::QosClass::Reliable, std::span<const uint8_t>(&b, 1)));
  }
  ep_b->Pump();

  EXPECT_EQ((*cb)->ReliableHoldSizeForTest(), 0u);
}

// Regression: an accepted (inbound) association retransmits an unacked Reliable send only
// kPreAuthMaxRtx times, not kDefaultMaxRtx — the reflection-amplification vector (a forged
// packet from a spoofed source pulls a Reliable reply that gets resent toward the victim).
TEST_F(AdpHardenTest, AcceptedConnectionCapsRetransmitsBeforeAuth) {
  auto clock = std::make_shared<pp::adp::VirtualClock>(1);
  auto hub = pp::adp::MemoryDatagramIo::MakeHub();
  auto addr_a = pp::adp::IpEndpoint::V4(10, 3, 3, 1, 1); // the (possibly spoofed) "victim" source
  auto addr_b = pp::adp::IpEndpoint::V4(10, 3, 3, 2, 2);
  auto io_a = std::make_shared<pp::adp::MemoryDatagramIo>(hub, addr_a);
  auto io_b = std::make_shared<pp::adp::MemoryDatagramIo>(hub, addr_b);
  auto ep_b = std::make_unique<pp::adp::Endpoint>(io_b, clock);
  ep_b->SetAcceptKey(Key());
  ep_b->SetAcceptEnabled(true);

  // Raw forged packet (no Connection on A's side — nothing will ever ack B's reply).
  pp::adp::WirePacket pkt;
  pkt.type = pp::adp::PacketType::Ack;
  pkt.assoc = Aid();
  pkt.seq = 0;
  pkt.timestamp_ms = 42;
  auto enc = pp::adp::WireCodec::Encode(pkt);
  ASSERT_TRUE(enc);
  auto sealed = pp::adp::HmacBinder(Key()).Seal(*enc);
  ASSERT_TRUE(sealed);
  ASSERT_TRUE(static_cast<bool>(io_a->SendTo(addr_b, *sealed)));
  ep_b->Pump();

  auto accepted = ep_b->Find(Aid());
  ASSERT_NE(accepted, nullptr);
  const uint8_t data = 0xAA;
  ASSERT_TRUE(static_cast<bool>(
      accepted->Send(pp::adp::QosClass::Reliable, std::span<const uint8_t>(&data, 1))));

  size_t reliable_sends = 0;
  for (int round = 0; round < 200; ++round) {
    clock->Advance(50);
    ep_b->Tick();
    for (;;) {
      auto got = io_a->RecvFrom();
      if (!got || !*got) {
        break;
      }
      auto decoded = pp::adp::WireCodec::Decode((*got)->second);
      if (decoded && decoded->type == pp::adp::PacketType::DataReliable) {
        ++reliable_sends;
      }
    }
  }
  // 1 initial send + kPreAuthMaxRtx retries.
  EXPECT_EQ(reliable_sends, static_cast<size_t>(1 + pp::adp::kPreAuthMaxRtx));
}

// Regression: minted AssocIds must not be predictable from wall-clock time — the old scheme
// packed the clock ms directly into the first 8 bytes.
TEST_F(AdpHardenTest, MintedAssocIdsAreNotClockDerived) {
  auto clock = std::make_shared<pp::adp::VirtualClock>(1'700'000'000'000); // realistic wall time
  auto hub = pp::adp::MemoryDatagramIo::MakeHub();
  auto addr_a = pp::adp::IpEndpoint::V4(10, 4, 4, 1, 1);
  auto io_a = std::make_shared<pp::adp::MemoryDatagramIo>(hub, addr_a);
  auto ep_a = std::make_unique<pp::adp::Endpoint>(io_a, clock);

  std::vector<pp::adp::AssocId> ids;
  for (int i = 0; i < 8; ++i) {
    pp::adp::OpenParams op;
    op.key = Key();
    op.mint_id = true;
    op.peer = pp::adp::IpEndpoint::V4(10, 4, 4, 2, 2);
    auto c = ep_a->Open(op);
    ASSERT_TRUE(static_cast<bool>(c));
    ids.push_back((*c)->Id());
  }
  // None of the clock's own bytes (little/big-endian, any 8-byte window) appear verbatim as a
  // prefix — the old scheme wrote them directly into bytes[0..7].
  const int64_t now = clock->NowMs();
  std::array<uint8_t, 8> clock_le{};
  for (size_t i = 0; i < 8; ++i) {
    clock_le[i] = static_cast<uint8_t>((now >> (i * 8)) & 0xff);
  }
  for (const auto& id : ids) {
    bool matches_clock_prefix = true;
    for (size_t i = 0; i < 8; ++i) {
      if (id.bytes[i] != clock_le[i]) {
        matches_clock_prefix = false;
        break;
      }
    }
    EXPECT_FALSE(matches_clock_prefix);
  }
  // All distinct (would also hold with the old scheme, but worth asserting).
  for (size_t i = 0; i < ids.size(); ++i) {
    for (size_t j = i + 1; j < ids.size(); ++j) {
      EXPECT_NE(ids[i], ids[j]);
    }
  }
}

// Regression: pre-auth (before UpgradeBinder), Close / Keepalive are forgeable with just the
// well-known pre-session key — they must be ignored, not acted on.
TEST_F(AdpHardenTest, PreAuthCloseAndKeepaliveAreIgnored) {
  auto clock = std::make_shared<pp::adp::VirtualClock>(1);
  auto hub = pp::adp::MemoryDatagramIo::MakeHub();
  auto addr_a = pp::adp::IpEndpoint::V4(10, 5, 5, 1, 1);
  auto addr_b = pp::adp::IpEndpoint::V4(10, 5, 5, 2, 2);
  auto io_a = std::make_shared<pp::adp::MemoryDatagramIo>(hub, addr_a);
  auto io_b = std::make_shared<pp::adp::MemoryDatagramIo>(hub, addr_b);
  auto ep_b = std::make_unique<pp::adp::Endpoint>(io_b, clock);
  ep_b->SetAcceptKey(Key());
  ep_b->SetAcceptEnabled(true);

  auto sendRaw = [&](pp::adp::PacketType type, uint32_t seq, std::span<const uint8_t> payload) {
    pp::adp::WirePacket pkt;
    pkt.type = type;
    pkt.assoc = Aid();
    pkt.seq = seq;
    pkt.timestamp_ms = static_cast<uint32_t>(clock->NowMs());
    pkt.payload.assign(payload.begin(), payload.end());
    auto enc = pp::adp::WireCodec::Encode(pkt);
    if (!enc) {
      return false;
    }
    auto sealed = pp::adp::HmacBinder(Key()).Seal(*enc);
    if (!sealed) {
      return false;
    }
    return static_cast<bool>(io_a->SendTo(addr_b, *sealed));
  };

  // Accept (Ack triggers AcceptOrCreate harmlessly, as in the retransmit-cap test above).
  ASSERT_TRUE(sendRaw(pp::adp::PacketType::Ack, 0, {}));
  ep_b->Pump();
  auto accepted = ep_b->Find(Aid());
  ASSERT_NE(accepted, nullptr);

  clock->Advance(1);
  ASSERT_TRUE(sendRaw(pp::adp::PacketType::Close, 0, {}));
  ep_b->Pump();
  EXPECT_FALSE(accepted->IsClosed()) << "forged pre-auth Close must be ignored";

  const uint8_t huge_interval[5] = {0xFF, 0xFF, 0xFF, 0xFF, 0x00};
  clock->Advance(1);
  ASSERT_TRUE(sendRaw(pp::adp::PacketType::Keepalive, 0, huge_interval));
  ep_b->Pump();
  EXPECT_EQ(accepted->PeerKeepaliveIntervalMs(), 0u) << "forged pre-auth Keepalive must be ignored";

  accepted->UpgradeBinder(Key());
  clock->Advance(1);
  ASSERT_TRUE(sendRaw(pp::adp::PacketType::Keepalive, 0, huge_interval));
  ep_b->Pump();
  // Capped, not the raw 0xFFFFFFFF the peer announced.
  EXPECT_EQ(accepted->PeerKeepaliveIntervalMs(), pp::adp::kMaxPeerKeepaliveIntervalMs);

  clock->Advance(1);
  ASSERT_TRUE(sendRaw(pp::adp::PacketType::Close, 0, {}));
  ep_b->Pump();
  EXPECT_TRUE(accepted->IsClosed()) << "post-auth Close is honored";
}

// Regression: Pump() drains at most its budget per call instead of looping until EAGAIN — a
// flood must not make one Pump() call (and any lock a caller holds around it) run unbounded.
TEST_F(AdpHardenTest, PumpRespectsItsBudget) {
  auto clock = std::make_shared<pp::adp::VirtualClock>(1);
  auto hub = pp::adp::MemoryDatagramIo::MakeHub();
  auto addr_a = pp::adp::IpEndpoint::V4(10, 6, 6, 1, 1);
  auto addr_b = pp::adp::IpEndpoint::V4(10, 6, 6, 2, 2);
  auto io_a = std::make_shared<pp::adp::MemoryDatagramIo>(hub, addr_a);
  auto io_b = std::make_shared<pp::adp::MemoryDatagramIo>(hub, addr_b);
  auto ep_a = std::make_unique<pp::adp::Endpoint>(io_a, clock);
  auto ep_b = std::make_unique<pp::adp::Endpoint>(io_b, clock);
  ep_b->SetAcceptKey(Key());
  ep_b->SetAcceptEnabled(true);

  pp::adp::OpenParams op;
  op.key = Key();
  op.id = Aid();
  op.mint_id = false;
  op.peer = addr_b;
  auto ca = ep_a->Open(op);
  ASSERT_TRUE(static_cast<bool>(ca));
  pp::adp::OpenParams opb = op;
  opb.peer = addr_a;
  auto cb = ep_b->Open(opb);
  ASSERT_TRUE(static_cast<bool>(cb));
  size_t received = 0;
  (*cb)->OnMessage([&](const pp::adp::Message&) { ++received; });

  constexpr size_t kSent = 20;
  for (size_t i = 0; i < kSent; ++i) {
    const uint8_t b = static_cast<uint8_t>(i);
    ASSERT_TRUE(static_cast<bool>(
        (*ca)->Send(pp::adp::QosClass::BestEffort, std::span<const uint8_t>(&b, 1))));
  }

  ep_b->Pump(/*budget=*/5);
  EXPECT_EQ(received, 5u);
  ep_b->Pump(/*budget=*/5);
  EXPECT_EQ(received, 10u);
  ep_b->Pump(); // default budget drains the rest.
  EXPECT_EQ(received, kSent);
}

TEST_F(AdpHardenTest, BindIpv6Wildcard) {
  auto bound = pp::adp::OsUdpDatagramIo::Bind(pp::adp::IpEndpoint::V6({}, 0));
  ASSERT_TRUE(static_cast<bool>(bound)) << (bound ? "" : bound.error().message);
  EXPECT_EQ((*bound)->LocalEndpoint().family, pp::adp::IpEndpoint::Family::V6);
  EXPECT_NE((*bound)->LocalEndpoint().port, 0u);
}

} // namespace
