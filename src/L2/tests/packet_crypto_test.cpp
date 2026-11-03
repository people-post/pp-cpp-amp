#include "amp/L2/Session.h"
#include "amp/L2/SessionControl.h"
#include "amp/L2/SessionCrypto.h"
#include "amp/L2/SessionKeys.h"

#include <gtest/gtest.h>

#include <set>
#include <string>

namespace pp::amp {
namespace {

using adp::PacketSpace;

ByteVector CountingKey() {
  ByteVector key(kSessionKeyBytes);
  for (size_t i = 0; i < key.size(); ++i) {
    key[i] = static_cast<uint8_t>(i);
  }
  return key;
}

const std::vector<uint8_t> kHeader = {0x21, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0x07, 0x00, 0x00, 0x00};
const std::vector<uint8_t> kPlain = {'a', 'm', 'p'};

std::string Hex(const std::vector<uint8_t>& bytes) {
  static const char kDigits[] = "0123456789abcdef";
  std::string out;
  for (const uint8_t b : bytes) {
    out.push_back(kDigits[b >> 4]);
    out.push_back(kDigits[b & 0x0f]);
  }
  return out;
}

std::vector<uint8_t> Seal(const ByteVector& key, Direction direction, uint32_t epoch, PacketSpace space,
                          uint32_t number, const std::vector<uint8_t>& header = kHeader) {
  std::vector<uint8_t> out;
  EXPECT_TRUE(static_cast<bool>(SessionCrypto::SealPacket(key, direction, epoch, space, number, header, kPlain, out)));
  return out;
}

TEST(PacketCryptoTest, SealedPacketOpensWithTheSameSpaceAndNumber) {
  const auto sealed = Seal(CountingKey(), Direction::InitiatorToResponder, 1, PacketSpace::Reliable, 7);
  EXPECT_EQ(sealed.size(), kPlain.size() + adp::kPacketTagBytes);
  auto opened = SessionCrypto::OpenPacket(CountingKey(), Direction::InitiatorToResponder, 1, PacketSpace::Reliable, 7,
                                          kHeader, sealed);
  ASSERT_TRUE(static_cast<bool>(opened));
  EXPECT_EQ(*opened, kPlain);
}

TEST(PacketCryptoTest, SealAppendsAfterWhatTheBufferHolds) {
  std::vector<uint8_t> out = kHeader;
  ASSERT_TRUE(static_cast<bool>(SessionCrypto::SealPacket(CountingKey(), Direction::InitiatorToResponder, 1,
                                                          PacketSpace::BestEffort, 1, kHeader, kPlain, out)));
  ASSERT_EQ(out.size(), kHeader.size() + kPlain.size() + adp::kPacketTagBytes);
  EXPECT_TRUE(std::equal(kHeader.begin(), kHeader.end(), out.begin()));
}

// The nonce is never sent: both ends must build exactly these bytes.
TEST(PacketCryptoTest, NonceIsDirectionSpaceEpochAndNumberThenZeros) {
  const auto nonce = SessionCrypto::PacketNonce(Direction::ResponderToInitiator, PacketSpace::CarrierReliable,
                                                0x04030201u, 0x0d0c0b0au);
  const std::array<uint8_t, kAeadNonceSize> expected = {
      0x01, 0x03, 0x01, 0x02, 0x03, 0x04, 0x0a, 0x0b, 0x0c, 0x0d, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  EXPECT_EQ(nonce, expected);
}

// Every input that tells two packets apart tells their nonces apart: both directions, all four spaces, two epochs and
// a run of numbers never repeat a nonce (audio and video, best effort and reliable, carrier and direct).
TEST(PacketCryptoTest, NoncesNeverRepeatAcrossDirectionsSpacesEpochsAndNumbers) {
  std::set<std::array<uint8_t, kAeadNonceSize>> seen;
  size_t made = 0;
  for (const auto direction : {Direction::InitiatorToResponder, Direction::ResponderToInitiator}) {
    for (const auto space : {PacketSpace::BestEffort, PacketSpace::Reliable, PacketSpace::CarrierBestEffort,
                             PacketSpace::CarrierReliable}) {
      for (const uint32_t epoch : {1u, 2u}) {
        for (uint32_t number = 1; number <= 2000; ++number) {
          seen.insert(SessionCrypto::PacketNonce(direction, space, epoch, number));
          ++made;
        }
        seen.insert(SessionCrypto::PacketNonce(direction, space, epoch, 0xffffffffu));
        ++made;
      }
    }
  }
  EXPECT_EQ(seen.size(), made);
}

TEST(PacketCryptoTest, AnotherSpaceNumberEpochOrDirectionDoesNotOpen) {
  const ByteVector key = CountingKey();
  const auto sealed = Seal(key, Direction::InitiatorToResponder, 1, PacketSpace::Reliable, 7);
  const auto opens = [&](Direction direction, uint32_t epoch, PacketSpace space, uint32_t number) {
    return static_cast<bool>(SessionCrypto::OpenPacket(key, direction, epoch, space, number, kHeader, sealed));
  };
  EXPECT_FALSE(opens(Direction::InitiatorToResponder, 1, PacketSpace::BestEffort, 7));
  EXPECT_FALSE(opens(Direction::InitiatorToResponder, 1, PacketSpace::Reliable, 8));
  EXPECT_FALSE(opens(Direction::InitiatorToResponder, 2, PacketSpace::Reliable, 7));
  EXPECT_FALSE(opens(Direction::ResponderToInitiator, 1, PacketSpace::Reliable, 7));
}

// The clear header is the associated data: a changed type, connection id or number byte fails the tag.
TEST(PacketCryptoTest, ATamperedHeaderOrBodyDoesNotOpen) {
  const ByteVector key = CountingKey();
  const auto sealed = Seal(key, Direction::InitiatorToResponder, 1, PacketSpace::Reliable, 7);
  for (size_t i = 0; i < kHeader.size(); ++i) {
    auto header = kHeader;
    header[i] ^= 0x01;
    EXPECT_FALSE(static_cast<bool>(
        SessionCrypto::OpenPacket(key, Direction::InitiatorToResponder, 1, PacketSpace::Reliable, 7, header, sealed)))
        << "header byte " << i;
  }
  auto body = sealed;
  body[0] ^= 0x01;
  EXPECT_FALSE(static_cast<bool>(
      SessionCrypto::OpenPacket(key, Direction::InitiatorToResponder, 1, PacketSpace::Reliable, 7, kHeader, body)));
  EXPECT_FALSE(
      static_cast<bool>(SessionCrypto::OpenPacket(key, Direction::InitiatorToResponder, 1, PacketSpace::Reliable, 7,
                                                  kHeader, std::span<const uint8_t>(sealed).first(15))));
}

// Fixed known answer: a change to the nonce layout, the AEAD or the associated data fails here, not between two
// releases on the wire.
TEST(PacketCryptoTest, KnownAnswer) {
  const auto sealed = Seal(CountingKey(), Direction::InitiatorToResponder, 1, PacketSpace::Reliable, 7);
  // Cross-checked with libsodium from Python, the nonce built by hand: 00 01 01000000 07000000, then zeros.
  EXPECT_EQ(Hex(sealed), "98f1e8271b3d9bb0439c625ad7631fbc53b5ae");
}

ByteVector Ikm() {
  return ByteVector(32, 0x42);
}
ByteVector Transcript() {
  return ByteVector(32, 0x24);
}

std::pair<Session, Session> SessionPair() {
  auto a = SessionKeys::Derive(Ikm(), Transcript(), /*initiator=*/true, 1);
  auto b = SessionKeys::Derive(Ikm(), Transcript(), /*initiator=*/false, 1);
  EXPECT_TRUE(static_cast<bool>(a));
  EXPECT_TRUE(static_cast<bool>(b));
  auto sa = Session::FromMaterial(*a, Ikm(), Transcript());
  auto sb = Session::FromMaterial(*b, Ikm(), Transcript());
  EXPECT_TRUE(static_cast<bool>(sa));
  EXPECT_TRUE(static_cast<bool>(sb));
  return {std::move(*sa), std::move(*sb)};
}

TEST(PacketCryptoTest, OneEndsPacketOpensAtTheOther) {
  auto [a, b] = SessionPair();
  std::vector<uint8_t> out;
  ASSERT_TRUE(static_cast<bool>(a.SealPacket(PacketSpace::BestEffort, 3, kHeader, kPlain, out)));
  auto opened = b.OpenPacket(PacketSpace::BestEffort, 3, kHeader, out, 1000);
  ASSERT_TRUE(static_cast<bool>(opened));
  EXPECT_EQ(*opened, kPlain);
  EXPECT_FALSE(static_cast<bool>(a.OpenPacket(PacketSpace::BestEffort, 3, kHeader, out, 1000)))
      << "a packet does not open in its own direction";
}

// A packet sealed just before a rekey still opens in the grace window, and not after it.
TEST(PacketCryptoTest, APacketFromBeforeARekeyOpensOnlyInTheGraceWindow) {
  auto [a, b] = SessionPair();
  std::vector<uint8_t> out;
  ASSERT_TRUE(static_cast<bool>(a.SealPacket(PacketSpace::Reliable, 9, kHeader, kPlain, out)));
  ASSERT_TRUE(static_cast<bool>(b.ApplyRekey(2, 1000)));
  EXPECT_TRUE(static_cast<bool>(b.OpenPacket(PacketSpace::Reliable, 9, kHeader, out, 1000 + kSessionRekeyGraceMs)));
  EXPECT_FALSE(
      static_cast<bool>(b.OpenPacket(PacketSpace::Reliable, 9, kHeader, out, 1000 + kSessionRekeyGraceMs + 1)));
  ASSERT_TRUE(static_cast<bool>(a.ApplyRekey(2, 1000)));
  std::vector<uint8_t> after;
  ASSERT_TRUE(static_cast<bool>(a.SealPacket(PacketSpace::Reliable, 9, kHeader, kPlain, after)));
  EXPECT_NE(after, out) << "the same number under the new epoch is another packet";
  EXPECT_TRUE(static_cast<bool>(b.OpenPacket(PacketSpace::Reliable, 9, kHeader, after, 5000)));
}

} // namespace
} // namespace pp::amp
