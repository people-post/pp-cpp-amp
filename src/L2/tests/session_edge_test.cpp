#include "crypto/MlDsa.h"
#include "amp/L2/MshHandshake.h"
#include "amp/L2/Session.h"

#include <gtest/gtest.h>
#include <cstring>

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

SessionMaterial MakeValidMaterial() {
  SessionMaterial material;
  material.k_assoc = ByteVector(kAssocKeyBytes, 0x01);
  material.k_send = ByteVector(kSessionKeyBytes, 0x02);
  material.k_recv = ByteVector(kSessionKeyBytes, 0x03);
  material.session_epoch = 1;
  material.initiator = true;
  return material;
}

// Sealed as a reliable packet with an empty clear header: what a session's keys do for its link (A029 H1).
Roe<std::vector<uint8_t>> SealOne(const Session& session, const uint32_t number, std::span<const uint8_t> plain) {
  std::vector<uint8_t> out;
  if (auto sealed = session.SealPacket(adp::PacketSpace::Reliable, number, {}, plain, out); !sealed) {
    return sealed.error();
  }
  return out;
}

Roe<std::vector<uint8_t>> OpenOne(const Session& session, const uint32_t number, std::span<const uint8_t> sealed,
                                  const int64_t now_ms = 0) {
  return session.OpenPacket(adp::PacketSpace::Reliable, number, {}, sealed, now_ms);
}

TEST(SessionEdgeTest, FromMaterialRejectsBadKeySizes) {
  auto material = MakeValidMaterial();
  material.k_send.clear();
  ByteVector master(32, 0x11);
  ByteVector transcript(32, 0x22);
  EXPECT_FALSE(static_cast<bool>(Session::FromMaterial(material, master, transcript)));
}

TEST(SessionEdgeTest, FromMaterialRejectsEmptyMasterOrTranscript) {
  auto material = MakeValidMaterial();
  ByteVector master(32, 0x11);
  ByteVector transcript(32, 0x22);
  EXPECT_FALSE(static_cast<bool>(Session::FromMaterial(material, ByteVector{}, transcript)));
  EXPECT_FALSE(static_cast<bool>(Session::FromMaterial(material, master, ByteVector{})));
}

TEST(SessionEdgeTest, AssocKeyMatchesMaterial) {
  auto established = MshHandshake::Run(MakeIdentity(), MakeIdentity());
  ASSERT_TRUE(static_cast<bool>(established));
  auto session = Session::FromMaterial(established->initiator_material, established->master_ikm,
                                       established->transcript_hash);
  ASSERT_TRUE(static_cast<bool>(session));
  const auto assoc = session->AssocKey();
  EXPECT_EQ(0, std::memcmp(assoc.bytes.data(), session->Material().k_assoc.data(), kAssocKeyBytes));
}

TEST(SessionEdgeTest, BidirectionalSealOpenAfterHandshake) {
  auto established = MshHandshake::Run(MakeIdentity(), MakeIdentity());
  ASSERT_TRUE(static_cast<bool>(established));
  auto alice = Session::FromMaterial(established->initiator_material, established->master_ikm,
                                     established->transcript_hash);
  auto bob = Session::FromMaterial(established->responder_material, established->master_ikm,
                                   established->transcript_hash);
  ASSERT_TRUE(static_cast<bool>(alice));
  ASSERT_TRUE(static_cast<bool>(bob));

  const std::vector<uint8_t> a_to_b = {'a', '2', 'b'};
  auto sealed_ab = SealOne(*alice, 1, a_to_b);
  ASSERT_TRUE(static_cast<bool>(sealed_ab));
  auto opened_ab = OpenOne(*bob, 1, *sealed_ab);
  ASSERT_TRUE(static_cast<bool>(opened_ab));
  EXPECT_EQ(*opened_ab, a_to_b);

  const std::vector<uint8_t> b_to_a = {'b', '2', 'a'};
  auto sealed_ba = SealOne(*bob, 3, b_to_a);
  ASSERT_TRUE(static_cast<bool>(sealed_ba));
  auto opened_ba = OpenOne(*alice, 3, *sealed_ba);
  ASSERT_TRUE(static_cast<bool>(opened_ba));
  EXPECT_EQ(*opened_ba, b_to_a);
}

TEST(SessionEdgeTest, ApplyRekeyRejectsUnexpectedEpoch) {
  auto established = MshHandshake::Run(MakeIdentity(), MakeIdentity());
  ASSERT_TRUE(static_cast<bool>(established));
  auto session = Session::FromMaterial(established->initiator_material, established->master_ikm,
                                       established->transcript_hash);
  ASSERT_TRUE(static_cast<bool>(session));
  EXPECT_FALSE(static_cast<bool>(session->ApplyRekey(3, 1000)));
  EXPECT_FALSE(static_cast<bool>(session->ApplyRekey(1, 1000)));
}

TEST(SessionEdgeTest, RekeyClearsGraceState) {
  auto established = MshHandshake::Run(MakeIdentity(), MakeIdentity());
  ASSERT_TRUE(static_cast<bool>(established));
  auto bob = Session::FromMaterial(established->responder_material, established->master_ikm,
                                   established->transcript_hash);
  ASSERT_TRUE(static_cast<bool>(bob));
  ASSERT_TRUE(static_cast<bool>(bob->ApplyRekey(2, 1000)));
  EXPECT_TRUE(bob->HasGraceRecvKey(1500));
  ASSERT_TRUE(static_cast<bool>(bob->Rekey()));
  EXPECT_FALSE(bob->HasGraceRecvKey(1500));
}

TEST(SessionEdgeTest, RekeyInvalidatesPriorEpochTraffic) {
  auto established = MshHandshake::Run(MakeIdentity(), MakeIdentity());
  ASSERT_TRUE(static_cast<bool>(established));
  auto alice = Session::FromMaterial(established->initiator_material, established->master_ikm,
                                     established->transcript_hash);
  auto bob = Session::FromMaterial(established->responder_material, established->master_ikm,
                                   established->transcript_hash);
  ASSERT_TRUE(static_cast<bool>(alice));
  ASSERT_TRUE(static_cast<bool>(bob));

  const std::vector<uint8_t> epoch1 = {'e', 'p', '1'};
  auto sealed_epoch1 = SealOne(*alice, 1, epoch1);
  ASSERT_TRUE(static_cast<bool>(sealed_epoch1));
  ASSERT_TRUE(static_cast<bool>(alice->Rekey()));
  ASSERT_TRUE(static_cast<bool>(bob->Rekey()));
  EXPECT_FALSE(static_cast<bool>(OpenOne(*bob, 1, *sealed_epoch1)));

  const std::vector<uint8_t> epoch2 = {'e', 'p', '2'};
  auto sealed_epoch2 = SealOne(*alice, 2, epoch2);
  ASSERT_TRUE(static_cast<bool>(sealed_epoch2));
  ASSERT_TRUE(static_cast<bool>(OpenOne(*bob, 2, *sealed_epoch2)));
}

} // namespace
} // namespace pp::amp
