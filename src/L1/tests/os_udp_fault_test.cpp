#include "amp/L1/Clock.h"
#include "amp/L1/Endpoint.h"
#include "amp/L1/LossyDatagramIo.h"
#include "amp/L1/MemoryDatagramIo.h"
#include "amp/L1/OsUdpDatagramIo.h"
#include "amp/L1/Types.h"

#include <gtest/gtest.h>
#include <sodium.h>

#if !defined(_WIN32)
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <chrono>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace pp::adp {

/** What the fault tests break behind an OsUdpDatagramIo's back. */
struct OsUdpDatagramIoTestPeer {
  static int SocketFd(const OsUdpDatagramIo& io) { return io.fd_; }
  static int WakeWriteFd(const OsUdpDatagramIo& io) { return io.wake_fds_[1]; }
  static uint64_t WakeHandleReplacements(const OsUdpDatagramIo& io) {
    return io.wake_handle_replacements_.load(std::memory_order_relaxed);
  }
  static uint64_t SocketPollPauses(const OsUdpDatagramIo& io) {
    return io.socket_poll_pauses_.load(std::memory_order_relaxed);
  }
};

} // namespace pp::adp

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
  ASSERT_TRUE(
      (*ca)->Send(pp::adp::QosClass::Reliable, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>("os"), 2)));
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
    ASSERT_TRUE(
        (*ca)->Send(pp::adp::QosClass::Reliable, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&c), 1)));
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
    ASSERT_TRUE(
        (*ca)->Send(pp::adp::QosClass::Reliable, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(s), 2)));
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

// pp-ledger load-resilience: a node asks for its socket buffers and can tell what it got, and on
// Linux how many datagrams the kernel dropped for want of receive buffer.
TEST(OsUdpBuffersTest, BuffersAreAskedForAndReported) {
  pp::adp::UdpSocketOptions small;
  small.receive_buffer_bytes = 4096;
  small.send_buffer_bytes = 4096;
  pp::adp::UdpSocketOptions large;
  large.receive_buffer_bytes = 4 * 1024 * 1024;
  auto a = pp::adp::OsUdpDatagramIo::Bind(pp::adp::IpEndpoint::V4(127, 0, 0, 1, 0), small);
  auto b = pp::adp::OsUdpDatagramIo::Bind(pp::adp::IpEndpoint::V4(127, 0, 0, 1, 0), large);
  ASSERT_TRUE(a && b);
  const auto sa = a.value()->Stats();
  const auto sb = b.value()->Stats();
  ASSERT_TRUE(sa && sb);
  EXPECT_GT(sa->receive_buffer_bytes, 0u);
  EXPECT_GE(sb->receive_buffer_bytes, sa->receive_buffer_bytes) << "more asked for, at least as much granted";
}

#if defined(__linux__)
TEST(OsUdpBuffersTest, TheKernelsDropsAreCounted) {
  pp::adp::UdpSocketOptions tiny;
  tiny.receive_buffer_bytes = 4096; // the kernel's minimum: a few datagrams
  auto receiver = pp::adp::OsUdpDatagramIo::Bind(pp::adp::IpEndpoint::V4(127, 0, 0, 1, 0), tiny);
  auto sender = pp::adp::OsUdpDatagramIo::Bind(pp::adp::IpEndpoint::V4(127, 0, 0, 1, 0));
  ASSERT_TRUE(receiver && sender);
  const std::vector<uint8_t> datagram(1200, 0x42);
  for (int i = 0; i < 2000; ++i) { // far more than the buffer holds, none read
    (void)sender.value()->SendTo(receiver.value()->LocalEndpoint(), datagram);
  }
  // The count rides on each datagram as of when it was queued: drain what was kept, then read one
  // sent after the drops.
  while (true) {
    auto drained = receiver.value()->RecvFrom();
    ASSERT_TRUE(drained);
    if (!drained.value().has_value()) {
      break;
    }
  }
  (void)sender.value()->SendTo(receiver.value()->LocalEndpoint(), datagram);
  auto got = receiver.value()->RecvFrom();
  ASSERT_TRUE(got && got.value().has_value());
  const auto stats = receiver.value()->Stats();
  ASSERT_TRUE(stats && stats->receive_drops);
  EXPECT_GT(*stats->receive_drops, 0u);
}
#endif

