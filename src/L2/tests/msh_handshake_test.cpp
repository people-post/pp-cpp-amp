#include "crypto/MlDsa.h"
#include "amp/L2/MshHandshake.h"
#include "amp/L2/Session.h"

#include <gtest/gtest.h>
#include <stdexcept>

namespace pp::amp {
namespace {

MshIdentity MakeIdentity() {
  auto keys = pp::MlDsa::GenerateKeyPair();
  if (!keys) {
    throw std::runtime_error(keys.error().message);
  }
  MshIdentity id;
  id.ml_dsa_secret_key = std::move(keys->secret_key);
  id.ml_dsa_public_key = std::move(keys->public_key);
  return id;
}

TEST(MshHandshakeTest, RunEstablishesComplementarySessions) {
  auto alice_id = MakeIdentity();
  auto bob_id = MakeIdentity();
  auto established = MshHandshake::Run(alice_id, bob_id);
  ASSERT_TRUE(static_cast<bool>(established));

  auto alice = Session::FromMaterial(established->initiator_material, established->master_ikm,
                                     established->transcript_hash);
  auto bob = Session::FromMaterial(established->responder_material, established->master_ikm,
                                   established->transcript_hash);
  ASSERT_TRUE(static_cast<bool>(alice));
  ASSERT_TRUE(static_cast<bool>(bob));

  const std::vector<uint8_t> msg = {'p', 'i', 'n', 'g'};
  auto sealed = alice->Seal(1, 1, msg);
  ASSERT_TRUE(static_cast<bool>(sealed));
  auto opened = bob->Open(1, 1, *sealed);
  ASSERT_TRUE(static_cast<bool>(opened));
  EXPECT_EQ(*opened, msg);

  EXPECT_EQ(alice->AssocKey().bytes, bob->AssocKey().bytes);
}

TEST(MshHandshakeTest, TamperedFinishedFails) {
  auto alice_id = MakeIdentity();
  auto bob_id = MakeIdentity();
  auto established = MshHandshake::Run(alice_id, bob_id);
  ASSERT_TRUE(static_cast<bool>(established));

  std::vector<ByteVector> transcript = {ByteVector{1, 2, 3}};
  ByteVector master(32, 9);
  auto finished = MshHandshake::BuildFinished(master, transcript);
  ASSERT_TRUE(static_cast<bool>(finished));
  (*finished)[finished->size() - 1] ^= 0xFF;
  auto err = MshHandshake::VerifyFinished(master, transcript, *finished);
  EXPECT_FALSE(static_cast<bool>(err));
}

TEST(SessionTest, RekeyRotatesSendRecv) {
  auto alice_id = MakeIdentity();
  auto bob_id = MakeIdentity();
  auto established = MshHandshake::Run(alice_id, bob_id);
  ASSERT_TRUE(static_cast<bool>(established));
  auto session = Session::FromMaterial(established->initiator_material, established->master_ikm,
                                       established->transcript_hash);
  ASSERT_TRUE(static_cast<bool>(session));
  const auto send_before = session->Material().k_send;
  ASSERT_TRUE(static_cast<bool>(session->Rekey()));
  EXPECT_NE(session->Material().k_send, send_before);
  EXPECT_EQ(session->Material().session_epoch, 2u);
}

} // namespace
} // namespace pp::amp
