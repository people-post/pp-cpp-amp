#include "support/mesh_test_harness.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

// MeshRuntime::IoExecutor (STACK.md § Io executor): the io lane as a pp::SerialExecutor, for code that
// runs on the lane without driving it.

using namespace std::chrono_literals;

namespace {

std::unique_ptr<pbr::test::AmpMeshHarness> harness() {
  auto created = pbr::test::AmpMeshHarness::Create();
  EXPECT_TRUE(created.isOk());
  return std::move(created.value());
}

} // namespace

TEST(IoExecutorTest, PostRunsOnTheLane) {
  auto h = harness();
  auto& runtime = *h->runtime_a;
  auto executor = runtime.IoExecutor();
  bool ran = false;
  bool on_lane = false;
  executor->Post([&]() {
    ran = true;
    on_lane = executor->RunsTasksOnCurrentThread();
  });
  EXPECT_FALSE(ran) << "posted, not run inline";
  runtime.Drive();
  EXPECT_TRUE(ran);
  EXPECT_TRUE(on_lane);
  EXPECT_FALSE(executor->RunsTasksOnCurrentThread()) << "unclaimed and not driving";
}

TEST(IoExecutorTest, TimersRunOnAmpsClock) {
  auto h = harness();
  auto& runtime = *h->runtime_a;
  auto executor = runtime.IoExecutor();
  int fired = 0;
  executor->PostAfter(100ms, [&fired]() { ++fired; });
  const auto cancelled = executor->PostAfter(100ms, [&fired]() { fired += 10; });
  ASSERT_NE(cancelled, 0u);
  executor->CancelTimer(cancelled);
  h->clock->Advance(99);
  runtime.Drive();
  EXPECT_EQ(fired, 0);
  h->clock->Advance(1);
  runtime.Drive();
  EXPECT_EQ(fired, 1);
}

TEST(IoExecutorTest, RunAndWaitRunsOnTheCallerUnderTheIoLock) {
  auto h = harness();
  auto& runtime = *h->runtime_a;
  auto executor = runtime.IoExecutor();
  std::atomic<bool> drove{false};
  bool on_lane = false;
  std::thread driver;
  executor->RunAndWait([&]() {
    on_lane = executor->RunsTasksOnCurrentThread();
    executor->RunAndWait([]() {});  // nested: the io lock is recursive
    // A drive from another thread waits for this task: it is on the lane.
    driver = std::thread([&]() {
      runtime.Drive();
      drove = true;
    });
    std::this_thread::sleep_for(30ms);
    EXPECT_FALSE(drove.load());
  });
  driver.join();
  EXPECT_TRUE(drove.load()) << "the drive ran once the task left the lane";
  EXPECT_TRUE(on_lane);
  EXPECT_FALSE(executor->RunsTasksOnCurrentThread());
}

TEST(IoExecutorTest, OnTheClaimedDriverThreadBetweenDrives) {
  auto h = harness();
  auto& runtime = *h->runtime_a;
  runtime.ClaimDriver();
  EXPECT_TRUE(runtime.IoExecutor()->RunsTasksOnCurrentThread());
}

TEST(IoExecutorTest, OutlivingTheRuntimeDropsPostsAndRunsWaitsOnTheCaller) {
  std::shared_ptr<pp::SerialExecutor> executor;
  {
    auto h = harness();
    executor = h->runtime_a->IoExecutor();
  }
  auto sentinel = std::make_shared<int>(0);
  std::weak_ptr<int> watch = sentinel;
  executor->Post([sentinel]() { ++*sentinel; });
  sentinel.reset();
  EXPECT_TRUE(watch.expired()) << "the post was dropped";
  EXPECT_EQ(executor->PostAfter(1ms, []() {}), 0u);
  EXPECT_FALSE(executor->RunsTasksOnCurrentThread());
  bool ran = false;
  executor->RunAndWait([&ran]() { ran = true; });
  EXPECT_TRUE(ran);
}
