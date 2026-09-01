#include "amp/L2/SessionCrypto.h"

#include <gtest/gtest.h>

namespace pp::amp {
namespace {

ByteVector TestKey(const uint8_t seed) { return ByteVector(kSessionKeyBytes, seed); }

TEST(SessionCryptoTest, SealOpenRoundTrip) {
  ByteVector key = TestKey(0x55);
  const std::vector<uint8_t> plain = {'h', 'e', 'l', 'l', 'o'};
  auto sealed = SessionCrypto::Seal(key, 1, 7, 3, Direction::InitiatorToResponder, plain);
  ASSERT_TRUE(static_cast<bool>(sealed));
  auto opened = SessionCrypto::Open(key, 1, 7, 3, Direction::InitiatorToResponder, *sealed);
  ASSERT_TRUE(static_cast<bool>(opened));
  EXPECT_EQ(*opened, plain);
}

TEST(SessionCryptoTest, WrongChannelSeqFailsOpen) {
  ByteVector key = TestKey(0x66);
  const std::vector<uint8_t> plain = {'x'};
  auto sealed = SessionCrypto::Seal(key, 1, 1, 1, Direction::InitiatorToResponder, plain);
  ASSERT_TRUE(static_cast<bool>(sealed));
  auto opened = SessionCrypto::Open(key, 1, 1, 2, Direction::InitiatorToResponder, *sealed);
  EXPECT_FALSE(static_cast<bool>(opened));
}

TEST(SessionCryptoTest, WrongChannelIdFailsOpen) {
  ByteVector key = TestKey(0x67);
  const std::vector<uint8_t> plain = {'y'};
  auto sealed = SessionCrypto::Seal(key, 1, 9, 1, Direction::InitiatorToResponder, plain);
  ASSERT_TRUE(static_cast<bool>(sealed));
  auto opened = SessionCrypto::Open(key, 1, 10, 1, Direction::InitiatorToResponder, *sealed);
  EXPECT_FALSE(static_cast<bool>(opened));
}

TEST(SessionCryptoTest, WrongEpochFailsOpen) {
  ByteVector key = TestKey(0x68);
  const std::vector<uint8_t> plain = {'z'};
  auto sealed = SessionCrypto::Seal(key, 1, 1, 1, Direction::InitiatorToResponder, plain);
  ASSERT_TRUE(static_cast<bool>(sealed));
  auto opened = SessionCrypto::Open(key, 2, 1, 1, Direction::InitiatorToResponder, *sealed);
  EXPECT_FALSE(static_cast<bool>(opened));
}

TEST(SessionCryptoTest, WrongDirectionFailsOpen) {
  ByteVector key = TestKey(0x69);
  const std::vector<uint8_t> plain = {'d'};
  auto sealed = SessionCrypto::Seal(key, 1, 1, 1, Direction::InitiatorToResponder, plain);
  ASSERT_TRUE(static_cast<bool>(sealed));
  auto opened = SessionCrypto::Open(key, 1, 1, 1, Direction::ResponderToInitiator, *sealed);
  EXPECT_FALSE(static_cast<bool>(opened));
}

TEST(SessionCryptoTest, WrongKeyFailsOpen) {
  const std::vector<uint8_t> plain = {'k'};
  auto sealed = SessionCrypto::Seal(TestKey(0x6A), 1, 1, 1, Direction::InitiatorToResponder, plain);
  ASSERT_TRUE(static_cast<bool>(sealed));
  auto opened = SessionCrypto::Open(TestKey(0x6B), 1, 1, 1, Direction::InitiatorToResponder, *sealed);
  EXPECT_FALSE(static_cast<bool>(opened));
}

TEST(SessionCryptoTest, TamperedCiphertextFailsOpen) {
  const std::vector<uint8_t> plain = {'t'};
  auto sealed = SessionCrypto::Seal(TestKey(0x6C), 1, 1, 1, Direction::InitiatorToResponder, plain);
  ASSERT_TRUE(static_cast<bool>(sealed));
  ASSERT_GT(sealed->size(), kAeadNonceSize);
  (*sealed)[kAeadNonceSize] ^= 0x80;
  auto opened = SessionCrypto::Open(TestKey(0x6C), 1, 1, 1, Direction::InitiatorToResponder, *sealed);
  EXPECT_FALSE(static_cast<bool>(opened));
}

TEST(SessionCryptoTest, SealedTooShortFailsOpen) {
  const std::vector<uint8_t> short_blob(kAeadNonceSize - 1, 0x01);
  auto opened = SessionCrypto::Open(TestKey(0x6D), 1, 1, 1, Direction::InitiatorToResponder, short_blob);
  EXPECT_FALSE(static_cast<bool>(opened));
}

TEST(SessionCryptoTest, BuildAadBindsDirectionByte) {
  auto aad_a = SessionCrypto::BuildAad(1, 2, 3, Direction::InitiatorToResponder);
  auto aad_b = SessionCrypto::BuildAad(1, 2, 3, Direction::ResponderToInitiator);
  ASSERT_EQ(aad_a.size(), 13u);
  EXPECT_NE(aad_a, aad_b);
  EXPECT_EQ(aad_a[12], static_cast<uint8_t>(Direction::InitiatorToResponder));
}

} // namespace
} // namespace pp::amp
