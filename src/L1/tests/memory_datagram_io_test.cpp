#include "amp/L1/MemoryDatagramIo.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <set>
#include <vector>

namespace {

std::vector<uint8_t> Byte(uint8_t v) { return {v}; }

TEST(MemoryDatagramIoTest, ReorderWindowDeliversAllAndCanPermute) {
  auto hub = pp::adp::MemoryDatagramIo::MakeHub();
  const auto addr_a = pp::adp::IpEndpoint::V4(10, 0, 0, 1, 1000);
  const auto addr_b = pp::adp::IpEndpoint::V4(10, 0, 0, 2, 2000);
  auto io_a = std::make_shared<pp::adp::MemoryDatagramIo>(hub, addr_a);
  auto io_b = std::make_shared<pp::adp::MemoryDatagramIo>(hub, addr_b);

  io_a->SetReorderWindow(2);
  io_a->SetRngSeed(0); // overflow pick is idx 1 → payload 2 first

  ASSERT_TRUE(io_a->SendTo(addr_b, Byte(1)).isOk());
  ASSERT_TRUE(io_a->SendTo(addr_b, Byte(2)).isOk());
  {
    auto empty = io_b->RecvFrom();
    ASSERT_TRUE(empty.isOk());
    EXPECT_FALSE(empty.value().has_value()) << "first two sends should stay buffered";
  }

  ASSERT_TRUE(io_a->SendTo(addr_b, Byte(3)).isOk());
  auto first = io_b->RecvFrom();
  ASSERT_TRUE(first.isOk());
  ASSERT_TRUE(first.value().has_value());
  const auto& first_pkt = *first.value();
  ASSERT_EQ(first_pkt.second.size(), 1u);
  EXPECT_EQ(first_pkt.second[0], 2) << "seed 0 should release payload 2 before 1";

  io_a->FlushReorder();
  std::set<uint8_t> got{first_pkt.second[0]};
  for (;;) {
    auto pkt = io_b->RecvFrom();
    ASSERT_TRUE(pkt.isOk());
    if (!pkt.value().has_value()) {
      break;
    }
    const auto& delivered = *pkt.value();
    ASSERT_EQ(delivered.second.size(), 1u);
    got.insert(delivered.second[0]);
  }
  const std::set<uint8_t> expected{1, 2, 3};
  EXPECT_EQ(got, expected);
}

TEST(MemoryDatagramIoTest, FlushReorderDrainsHeldDatagrams) {
  auto hub = pp::adp::MemoryDatagramIo::MakeHub();
  const auto addr_a = pp::adp::IpEndpoint::V4(10, 0, 0, 1, 1000);
  const auto addr_b = pp::adp::IpEndpoint::V4(10, 0, 0, 2, 2000);
  auto io_a = std::make_shared<pp::adp::MemoryDatagramIo>(hub, addr_a);
  auto io_b = std::make_shared<pp::adp::MemoryDatagramIo>(hub, addr_b);

  io_a->SetReorderWindow(4);
  ASSERT_TRUE(io_a->SendTo(addr_b, Byte(9)).isOk());
  ASSERT_TRUE(io_a->SendTo(addr_b, Byte(8)).isOk());

  auto before = io_b->RecvFrom();
  ASSERT_TRUE(before.isOk());
  EXPECT_FALSE(before.value().has_value());

  io_a->FlushReorder();
  std::vector<uint8_t> got;
  for (;;) {
    auto pkt = io_b->RecvFrom();
    ASSERT_TRUE(pkt.isOk());
    if (!pkt.value().has_value()) {
      break;
    }
    const auto& delivered = *pkt.value();
    ASSERT_EQ(delivered.second.size(), 1u);
    got.push_back(delivered.second[0]);
  }
  const std::vector<uint8_t> expected{9, 8};
  EXPECT_EQ(got, expected);
}

} // namespace
