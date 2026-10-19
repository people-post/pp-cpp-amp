#include "amp/link/PeerId.h"
#include "crypto/MlDsa.h"

#include <gtest/gtest.h>

// Links name peers by the mesh PeerId (pp-cpp-crypto's derivation; golden vectors there).

TEST(PeerIdTest, TheHookDerivesTheMeshPeerIdOfAnMlDsa65Key) {
  const pp::amp::ByteVector key42(pp::kMlDsa65PublicKeyBytes, 0x42);
  EXPECT_EQ(pp::amp::PeerIdFromIdentityKey(key42), "QmTWkSQAcGsETogTFKrPJ3GzdHG3vP9UwXVyp2HeK9YBvP");
  EXPECT_EQ(pp::amp::PeerIdFromIdentityKey(pp::amp::ByteVector(32, 0x01)), "")
      << "not an ML-DSA-65 key: the link falls back to its key fingerprint";
}

TEST(PeerIdTest, FromMultiaddr) {
  EXPECT_EQ(pp::amp::PeerIdFromMultiaddr("/ip4/10.0.0.1/udp/4001/adp/1.0.0/p2p/QmPeer"), "QmPeer");
  EXPECT_EQ(pp::amp::PeerIdFromMultiaddr("/p2p/QmRelay/p2p-circuit/p2p/QmTarget"), "QmTarget") << "the last one";
  EXPECT_EQ(pp::amp::PeerIdFromMultiaddr("/p2p/QmPeer/extra"), "QmPeer");
  EXPECT_EQ(pp::amp::PeerIdFromMultiaddr("/ip4/10.0.0.1/udp/4001"), "");
}
