#include "amp/L1/LossyDatagramIo.h"
#include "amp/link/AdpMultiaddr.h"
#include "support/mesh_test_harness.h"

#include <gtest/gtest.h>
#include <sodium.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <thread>

// An idle driver waits until the runtime's next deadline (DriveResult::next_due), not a fixed few ms, and a change
// another thread makes on the lane wakes it when it is due sooner (STACK.md § Drive; pp-browser #596: an idle phone's
// pump woke 200 times a second for nothing).

namespace pbr::test {
namespace {

using pp::amp::DriveResult;
using pp::amp::kDefaultHousekeepingInterval;
using pp::amp::MeshRuntime;
using std::chrono::milliseconds;

std::unique_ptr<AmpMeshHarness> Harness() {
  EXPECT_GE(sodium_init(), 0);
  auto created = AmpMeshHarness::Create();
  EXPECT_TRUE(created.isOk());
  return std::move(created.value());
}

/** Drive `rt` through the pass that follows activity (due within the housekeeping interval), clock and all. */
DriveResult Quiet(MeshRuntime& rt, pp::adp::VirtualClock& clock) {
  DriveResult r = rt.Drive();
  for (int i = 0; i < 50 && r.next_due <= kDefaultHousekeepingInterval; ++i) {
    clock.Advance(r.next_due.count());
    r = rt.Drive();
  }
  return r;
}

/** Drives both runtimes as two idle pumps would: each when it is due, and both at once after a datagram. */
struct DeadlineDriver {
  AmpMeshHarness& h;
  /** The longest wait (MeshPumpThread's cap). */
  milliseconds cap{1000};
  int turns = 0;
  milliseconds next{0};

  /** Turns until neither reads anything or is due now (bounded); `next`: the sooner of their deadlines. */
  void Settle() {
    for (int i = 0; i < 200; ++i) {
      const DriveResult a = h.runtime_a->Drive();
      const DriveResult b = h.runtime_b->Drive();
      turns += 2;
      next = std::min(a.next_due, b.next_due);
      if (next.count() > 0 && a.received == 0 && b.received == 0) {
        return;
      }
    }
  }

