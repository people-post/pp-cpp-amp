#include "crypto/MlDsa.h"
#include "amp/L2/MshHandshake.h"
#include "amp/L2/Session.h"
#include "amp/L2/SessionControl.h"

#include <gtest/gtest.h>

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

TEST(SessionRekeyGraceTest, ApplyRekeyAcceptsPreviousEpochWithinGrace) {
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

  const std::vector<uint8_t> msg = {'g', 'r', 'a', 'c', 'e'};
  auto sealed = alice->Seal(3, 1, msg);
  ASSERT_TRUE(static_cast<bool>(sealed));

  ASSERT_TRUE(static_cast<bool>(bob->ApplyRekey(2, 1000)));
  EXPECT_TRUE(bob->HasGraceRecvKey(1500));
  auto opened = bob->Open(3, 1, *sealed, 1500);
  ASSERT_TRUE(static_cast<bool>(opened));
  EXPECT_EQ(*opened, msg);
}

TEST(SessionRekeyGraceTest, ApplyRekeyRejectsPreviousEpochAfterGrace) {
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

  const std::vector<uint8_t> msg = {'s', 't', 'a', 'l', 'e'};
  auto sealed = alice->Seal(4, 1, msg);
  ASSERT_TRUE(static_cast<bool>(sealed));

  ASSERT_TRUE(static_cast<bool>(bob->ApplyRekey(2, 1000)));
  const int64_t after_grace = 1000 + kSessionRekeyGraceMs + 1;
  EXPECT_FALSE(bob->HasGraceRecvKey(after_grace));
  auto opened = bob->Open(4, 1, *sealed, after_grace);
  EXPECT_FALSE(static_cast<bool>(opened));
}

} // namespace
} // namespace pp::amp
