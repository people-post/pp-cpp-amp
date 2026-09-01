#include "amp/L3/MessageReassembly.h"
#include "amp/L3/Types.h"

#include <gtest/gtest.h>

namespace pp::amp {
namespace {

ChannelFragBody MakeFrag(uint64_t msg_id, uint16_t index, uint16_t count, std::vector<uint8_t> chunk,
                         uint32_t total_len) {
  ChannelFragBody frag;
  frag.msg_id = msg_id;
  frag.frag_index = index;
  frag.frag_count = count;
  frag.total_len = total_len;
  frag.chunk = std::move(chunk);
  return frag;
}

TEST(MessageReassemblyTest, AssemblesInOrder) {
  MessageReassembly asmbl;
  auto f0 = MakeFrag(1, 0, 2, {'a', 'b'}, 4);
  auto f1 = MakeFrag(1, 1, 2, {'c', 'd'}, 4);

  auto p0 = asmbl.Push(f0, 0);
  ASSERT_TRUE(static_cast<bool>(p0));
  EXPECT_FALSE(p0->has_value());

  auto p1 = asmbl.Push(f1, 0);
  ASSERT_TRUE(static_cast<bool>(p1));
  ASSERT_TRUE(p1->has_value());
  const auto assembled = p1->value();
  EXPECT_EQ(assembled, (std::vector<uint8_t>{'a', 'b', 'c', 'd'}));
}

TEST(MessageReassemblyTest, DuplicateFragDropped) {
  MessageReassembly asmbl;
  auto f0 = MakeFrag(2, 0, 2, {'x'}, 2);
  auto f0dup = MakeFrag(2, 0, 2, {'x'}, 2);
  auto f1 = MakeFrag(2, 1, 2, {'y'}, 2);

  ASSERT_TRUE(static_cast<bool>(asmbl.Push(f0, 0)));
  auto dup = asmbl.Push(f0dup, 0);
  ASSERT_TRUE(static_cast<bool>(dup));
  EXPECT_FALSE(dup->has_value());
  auto done = asmbl.Push(f1, 0);
  ASSERT_TRUE(static_cast<bool>(done));
  ASSERT_TRUE(done->has_value());
  const auto assembled = done->value();
  EXPECT_EQ(assembled, (std::vector<uint8_t>{'x', 'y'}));
}

TEST(MessageReassemblyTest, AssemblesOutOfOrder) {
  MessageReassembly asmbl;
  auto f0 = MakeFrag(3, 0, 3, {'a'}, 3);
  auto f1 = MakeFrag(3, 1, 3, {'b'}, 3);
  auto f2 = MakeFrag(3, 2, 3, {'c'}, 3);

  auto p2 = asmbl.Push(f2, 0);
  ASSERT_TRUE(static_cast<bool>(p2));
  EXPECT_FALSE(p2->has_value());

  auto p0 = asmbl.Push(f0, 0);
  ASSERT_TRUE(static_cast<bool>(p0));
  EXPECT_FALSE(p0->has_value());

  auto p1 = asmbl.Push(f1, 0);
  ASSERT_TRUE(static_cast<bool>(p1));
  ASSERT_TRUE(p1->has_value());
  EXPECT_EQ(p1->value(), (std::vector<uint8_t>{'a', 'b', 'c'}));
}

TEST(MessageReassemblyTest, LossLeavesPartialIncomplete) {
  MessageReassembly asmbl;
  auto f0 = MakeFrag(4, 0, 2, {'x'}, 2);
  auto p0 = asmbl.Push(f0, 0);
  ASSERT_TRUE(static_cast<bool>(p0));
  EXPECT_FALSE(p0->has_value());
}

TEST(MessageReassemblyTest, SweepExpiredDropsStalePartial) {
  MessageReassembly asmbl;
  constexpr int64_t t0 = 1'000;
  auto f0 = MakeFrag(5, 0, 2, {'p'}, 2);
  ASSERT_TRUE(static_cast<bool>(asmbl.Push(f0, t0)));

  asmbl.SweepExpired(t0 + kDefaultFragAssemblyTimeoutMs + 1);

  auto f0_new = MakeFrag(5, 0, 2, {'q'}, 2);
  auto f1 = MakeFrag(5, 1, 2, {'r'}, 2);
  const int64_t t1 = t0 + kDefaultFragAssemblyTimeoutMs + 2;
  ASSERT_TRUE(static_cast<bool>(asmbl.Push(f0_new, t1)));
  auto done = asmbl.Push(f1, t1);
  ASSERT_TRUE(static_cast<bool>(done));
  ASSERT_TRUE(done->has_value());
  EXPECT_EQ(done->value(), (std::vector<uint8_t>{'q', 'r'}));
}

} // namespace
} // namespace pp::amp
