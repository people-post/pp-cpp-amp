#include "crypto/MlDsa.h"
#include "crypto/MlKem.h"
#include "amp/L2/MshHandshake.h"
#include "amp/L2/MshMessages.h"
#include "amp/L2/SessionKeys.h"

#include <gtest/gtest.h>

namespace pp::amp {
namespace {

MshPayload MakeSignedPayload() {
  auto keys = pp::MlDsa::GenerateKeyPair();
  auto kem = pp::MlKem::GenerateKeyPair();
  if (!keys || !kem) {
    throw std::runtime_error("keygen failed");
  }
  MshPayload payload;
  payload.kem_ciphertext = ByteVector(pp::kMlKem768CiphertextBytes, 0x01);
  payload.identity_public_key = keys->public_key;
  payload.static_kem_public_key = kem->public_key;
  auto sign_msg = MshMessages::BuildIdentitySignMessage(payload.static_kem_public_key);
  if (!sign_msg) {
    throw std::runtime_error("sign message failed");
  }
  auto sig = pp::MlDsa::Sign(keys->secret_key, *sign_msg);
  if (!sig) {
    throw std::runtime_error("sign failed");
  }
  payload.identity_signature = std::move(*sig);
  return payload;
}

TEST(MshEdgeTest, InvalidIdentitySignatureRejected) {
  auto payload = MakeSignedPayload();
  ASSERT_FALSE(payload.identity_signature.empty());
  payload.identity_signature[0] ^= 0xFF;

  auto sign_msg = MshMessages::BuildIdentitySignMessage(payload.static_kem_public_key);
  ASSERT_TRUE(static_cast<bool>(sign_msg));
  auto ok = pp::MlDsa::Verify(payload.identity_public_key, *sign_msg, payload.identity_signature);
  ASSERT_TRUE(static_cast<bool>(ok));
  EXPECT_FALSE(*ok);
}

TEST(MshEdgeTest, TranscriptHashOrderMatters) {
  const ByteVector part_a{'h', 'e', 'l', 'l', 'o'};
  const ByteVector part_b{'w', 'o', 'r', 'l', 'd'};
  auto forward = SessionKeys::TranscriptHash({part_a, part_b});
  auto reverse = SessionKeys::TranscriptHash({part_b, part_a});
  ASSERT_TRUE(static_cast<bool>(forward));
  ASSERT_TRUE(static_cast<bool>(reverse));
  EXPECT_NE(*forward, *reverse);
}

TEST(MshEdgeTest, VerifyFinishedRejectsBitFlip) {
  ByteVector master(32, 0x42);
  std::vector<ByteVector> transcript = {ByteVector{'a'}, ByteVector{'b'}};
  auto finished = MshHandshake::BuildFinished(master, transcript);
  ASSERT_TRUE(static_cast<bool>(finished));
  (*finished)[finished->size() - 1] ^= 0x80;
  EXPECT_FALSE(static_cast<bool>(MshHandshake::VerifyFinished(master, transcript, *finished)));
}

TEST(MshEdgeTest, VerifyFinishedRejectsWrongMasterIkm) {
  ByteVector master(32, 0x42);
  std::vector<ByteVector> transcript = {ByteVector{'x'}};
  auto finished = MshHandshake::BuildFinished(master, transcript);
  ASSERT_TRUE(static_cast<bool>(finished));
  ByteVector other_master(32, 0x43);
  EXPECT_FALSE(static_cast<bool>(MshHandshake::VerifyFinished(other_master, transcript, *finished)));
}

} // namespace
} // namespace pp::amp
