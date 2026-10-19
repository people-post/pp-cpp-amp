#include "support/mesh_test_harness.h"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

// One driver per runtime (STACK.md § Drive): a thread that claims the runtime is its only driver;
// unclaimed, any thread may drive, one at a time, as harnesses do.

namespace {

std::unique_ptr<pbr::test::AmpMeshHarness> harness() {
  auto created = pbr::test::AmpMeshHarness::Create();
  EXPECT_TRUE(created.isOk());
  return std::move(created.value());
}

} // namespace

TEST(DriverClaimTest, UnclaimedAnyThreadDrivesOneAtATime) {
  auto h = harness();
  auto& runtime = *h->runtime_a;
  int ran = 0;
  runtime.PostToIo([&]() { ++ran; });
  std::thread other([&]() { runtime.Drive(); });
  other.join();
  EXPECT_EQ(ran, 1) << "another thread drove";
  runtime.PostToIo([&]() { ++ran; });
  runtime.Drive();
  EXPECT_EQ(ran, 2) << "and then this one";
  EXPECT_EQ(runtime.RefusedDrives(), 0u);
}

TEST(DriverClaimTest, AClaimedRuntimeRefusesASecondDriver) {
  auto h = harness();
  auto& runtime = *h->runtime_a;
  runtime.ClaimDriver();
  std::atomic<int> ran{0};
  runtime.PostToIo([&]() { ++ran; });
  std::thread other([&]() { runtime.Drive(); runtime.Pump(); runtime.Tick(); });
  other.join();
  EXPECT_EQ(ran.load(), 0) << "the second driver did nothing";
  EXPECT_EQ(runtime.RefusedDrives(), 3u);
  runtime.Drive();
  EXPECT_EQ(ran.load(), 1) << "the claimed driver runs the work";

  runtime.ReleaseDriver();
  runtime.PostToIo([&]() { ++ran; });
  std::thread after([&]() { runtime.Drive(); });
  after.join();
  EXPECT_EQ(ran.load(), 2) << "released: any thread again";
}

TEST(DriverClaimTest, OnDriverThreadNamesWhereIoWorkRuns) {
  auto h = harness();
  auto& runtime = *h->runtime_a;
  bool inside = false;
  runtime.PostToIo([&]() { inside = runtime.OnDriverThread(); });
  runtime.Drive();
  EXPECT_TRUE(inside) << "unclaimed: the thread inside the drive";
  EXPECT_FALSE(runtime.OnDriverThread()) << "and not outside it";

  runtime.ClaimDriver();
  EXPECT_TRUE(runtime.OnDriverThread()) << "claimed: the driver thread, between drives too";
  bool elsewhere = true;
  std::thread other([&]() { elsewhere = runtime.OnDriverThread(); });
  other.join();
  EXPECT_FALSE(elsewhere);
}

TEST(DriverClaimTest, StopReleasesTheClaim) {
  auto h = harness();
  auto& runtime = *h->runtime_a;
  runtime.ClaimDriver();
  runtime.Stop();
  EXPECT_FALSE(runtime.OnDriverThread());
  runtime.Start();
  std::thread other([&]() { runtime.ClaimDriver(); runtime.Drive(); });
  other.join();
  EXPECT_EQ(runtime.RefusedDrives(), 0u) << "a restarted runtime is claimed afresh";
}