  /** Until `until_ms` (Amp clock): settle, then jump to the sooner deadline (capped). */
  void RunUntil(const int64_t until_ms) {
    while (h.clock->NowMs() < until_ms) {
      Settle();
      const int64_t step = std::clamp<int64_t>(std::min(next, cap).count(), 1, until_ms - h.clock->NowMs());
      h.clock->Advance(step);
    }
  }
};

bool Associate(AmpMeshHarness& h) {
  if (!h.runtime_a->RegisterEndpoint("b", h.ma_b)) {
    return false;
  }
  bool done = false;
  bool ok = false;
  h.runtime_a->EnsureAssociation("b", [&](pp::amp::PeerLinkManager::LinkRoe r) {
    done = true;
    ok = static_cast<bool>(r);
  });
  for (int i = 0; i < 500 && !done; ++i) {
    h.runtime_a->Drive();
    h.runtime_b->Drive();
  }
  return ok;
}

TEST(DriveDueTest, NothingDueSaysSoAndATimerSaysWhen) {
  auto h = Harness();
  auto& rt = *h->runtime_a;
  EXPECT_EQ(Quiet(rt, *h->clock).next_due, DriveResult::kNothingDue) << "no link, timer or tick: nothing to wake for";
  bool fired = false;
  rt.PostAfter(milliseconds(300), [&] { fired = true; });
  EXPECT_LE(rt.Drive().next_due, kDefaultHousekeepingInterval) << "a change: the walk looks again soon";
  h->clock->Advance(kDefaultHousekeepingInterval.count());
  EXPECT_EQ(rt.Drive().next_due, milliseconds(300) - kDefaultHousekeepingInterval) << "then the timer's deadline";
  h->clock->Advance(300 - kDefaultHousekeepingInterval.count());
  EXPECT_TRUE(rt.Drive().worked);
  EXPECT_TRUE(fired);
}

TEST(DriveDueTest, PostedWorkIsDueAtOnce) {
  auto h = Harness();
  auto& rt = *h->runtime_a;
  (void)Quiet(rt, *h->clock);
  rt.PostToIo([] {});
  EXPECT_EQ(rt.NextDueIn(), milliseconds(0));
}

// An io tick that says when it is due again runs then, not every housekeeping interval; one that does not keeps the
// old cadence (and keeps an idle driver at it).
TEST(DriveDueTest, AnIoTickRunsWhenItAsked) {
  auto h = Harness();
  auto& rt = *h->runtime_a;
  (void)Quiet(rt, *h->clock);
  int due_runs = 0;
  rt.AddIoDueTick([&] {
    ++due_runs;
    return milliseconds(2000);
  });
  EXPECT_EQ(Quiet(rt, *h->clock).next_due, milliseconds(2000)) << "idle: the tick's own deadline";
  const int runs = due_runs;
  EXPECT_GE(runs, 1);
  h->clock->Advance(1999);
  EXPECT_EQ(rt.Drive().next_due, milliseconds(1));
  EXPECT_EQ(due_runs, runs);
  h->clock->Advance(1);
  EXPECT_EQ(rt.Drive().next_due, milliseconds(2000));
  EXPECT_EQ(due_runs, runs + 1);

  rt.PostToIo([] {}); // the lane is active: the tick runs again within the interval
  EXPECT_EQ(rt.Drive().next_due, kDefaultHousekeepingInterval);
  h->clock->Advance(kDefaultHousekeepingInterval.count());
  (void)rt.Drive();
  EXPECT_EQ(due_runs, runs + 2);

  int legacy_runs = 0;
  rt.AddIoTick([&] { ++legacy_runs; });
  for (int i = 0; i < 3; ++i) {
    const auto next = rt.Drive().next_due;
    EXPECT_LE(next, kDefaultHousekeepingInterval) << "a tick without a deadline keeps the cadence";
    h->clock->Advance(next.count());
  }
  EXPECT_GE(legacy_runs, 3);
}

// An idle link is due at its own deadlines (keepalive, liveness), not every housekeeping interval: driven only when
// due, a warm link lives for minutes on a few hundred turns, as it lived on thousands.
TEST(DriveDueTest, AnIdleWarmLinkIsDrivenByItsDeadlinesAndStaysUp) {
  auto h = Harness();
  ASSERT_TRUE(Associate(*h));
  h->runtime_a->MarkWarm("b");
  DeadlineDriver driver{*h};
  driver.RunUntil(h->clock->NowMs() + 1'000);
  const int64_t start = h->clock->NowMs();
  driver.turns = 0;
  driver.RunUntil(start + 5 * 60'000);
  EXPECT_TRUE(h->runtime_a->IsConnected("b")) << "keepalives went out in time";
  EXPECT_NE(h->mgr_b().FindConnectedInboundLink(), nullptr);
  // 5 min at a 1 s cap: ~300 jumps (two runtimes) plus the keepalive exchanges; a 50 ms cadence was 12 000 turns.
  EXPECT_LT(driver.turns, 2500) << "an idle link wakes its driver only when something is due";
}

// A peer that goes silent is dropped when its liveness window ends, as when the driver polled: the deadline the walk
// reports is the window's end.
TEST(DriveDueTest, ASilentPeerIsDroppedWhenItsWindowEnds) {
  auto h = Harness();
  ASSERT_TRUE(Associate(*h));
  h->runtime_a->MarkHot("b");
  DeadlineDriver driver{*h};
  driver.RunUntil(h->clock->NowMs() + 1'000); // the hot cadence is announced (and the window widened)
  auto* link = h->mgr_a().FindLink("b");
  ASSERT_NE(link, nullptr);
  const int64_t window = link->ConnectionOrNull()->LivenessWindowMs();
  ASSERT_GT(window, pp::adp::kAliveTimeoutMs);
  int64_t dropped_at = 0;
  h->runtime_a->AddLinkEventListener([&](const pp::amp::LinkEvent& event) {
    if (event.kind == pp::amp::LinkEvent::Kind::Dropped && dropped_at == 0) {
      dropped_at = h->clock->NowMs();
    }
  });
  const int64_t last_rx = link->ConnectionOrNull()->LastAuthRxMs();
  // B stops without a word (a dead phone): it is never driven again. A runs only when it is due.
  auto& rt = *h->runtime_a;
  int turns = 0;
  DriveResult r = rt.Drive();
  while (h->clock->NowMs() < last_rx + window + 5'000 && dropped_at == 0 && turns < 100'000) {
    h->clock->Advance(std::clamp<int64_t>(r.next_due.count(), 1, 1000));
    r = rt.Drive();
    ++turns;
  }
  ASSERT_NE(dropped_at, 0) << "evicted";
  EXPECT_GT(dropped_at, last_rx + window);
  EXPECT_LE(dropped_at, last_rx + window + kDefaultHousekeepingInterval.count() + 1)
      << "no later than a driver polling at the housekeeping cadence";
  EXPECT_LT(turns, 200) << "and with few turns: " << window / 1000 << " s of waiting";
}

/** How A's driver runs: every 5 ms (the old pump), or only when due (the new one). */
enum class DriveMode { Poll5ms, WhenDue };

/** Drives A alone (B, if linked, is silent) until `done` or `span_ms`; returns the Amp time `done` became true. */
int64_t DriveAUntil(AmpMeshHarness& h, const DriveMode mode, const int64_t span_ms, const std::function<bool()>& done) {
  const int64_t until = h.clock->NowMs() + span_ms;
  DriveResult r = h.runtime_a->Drive();
  while (h.clock->NowMs() < until && !done()) {
    const int64_t step = mode == DriveMode::Poll5ms ? 5 : std::clamp<int64_t>(r.next_due.count(), 1, 1000);
    h.clock->Advance(step);
    r = h.runtime_a->Drive();
  }
  return done() ? h.clock->NowMs() : -1;
}

/** What happens at a deadline happens as soon driven only when due as polled every 5 ms (within the interval). */
void ExpectSameTime(const int64_t polled, const int64_t when_due, const char* what) {
  ASSERT_GT(polled, 0) << what << ": happened when polled";
  ASSERT_GT(when_due, 0) << what << ": happened when driven by deadlines";
  EXPECT_LE(when_due, polled + kDefaultHousekeepingInterval.count()) << what;
}

// A dial nobody answers times out, and a link silent after a network change is evicted at the grace's end, no later
// than when the driver polled every 5 ms.
TEST(DriveDueTest, DeadlinesComeAsSoonAsWhenPolled) {
  int64_t times[2][2] = {};
  for (const DriveMode mode : {DriveMode::Poll5ms, DriveMode::WhenDue}) {
    const int m = mode == DriveMode::Poll5ms ? 0 : 1;
    {
      auto h = Harness();
      const auto nobody = pp::amp::FormatAdpMultiaddr(pp::adp::IpEndpoint::V4(10, 0, 0, 99, 99), h->peer_id_b);
      ASSERT_TRUE(static_cast<bool>(nobody));
      ASSERT_TRUE(static_cast<bool>(h->runtime_a->RegisterEndpoint("x", *nobody)));
      const int64_t start = h->clock->NowMs();
      bool failed = false;
      h->runtime_a->EnsureAssociation("x", [&](pp::amp::PeerLinkManager::LinkRoe r) { failed = !r; });
      times[0][m] = DriveAUntil(*h, mode, 60'000, [&] { return failed; }) - start;
    }
    {
      auto h = Harness();
      ASSERT_TRUE(Associate(*h));
      DeadlineDriver driver{*h};
      driver.RunUntil(h->clock->NowMs() + 200);
      bool dropped = false;
      h->runtime_a->AddLinkEventListener([&](const pp::amp::LinkEvent& event) {
        dropped = dropped || event.kind == pp::amp::LinkEvent::Kind::Dropped;
      });
      const int64_t start = h->clock->NowMs();
      h->runtime_a->NotifyNetworkChanged();
      times[1][m] = DriveAUntil(*h, mode, 60'000, [&] { return dropped; }) - start;
    }
  }
  EXPECT_GE(times[0][0], 1000) << "the dial waited out its budget";
  EXPECT_GE(times[1][0], 1000) << "the link had its grace";
  ExpectSameTime(times[0][0], times[0][1], "an unanswered dial's timeout");
  ExpectSameTime(times[1][0], times[1][1], "a network change's eviction");
}

// A post, an earlier timer or any change while the driver waits long wakes it; a read or a later timer while it
// waits briefly does not.
TEST(DriveDueTest, ChangesFromAnotherThreadWakeTheDriverWhenDueSooner) {
  auto h = Harness();
  auto& rt = *h->runtime_a;
  std::atomic<int> wakes{0};
  rt.SetWakeHandler([&] { wakes.fetch_add(1); });
  rt.ClaimDriver();
  const auto other = [&](const std::function<void()>& fn) {
    std::thread t(fn);
    t.join();
  };

  ASSERT_EQ(Quiet(rt, *h->clock).next_due, DriveResult::kNothingDue);
  const int start = wakes.load();
  other([&] { (void)rt.IsConnected("nobody"); });
  EXPECT_EQ(wakes.load(), start + 1) << "waiting long: any change is looked at within the interval";
  other([&] { (void)rt.IsConnected("nobody"); });
  EXPECT_EQ(wakes.load(), start + 1) << "one wake per wait";

  rt.PostAfter(milliseconds(30), [] {});
  ASSERT_EQ(rt.Drive().next_due, milliseconds(30));
  const int before = wakes.load();
  other([&] { (void)rt.IsConnected("nobody"); });
  other([&] { rt.PostAfter(milliseconds(500), [] {}); });
  EXPECT_EQ(wakes.load(), before) << "a short wait: a read or a later timer waits for it";
  other([&] { rt.PostAfter(milliseconds(5), [] {}); });
  EXPECT_EQ(wakes.load(), before + 1) << "a sooner timer wakes it";
  ASSERT_EQ(rt.Drive().next_due, milliseconds(5));
  other([&] { rt.PostToIo([] {}); });
  EXPECT_EQ(wakes.load(), before + 2) << "posted work wakes it";
  EXPECT_TRUE(rt.Drive().worked);
}

// A reliable send from another thread (a sender holding the io lock: THREADING rule 4) makes a resend due: a driver
// waiting past it is woken, and the next turn's deadline is the resend's.
TEST(DriveDueTest, AReliableSendFromAnotherThreadWakesTheDriver) {
  auto h = Harness();
  ASSERT_TRUE(Associate(*h));
  DeadlineDriver driver{*h};
  driver.RunUntil(h->clock->NowMs() + 200);
  auto& rt = *h->runtime_a;
  std::atomic<int> wakes{0};
  rt.SetWakeHandler([&] { wakes.fetch_add(1); });
  rt.ClaimDriver();
  ASSERT_GT(Quiet(rt, *h->clock).next_due, kDefaultHousekeepingInterval);
  ASSERT_EQ(h->ep_a->NextResendDueMs(), pp::adp::Endpoint::kNoResendDue);
  const int before = wakes.load();
  std::thread sender([&] {
    rt.WithIoLock([&] {
      auto* link = rt.Links().FindLink("b");
      if (link && link->ConnectionOrNull()) {
        const std::vector<uint8_t> payload{1, 2, 3};
        (void)link->ConnectionOrNull()->Send(pp::adp::QosClass::Reliable, payload);
      }
    });
  });
  sender.join();
  EXPECT_EQ(wakes.load(), before + 1);
  ASSERT_NE(h->ep_a->NextResendDueMs(), pp::adp::Endpoint::kNoResendDue) << "its resend is due";
  EXPECT_LE(rt.Drive().next_due.count(), h->ep_a->NextResendDueMs() - h->clock->NowMs());
}

// A receive that fails for real (not "nothing ready") is counted and told to the endpoint's owner, not dropped.
TEST(DriveDueTest, AReceiveErrorIsCountedAndTold) {
  auto clock = std::make_shared<pp::adp::VirtualClock>(1'000'000);
  auto hub = pp::adp::MemoryDatagramIo::MakeHub();
  auto lossy = std::make_shared<pp::adp::LossyDatagramIo>(
      std::make_shared<pp::adp::MemoryDatagramIo>(hub, pp::adp::IpEndpoint::V4(10, 0, 0, 9, 9)));
  pp::adp::Endpoint endpoint(lossy, clock);
  std::vector<std::string> told;
  endpoint.SetReceiveErrorObserver([&](const pp::Error& error) { told.push_back(error.message); });
  EXPECT_EQ(endpoint.Pump(), 0u);
  EXPECT_EQ(endpoint.Stats().rx_errors, 0u) << "nothing ready is no error";
  lossy->FailReceives(pp::Error("recvfrom errno=57"));
  EXPECT_EQ(endpoint.Pump(), 0u);
  EXPECT_EQ(endpoint.Stats().rx_errors, 1u);
  ASSERT_EQ(told.size(), 1u);
  EXPECT_EQ(told[0], "recvfrom errno=57");
}

} // namespace
} // namespace pbr::test
