#include "amp/L2/SessionKeys.h"

#include <gtest/gtest.h>

namespace pp::amp {
namespace {

TEST(SessionKeysTest, TranscriptHashDeterministic) {
  std::vector<ByteVector> parts = {ByteVector{'a', 'b'}, ByteVector{'c'}};
  auto h1 = SessionKeys::TranscriptHash(parts);
  auto h2 = SessionKeys::TranscriptHash(parts);
  ASSERT_TRUE(static_cast<bool>(h1));
  ASSERT_TRUE(static_cast<bool>(h2));
  EXPECT_EQ(*h1, *h2);
  EXPECT_EQ(h1->size(), 32u);
}

TEST(SessionKeysTest, DeriveDirectionalKeysDiffer) {
  ByteVector master(32, 0x11);
  ByteVector transcript(32, 0x22);
  auto initiator = SessionKeys::Derive(master, transcript, true, 1);
  auto responder = SessionKeys::Derive(master, transcript, false, 1);
  ASSERT_TRUE(static_cast<bool>(initiator));
  ASSERT_TRUE(static_cast<bool>(responder));
  EXPECT_EQ(initiator->k_assoc, responder->k_assoc);
  EXPECT_NE(initiator->k_send, responder->k_send);
  EXPECT_EQ(initiator->k_send, responder->k_recv);
  EXPECT_EQ(initiator->k_recv, responder->k_send);
}

TEST(SessionKeysTest, RekeyChangesSendRecv) {
  ByteVector master(32, 0x33);
  ByteVector transcript(32, 0x44);
  auto epoch1 = SessionKeys::Derive(master, transcript, true, 1);
  auto epoch2 = SessionKeys::Derive(master, transcript, true, 2);
  ASSERT_TRUE(static_cast<bool>(epoch1));
  ASSERT_TRUE(static_cast<bool>(epoch2));
  EXPECT_EQ(epoch1->k_assoc, epoch2->k_assoc);
  EXPECT_NE(epoch1->k_send, epoch2->k_send);
}

TEST(SessionKeysTest, RejectEmptyMasterIkm) {
  ByteVector transcript(32, 0x55);
  EXPECT_FALSE(static_cast<bool>(SessionKeys::Derive(ByteVector{}, transcript, true, 1)));
}

TEST(SessionKeysTest, RejectBadTranscriptHashSize) {
  ByteVector master(32, 0x66);
  EXPECT_FALSE(static_cast<bool>(SessionKeys::Derive(master, ByteVector{1, 2, 3}, true, 1)));
}

TEST(SessionKeysTest, AssocKeyDiffersFromDirectionalKeys) {
  ByteVector master(32, 0x77);
  ByteVector transcript(32, 0x88);
  auto material = SessionKeys::Derive(master, transcript, true, 1);
  ASSERT_TRUE(static_cast<bool>(material));
  EXPECT_NE(material->k_assoc, material->k_send);
  EXPECT_NE(material->k_assoc, material->k_recv);
}

TEST(SessionKeysTest, GoldenTranscriptHash) {
  const ByteVector part_a{'a', 'm', 'p'};
  const ByteVector part_b{'m', 's', 'h'};
  auto hash = SessionKeys::TranscriptHash({part_a, part_b});
  ASSERT_TRUE(static_cast<bool>(hash));
  EXPECT_EQ(hash->size(), 32u);
  const ByteVector expected = {
      0xae, 0xec, 0x57, 0x28, 0xee, 0x7b, 0x7d, 0xea, 0x49, 0x16, 0xd8, 0xab, 0x6e, 0x48, 0xcd, 0x00,
      0xda, 0x48, 0x43, 0xae, 0x7f, 0xc6, 0x01, 0xc4, 0x2b, 0xda, 0xd5, 0xb0, 0xbe, 0x58, 0xab, 0x48};
  EXPECT_EQ(*hash, expected);
}

} // namespace
} // namespace pp::amp
