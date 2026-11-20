#include "crypto/MlDsa.h"
#include "crypto/MlKem.h"
#include "amp/L2/MshMessages.h"
#include "amp/L2/Types.h"

#include <cstring>

#include <gtest/gtest.h>

namespace pp::amp {
namespace {

MshHello MakeHello() {
  MshHello hello;
  hello.kem_public_key = ByteVector(pp::kMlKem768PublicKeyBytes, 0xAB);
  hello.nonce = ByteVector(kHandshakeNonceBytes, 0xCD);
  return hello;
}

MshPayload MakePayload() {
  MshPayload payload;
  payload.kem_ciphertext = ByteVector(pp::kMlKem768CiphertextBytes, 0x11);
  payload.identity_public_key = ByteVector(pp::kMlDsa65PublicKeyBytes, 0x22);
  payload.static_kem_public_key = ByteVector(pp::kMlKem768PublicKeyBytes, 0x33);
  payload.identity_signature = ByteVector(pp::kMlDsa65SignatureBytes, 0x44);
  return payload;
}

TEST(MshWireTest, HelloRoundTripClientAndServer) {
  const auto hello = MakeHello();
  auto client_wire = MshMessages::EncodeHello(MshMessageType::ClientHello, hello);
  auto server_wire = MshMessages::EncodeHello(MshMessageType::ServerHello, hello);
  ASSERT_TRUE(static_cast<bool>(client_wire));
  ASSERT_TRUE(static_cast<bool>(server_wire));

  auto client_decoded = MshMessages::DecodeHello(MshMessageType::ClientHello, *client_wire);
  auto server_decoded = MshMessages::DecodeHello(MshMessageType::ServerHello, *server_wire);
  ASSERT_TRUE(static_cast<bool>(client_decoded));
  ASSERT_TRUE(static_cast<bool>(server_decoded));
  EXPECT_EQ(client_decoded->version, kMshVersion);
  EXPECT_EQ(client_decoded->kem_public_key, hello.kem_public_key);
  EXPECT_EQ(client_decoded->nonce, hello.nonce);
  EXPECT_EQ(server_decoded->kem_public_key, hello.kem_public_key);
}

TEST(MshWireTest, PayloadRoundTripClientAndServer) {
  const auto payload = MakePayload();
  auto client_wire = MshMessages::EncodePayload(MshMessageType::ClientPayload, payload);
  auto server_wire = MshMessages::EncodePayload(MshMessageType::ServerPayload, payload);
  ASSERT_TRUE(static_cast<bool>(client_wire));
  ASSERT_TRUE(static_cast<bool>(server_wire));

  auto client_decoded = MshMessages::DecodePayload(MshMessageType::ClientPayload, *client_wire);
  auto server_decoded = MshMessages::DecodePayload(MshMessageType::ServerPayload, *server_wire);
  ASSERT_TRUE(static_cast<bool>(client_decoded));
  ASSERT_TRUE(static_cast<bool>(server_decoded));
  EXPECT_EQ(client_decoded->kem_ciphertext, payload.kem_ciphertext);
  EXPECT_EQ(client_decoded->identity_public_key, payload.identity_public_key);
  EXPECT_EQ(client_decoded->identity_signature, payload.identity_signature);
  EXPECT_EQ(server_decoded->static_kem_public_key, payload.static_kem_public_key);
}

TEST(MshWireTest, FinishedRoundTrip) {
  MshFinished finished;
  finished.mac = ByteVector(kFinishedMacBytes, 0x55);
  auto wire = MshMessages::EncodeFinished(finished);
  ASSERT_TRUE(static_cast<bool>(wire));
  auto decoded = MshMessages::DecodeFinished(*wire);
  ASSERT_TRUE(static_cast<bool>(decoded));
  EXPECT_EQ(decoded->mac, finished.mac);
}

TEST(MshWireTest, RejectWrongMessageType) {
  const auto hello = MakeHello();
  auto wire = MshMessages::EncodeHello(MshMessageType::ClientHello, hello);
  ASSERT_TRUE(static_cast<bool>(wire));
  EXPECT_FALSE(static_cast<bool>(MshMessages::DecodeHello(MshMessageType::ServerHello, *wire)));
}

TEST(MshWireTest, RejectTruncatedHello) {
  const auto hello = MakeHello();
  auto wire = MshMessages::EncodeHello(MshMessageType::ClientHello, hello);
  ASSERT_TRUE(static_cast<bool>(wire));
  wire->resize(wire->size() - 4);
  EXPECT_FALSE(static_cast<bool>(MshMessages::DecodeHello(MshMessageType::ClientHello, *wire)));
}

TEST(MshWireTest, RejectTrailingBytesOnHello) {
  const auto hello = MakeHello();
  auto wire = MshMessages::EncodeHello(MshMessageType::ClientHello, hello);
  ASSERT_TRUE(static_cast<bool>(wire));
  wire->push_back(0xFF);
  EXPECT_FALSE(static_cast<bool>(MshMessages::DecodeHello(MshMessageType::ClientHello, *wire)));
}

TEST(MshWireTest, RejectUnsupportedHelloVersion) {
  auto wire = MshMessages::EncodeHello(MshMessageType::ClientHello, MakeHello());
  ASSERT_TRUE(static_cast<bool>(wire));
  ASSERT_GT(wire->size(), 2u);
  (*wire)[1] = static_cast<uint8_t>(kMshVersion + 1);
  EXPECT_FALSE(static_cast<bool>(MshMessages::DecodeHello(MshMessageType::ClientHello, *wire)));
}

// Release compatibility U4: a hello of another MSH version fails with a code of its own (the handshake reports a
// version mismatch, not a broken peer), and its version can be read before anything else in it.
TEST(MshWireTest, AnotherHelloVersionFailsWithItsOwnCode) {
  auto wire = MshMessages::EncodeHello(MshMessageType::ServerHello, MakeHello());
  ASSERT_TRUE(static_cast<bool>(wire));
  (*wire)[1] = 2;
  auto decoded = MshMessages::DecodeHello(MshMessageType::ServerHello, *wire);
  ASSERT_FALSE(static_cast<bool>(decoded));
  EXPECT_EQ(decoded.error().code, kMshErrUnsupportedVersion);
  EXPECT_EQ(MshMessages::PeekHelloVersion(*wire), std::optional<uint8_t>(2));
  EXPECT_FALSE(MshMessages::PeekHelloVersion(std::vector<uint8_t>{2}).has_value());

  (*wire)[1] = kMshVersion;
  auto truncated = MshMessages::DecodeHello(MshMessageType::ServerHello, std::span(*wire).first(10));
  ASSERT_FALSE(static_cast<bool>(truncated));
  EXPECT_NE(truncated.error().code, kMshErrUnsupportedVersion) << "a truncated hello of our version is not one";
}

TEST(MshWireTest, KnownMessageTypes) {
  EXPECT_FALSE(IsKnownMshMessageType(0));
  for (uint8_t t = 1; t <= 5; ++t) {
    EXPECT_TRUE(IsKnownMshMessageType(t)) << int(t);
  }
  EXPECT_FALSE(IsKnownMshMessageType(6));
  EXPECT_FALSE(IsKnownMshMessageType(0xFF));
}

TEST(MshWireTest, RejectBadFieldSizesOnEncode) {
  auto hello = MakeHello();
  hello.nonce.clear();
  EXPECT_FALSE(static_cast<bool>(MshMessages::EncodeHello(MshMessageType::ClientHello, hello)));

  auto payload = MakePayload();
  payload.identity_signature.clear();
  EXPECT_FALSE(static_cast<bool>(MshMessages::EncodePayload(MshMessageType::ClientPayload, payload)));

  MshFinished finished;
  finished.mac = ByteVector(kFinishedMacBytes - 1, 0x01);
  EXPECT_FALSE(static_cast<bool>(MshMessages::EncodeFinished(finished)));
}

TEST(MshWireTest, BuildIdentitySignMessageRequiresFullKemKey) {
  EXPECT_FALSE(static_cast<bool>(MshMessages::BuildIdentitySignMessage(ByteVector(8, 0x01))));
  auto ok = MshMessages::BuildIdentitySignMessage(ByteVector(pp::kMlKem768PublicKeyBytes, 0x02));
  ASSERT_TRUE(static_cast<bool>(ok));
  EXPECT_EQ(ok->size(), std::strlen(kAmpIdentityBindPrefix) + pp::kMlKem768PublicKeyBytes);
}

} // namespace
} // namespace pp::amp