namespace {

std::optional<std::vector<uint8_t>> RecvWithin(pp::adp::DatagramIo& io, int tries) {
  for (int i = 0; i < tries; ++i) {
    auto got = io.RecvFrom();
    if (got && *got) {
      return (*got)->second;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return std::nullopt;
}

// iOS can leave an app's socket dead after a suspend: the app closes it and binds the same port.
TEST(AdpOsUdpReopenTest, AReopenedSocketKeepsItsPortAndWorks) {
  auto bound = pp::adp::OsUdpDatagramIo::Bind(pp::adp::IpEndpoint::V4(127, 0, 0, 1, 0));
  auto peer = pp::adp::OsUdpDatagramIo::Bind(pp::adp::IpEndpoint::V4(127, 0, 0, 1, 0));
  if (!bound || !peer) {
    GTEST_SKIP() << "OsUdp Bind failed — skip in restricted environments";
  }
  pp::adp::DatagramIo& io = **bound;
  const pp::adp::IpEndpoint before = io.LocalEndpoint();
  ASSERT_NE(before.port, 0);

  ASSERT_TRUE(static_cast<bool>(io.Reopen()));
  EXPECT_EQ(io.LocalEndpoint(), before);

  const std::vector<uint8_t> ping{1, 2, 3};
  ASSERT_TRUE(static_cast<bool>((*peer)->SendTo(before, ping)));
  EXPECT_EQ(RecvWithin(io, 200), ping) << "the new socket receives on the old port";
  ASSERT_TRUE(static_cast<bool>(io.SendTo((*peer)->LocalEndpoint(), ping)));
  EXPECT_EQ(RecvWithin(**peer, 200), ping) << "and sends from it";
}

// A batch read hands every ready datagram over in order, each from its sender with its own bytes, and stops at the
// budget: the rest wait for the next call. More than one recvmmsg's worth (32) here.
TEST(OsUdpBatchTest, ABatchReadHandsOverEveryDatagramInOrderUpToTheBudget) {
  auto receiver = pp::adp::OsUdpDatagramIo::Bind(pp::adp::IpEndpoint::V4(127, 0, 0, 1, 0));
  auto sender = pp::adp::OsUdpDatagramIo::Bind(pp::adp::IpEndpoint::V4(127, 0, 0, 1, 0));
  if (!receiver || !sender) {
    GTEST_SKIP() << "OsUdp Bind failed — skip in restricted environments";
  }
  constexpr int kSent = 100;
  for (int i = 0; i < kSent; ++i) {
    std::vector<uint8_t> datagram(static_cast<size_t>(100 + i), static_cast<uint8_t>(i));
    ASSERT_TRUE(static_cast<bool>((*sender)->SendTo((*receiver)->LocalEndpoint(), datagram)));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(20)); // loopback delivers at once; a margin

  std::vector<std::vector<uint8_t>> got;
  bool from_sender = true;
  const auto visit = [&](const pp::adp::IpEndpoint& from, const std::span<const uint8_t> datagram) {
    from_sender = from_sender && from == (*sender)->LocalEndpoint();
    got.emplace_back(datagram.begin(), datagram.end());
  };
  auto first = (*receiver)->RecvBatch(70, visit);
  ASSERT_TRUE(static_cast<bool>(first));
  EXPECT_EQ(*first, 70u) << "the budget, though more were ready";
  auto rest = (*receiver)->RecvBatch(1000, visit);
  ASSERT_TRUE(static_cast<bool>(rest));
  EXPECT_EQ(*rest, 30u);
  auto none = (*receiver)->RecvBatch(1000, visit);
  ASSERT_TRUE(static_cast<bool>(none));
  EXPECT_EQ(*none, 0u) << "drained: nothing, and no error";

  ASSERT_EQ(got.size(), static_cast<size_t>(kSent));
  EXPECT_TRUE(from_sender);
  for (int i = 0; i < kSent; ++i) {
    EXPECT_EQ(got[static_cast<size_t>(i)], std::vector<uint8_t>(static_cast<size_t>(100 + i), static_cast<uint8_t>(i)))
        << "datagram " << i;
  }
}

#if defined(__linux__)
// The kernel's drop count still rides on what a batch read takes (SO_RXQ_OVFL on each message).
TEST(OsUdpBatchTest, ABatchReadCountsTheKernelsDrops) {
  pp::adp::UdpSocketOptions tiny;
  tiny.receive_buffer_bytes = 4096;
  auto receiver = pp::adp::OsUdpDatagramIo::Bind(pp::adp::IpEndpoint::V4(127, 0, 0, 1, 0), tiny);
  auto sender = pp::adp::OsUdpDatagramIo::Bind(pp::adp::IpEndpoint::V4(127, 0, 0, 1, 0));
  ASSERT_TRUE(receiver && sender);
  const std::vector<uint8_t> datagram(1200, 0x42);
  for (int i = 0; i < 2000; ++i) {
    (void)(*sender)->SendTo((*receiver)->LocalEndpoint(), datagram);
  }
  const auto ignore = [](const pp::adp::IpEndpoint&, std::span<const uint8_t>) {};
  while (true) {
    auto n = (*receiver)->RecvBatch(1000, ignore);
    ASSERT_TRUE(static_cast<bool>(n));
    if (*n == 0) {
      break;
    }
  }
  (void)(*sender)->SendTo((*receiver)->LocalEndpoint(), datagram);
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  auto one = (*receiver)->RecvBatch(1000, ignore);
  ASSERT_TRUE(one && *one == 1u);
  const auto stats = (*receiver)->Stats();
  ASSERT_TRUE(stats && stats->receive_drops);
  EXPECT_GT(*stats->receive_drops, 0u);
}
#endif

// An idle reader waits on its socket: it wakes as a datagram arrives, not at the end of the wait, and a wake from
// another thread (the driver's Stop, posted work) ends the wait too. An idle wait with neither lasts its timeout.
TEST(OsUdpWaitTest, AWaitEndsWhenADatagramArrivesOrOnAWake) {
  auto receiver = pp::adp::OsUdpDatagramIo::Bind(pp::adp::IpEndpoint::V4(127, 0, 0, 1, 0));
  auto sender = pp::adp::OsUdpDatagramIo::Bind(pp::adp::IpEndpoint::V4(127, 0, 0, 1, 0));
  if (!receiver || !sender) {
    GTEST_SKIP() << "OsUdp Bind failed — skip in restricted environments";
  }
  using Clock = std::chrono::steady_clock;
  auto& io = **receiver;

  auto began = Clock::now();
  EXPECT_FALSE(io.WaitReadable(std::chrono::milliseconds(60)));
  EXPECT_GE(Clock::now() - began, std::chrono::milliseconds(50)) << "idle: the whole timeout";

  std::thread later([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    (void)(*sender)->SendTo(io.LocalEndpoint(), std::vector<uint8_t>{1, 2, 3});
  });
  began = Clock::now();
  EXPECT_TRUE(io.WaitReadable(std::chrono::seconds(5))) << "readable";
  EXPECT_LT(Clock::now() - began, std::chrono::seconds(2)) << "woken by the datagram, not the timeout";
  later.join();
  const auto ignore = [](const pp::adp::IpEndpoint&, std::span<const uint8_t>) {};
  ASSERT_TRUE(io.RecvBatch(10, ignore) && true);

  std::thread waker([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    io.WakeWaiter();
  });
  began = Clock::now();
  EXPECT_FALSE(io.WaitReadable(std::chrono::seconds(5))) << "woken, nothing to read";
  EXPECT_LT(Clock::now() - began, std::chrono::seconds(2)) << "the wake ended it";
  waker.join();

  io.WakeWaiter(); // before the wait: the next one returns at once
  began = Clock::now();
  (void)io.WaitReadable(std::chrono::seconds(5));
  EXPECT_LT(Clock::now() - began, std::chrono::seconds(1));
}

// A run of datagrams to one peer goes in one SendSegments (UDP GSO on Linux): every one arrives whole, in order, the
// shorter last one too — whether the kernel segmented them or the IO sent them one by one.
TEST(OsUdpBatchTest, SegmentsArriveAsTheDatagramsTheyWere) {
  auto receiver = pp::adp::OsUdpDatagramIo::Bind(pp::adp::IpEndpoint::V4(127, 0, 0, 1, 0));
  auto sender = pp::adp::OsUdpDatagramIo::Bind(pp::adp::IpEndpoint::V4(127, 0, 0, 1, 0));
  if (!receiver || !sender) {
    GTEST_SKIP() << "OsUdp Bind failed — skip in restricted environments";
  }
  constexpr size_t kSegment = 1200;
  std::vector<std::vector<uint8_t>> datagrams;
  for (int i = 0; i < 10; ++i) {
    datagrams.emplace_back(kSegment, static_cast<uint8_t>(i));
  }
  datagrams.emplace_back(345, 0x77); // the shorter last
  std::vector<std::span<const uint8_t>> parts(datagrams.begin(), datagrams.end());
  ASSERT_TRUE(static_cast<bool>((*sender)->SendSegments((*receiver)->LocalEndpoint(), parts, kSegment)));
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  std::vector<std::vector<uint8_t>> got;
  auto n = (*receiver)->RecvBatch(100, [&](const pp::adp::IpEndpoint&, const std::span<const uint8_t> datagram) {
    got.emplace_back(datagram.begin(), datagram.end());
  });
  ASSERT_TRUE(static_cast<bool>(n));
  EXPECT_EQ(got, datagrams);
}

TEST(AdpOsUdpReopenTest, InMemoryIoHasNoSocketToReopen) {
  auto hub = pp::adp::MemoryDatagramIo::MakeHub();
  pp::adp::MemoryDatagramIo memory(hub, pp::adp::IpEndpoint::V4(10, 0, 0, 1, 1));
  EXPECT_FALSE(static_cast<bool>(memory.Reopen()));
}

#if !defined(_WIN32)
// The states that made an idle reader's wait return at once, forever, while its read found nothing — a driver
// waiting there spun at 100 % of a core (pp-browser #596). Each test reads as a pump does (wait, then read) for a
// while and counts the rounds: a waiting reader does a handful, a spinning one thousands.

using Peer = pp::adp::OsUdpDatagramIoTestPeer;

struct ReadRounds {
  int rounds = 0;
  int errors = 0;
  size_t datagrams = 0;
};

/** Wait (`wait` each) and read, as a pump does, for `span`. */
ReadRounds ReadFor(pp::adp::OsUdpDatagramIo& io, const std::chrono::milliseconds span,
                   const std::chrono::milliseconds wait = std::chrono::milliseconds(50)) {
  ReadRounds out;
  const auto until = std::chrono::steady_clock::now() + span;
  const auto count = [&out](const pp::adp::IpEndpoint&, std::span<const uint8_t>) { ++out.datagrams; };
  while (std::chrono::steady_clock::now() < until && out.rounds < 100'000) {
    (void)io.WaitReadable(wait);
    if (!io.RecvBatch(64, count)) {
      ++out.errors;
    }
    ++out.rounds;
  }
  return out;
}

/** The reopened (or healthy) socket still hears a datagram. */
void ExpectHears(pp::adp::OsUdpDatagramIo& io) {
  auto sender = pp::adp::OsUdpDatagramIo::Bind(pp::adp::IpEndpoint::V4(127, 0, 0, 1, 0));
  ASSERT_TRUE(static_cast<bool>(sender));
  ASSERT_TRUE(static_cast<bool>((*sender)->SendTo(io.LocalEndpoint(), std::vector<uint8_t>{7, 7})));
  const auto got = ReadFor(io, std::chrono::milliseconds(300));
  EXPECT_GE(got.datagrams, 1u);
}

// Someone closed the wake pipe's write end (it hangs up for good): the handle is replaced, the waits wait again, and
// a wake still ends one.
TEST(OsUdpWaitFaultTest, AWakePipeThatHungUpIsReplaced) {
  auto bound = pp::adp::OsUdpDatagramIo::Bind(pp::adp::IpEndpoint::V4(127, 0, 0, 1, 0));
  if (!bound) {
    GTEST_SKIP() << "OsUdp Bind failed — skip in restricted environments";
  }
  auto& io = **bound;
  ASSERT_GE(Peer::WakeWriteFd(io), 0);
  ::close(Peer::WakeWriteFd(io));
  const auto rounds = ReadFor(io, std::chrono::milliseconds(300));
  EXPECT_LE(rounds.rounds, 12) << "each wait waited (6 of 50 ms in 300 ms), not a spin";
  EXPECT_GE(Peer::WakeHandleReplacements(io), 1u);

  std::thread waker([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    io.WakeWaiter();
  });
  const auto began = std::chrono::steady_clock::now();
  EXPECT_FALSE(io.WaitReadable(std::chrono::seconds(5)));
  EXPECT_LT(std::chrono::steady_clock::now() - began, std::chrono::seconds(2)) << "the new handle wakes";
  waker.join();
}

// A socket shut down under the reader (as iOS defuncts a suspended app's socket: readable at end of file, reads
// fail, for good) is reported, not polled again, and works after a reopen.
TEST(OsUdpWaitFaultTest, AHungUpSocketIsReportedNotPolledForEverAndReopens) {
  auto bound = pp::adp::OsUdpDatagramIo::Bind(pp::adp::IpEndpoint::V4(127, 0, 0, 1, 0));
  if (!bound) {
    GTEST_SKIP() << "OsUdp Bind failed — skip in restricted environments";
  }
  auto& io = **bound;
  const int fd = Peer::SocketFd(io);
  (void)::shutdown(fd, SHUT_RDWR); // an unconnected socket says ENOTCONN, and Linux shuts it down all the same
  pollfd probe{fd, POLLIN, 0};
  if (::poll(&probe, 1, 0) <= 0) {
    GTEST_SKIP() << "this OS does not shut down an unconnected UDP socket";
  }
  const auto rounds = ReadFor(io, std::chrono::milliseconds(300));
  EXPECT_LE(rounds.rounds, 12) << "each wait waited, not a spin";
  EXPECT_GE(rounds.errors, 1) << "the reader is told";
  ASSERT_TRUE(static_cast<bool>(io.Reopen()));
  ExpectHears(io);
}

// A socket closed under the reader (the poll says invalid; reads fail EBADF): reported, not polled for ever, and a
// reopen does not close the number again (it may be another file's by then).
TEST(OsUdpWaitFaultTest, ASocketClosedUnderTheReaderIsReportedAndReopens) {
  auto bound = pp::adp::OsUdpDatagramIo::Bind(pp::adp::IpEndpoint::V4(127, 0, 0, 1, 0));
  if (!bound) {
    GTEST_SKIP() << "OsUdp Bind failed — skip in restricted environments";
  }
  auto& io = **bound;
  ::close(Peer::SocketFd(io));
  const auto rounds = ReadFor(io, std::chrono::milliseconds(300));
  EXPECT_LE(rounds.rounds, 12) << "each wait waited, not a spin";
  EXPECT_GE(rounds.errors, 1) << "the reader is told";
  ASSERT_TRUE(static_cast<bool>(io.Reopen()));
  ExpectHears(io);
}

#if defined(__linux__)
// An error the read does not clear: with IP_RECVERR an ICMP error waits on the socket's error queue, the poll says
// POLLERR as long as it does, and a plain read finds nothing. The socket is left out of the poll for a while (it is
// still read), told once, and datagrams still get through.
TEST(OsUdpWaitFaultTest, AnErrorTheReadDoesNotClearDoesNotSpin) {
  auto bound = pp::adp::OsUdpDatagramIo::Bind(pp::adp::IpEndpoint::V4(127, 0, 0, 1, 0));
  if (!bound) {
    GTEST_SKIP() << "OsUdp Bind failed — skip in restricted environments";
  }
  auto& io = **bound;
  const int on = 1;
  ASSERT_EQ(::setsockopt(Peer::SocketFd(io), SOL_IP, IP_RECVERR, &on, sizeof(on)), 0);
  pp::adp::IpEndpoint closed_port;
  {
    auto gone = pp::adp::OsUdpDatagramIo::Bind(pp::adp::IpEndpoint::V4(127, 0, 0, 1, 0));
    ASSERT_TRUE(static_cast<bool>(gone));
    closed_port = (*gone)->LocalEndpoint();
  }
  ASSERT_TRUE(static_cast<bool>(io.SendTo(closed_port, std::vector<uint8_t>{1}))); // ICMP port unreachable back
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  pollfd probe{Peer::SocketFd(io), POLLIN, 0};
  ASSERT_EQ(::poll(&probe, 1, 0), 1);
  ASSERT_NE(probe.revents & POLLERR, 0) << "the error waits on the queue";
  const auto rounds = ReadFor(io, std::chrono::milliseconds(500));
  EXPECT_LE(rounds.rounds, 30) << "not a spin";
  EXPECT_GE(Peer::SocketPollPauses(io), 1u);
  EXPECT_GE(rounds.errors, 1) << "told: the socket polls readable and reads nothing";
  ExpectHears(io);
}
#endif
#endif

} // namespace

#if defined(__linux__)
namespace pp::adp {
namespace {

// pp-browser #597: a kernel before UDP GSO (4.18) took a run's UDP_SEGMENT without a word and sent it as
// one datagram nobody could read, so no handshake ever completed. The socket now asks the kernel at open;
// a socket that does not name the option (a TCP one answers as such a kernel does) gets one by one.
TEST(OsUdpGsoTest, SegmentingIsAskedOfTheKernelNotAssumed) {
  const int udp = ::socket(AF_INET, SOCK_DGRAM, 0);
  const int tcp = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(udp, 0);
  ASSERT_GE(tcp, 0);
  EXPECT_TRUE(UdpSegmentSupported(udp)) << "this kernel (4.18+) segments UDP";
  EXPECT_FALSE(UdpSegmentSupported(tcp)) << "an option the socket does not name means no GSO";
  EXPECT_FALSE(UdpSegmentSupported(-1));
  ::close(udp);
  ::close(tcp);
}

} // namespace
} // namespace pp::adp
#endif
