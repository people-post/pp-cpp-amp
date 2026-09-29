#include "amp/link/MshAdpHandshake.h"

#include "amp/link/AmpAdpCarrier.h"

#include "crypto/MlDsa.h"
#include "crypto/MlKem.h"

#include <deque>
#include <functional>
#include <gtest/gtest.h>

namespace pp::amp {
namespace {

MshIdentity MakeIdentity() {
  auto keys = pp::MlDsa::GenerateKeyPair();
  if (!keys) {
    throw std::runtime_error("keygen failed");
  }
  MshIdentity identity;
  identity.ml_dsa_secret_key = keys->secret_key;
  identity.ml_dsa_public_key = keys->public_key;
  return identity;
}

/** Re-signs a ClientPayload with a KEM key that does not match the ClientHello already sent. */
std::vector<uint8_t> RebindPayloadToOtherKemKey(const MshIdentity& identity, std::span<const uint8_t> wire) {
  auto body = AmpAdpCarrier::DecodeMshBody(wire);
  if (!body) {
    throw std::runtime_error("decode msh body failed");
  }
  auto payload = MshMessages::DecodePayload(MshMessageType::ClientPayload, *body);
  if (!payload) {
    throw std::runtime_error("decode payload failed");
  }
  auto other_kem = pp::MlKem::GenerateKeyPair();
  if (!other_kem) {
    throw std::runtime_error("kem keygen failed");
  }
  payload->static_kem_public_key = other_kem->public_key;
  auto sign_msg = MshMessages::BuildIdentitySignMessage(payload->static_kem_public_key);
  if (!sign_msg) {
    throw std::runtime_error("sign message failed");
  }
  auto sig = pp::MlDsa::Sign(identity.ml_dsa_secret_key, *sign_msg);
  if (!sig) {
    throw std::runtime_error("sign failed");
  }
  payload->identity_signature = std::move(*sig);
  auto re_encoded = MshMessages::EncodePayload(MshMessageType::ClientPayload, *payload);
  if (!re_encoded) {
    throw std::runtime_error("encode payload failed");
  }
  auto re_wire = AmpAdpCarrier::EncodeMsh(MshMessageType::ClientPayload, *re_encoded);
  if (!re_wire) {
    throw std::runtime_error("encode msh failed");
  }
  return *re_wire;
}

/** Queues wire deliveries instead of recursing into HandleMsh, matching real async transport. */
class Pump {
public:
  using Rewrite = std::function<std::vector<uint8_t>(std::vector<uint8_t>)>;

  void SetTargets(MshAdpHandshake* to_client, MshAdpHandshake* to_server) {
    to_client_ = to_client;
    to_server_ = to_server;
  }

  MshAdpHandshake::SendWire SendToServer(Rewrite rewrite = nullptr) {
    return [this, rewrite](std::vector<uint8_t> wire) -> Roe<void> {
      queue_.push_back({true, rewrite ? rewrite(std::move(wire)) : std::move(wire)});
      return Roe<void>();
    };
  }

  MshAdpHandshake::SendWire SendToClient() {
    return [this](std::vector<uint8_t> wire) -> Roe<void> {
      queue_.push_back({false, std::move(wire)});
      return Roe<void>();
    };
  }

  /** Delivers everything queued so far, including what deliveries themselves enqueue. */
  Roe<void> Drain() {
    while (!queue_.empty()) {
      auto [to_server, wire] = std::move(queue_.front());
      queue_.pop_front();
      auto type = AmpAdpCarrier::DecodeMshType(wire);
      auto body = AmpAdpCarrier::DecodeMshBody(wire);
      if (!type || !body) {
        return Error("decode failed");
      }
      auto* target = to_server ? to_server_ : to_client_;
      if (auto handled = target->HandleMsh(*type, *body); !handled) {
        return handled;
      }
    }
    return Roe<void>();
  }

private:
  MshAdpHandshake* to_client_ = nullptr;
  MshAdpHandshake* to_server_ = nullptr;
  std::deque<std::pair<bool, std::vector<uint8_t>>> queue_;
};

TEST(MshAdpHandshakeTest, CompletesWithMatchingIdentities) {
  auto client_identity = MakeIdentity();
  auto server_identity = MakeIdentity();

  Pump pump;
  Roe<MshAdpEstablished> client_result = Error("not completed");
  Roe<MshAdpEstablished> server_result = Error("not completed");

  MshAdpHandshake client(
      MshAdpHandshake::Role::Initiator, client_identity, pump.SendToServer(),
      [&](Roe<MshAdpEstablished> result) { client_result = std::move(result); },
      /*chunked_wire=*/false);
  MshAdpHandshake server(
      MshAdpHandshake::Role::Responder, server_identity, pump.SendToClient(),
      [&](Roe<MshAdpEstablished> result) { server_result = std::move(result); },
      /*chunked_wire=*/false);
  pump.SetTargets(&client, &server);

  auto start = client.Start();
  ASSERT_TRUE(static_cast<bool>(start)) << start.error().message;
  auto drained = pump.Drain();
  ASSERT_TRUE(static_cast<bool>(drained)) << drained.error().message;

  ASSERT_TRUE(static_cast<bool>(client_result)) << client_result.error().message;
  ASSERT_TRUE(static_cast<bool>(server_result)) << server_result.error().message;
  EXPECT_EQ(client_result->remote_identity_public_key, server_identity.ml_dsa_public_key);
  EXPECT_EQ(server_result->remote_identity_public_key, client_identity.ml_dsa_public_key);
}

// Regression: a ClientPayload signed for a KEM key other than the one already sent in
// ClientHello must be rejected, not accepted as proof of the signer's identity.
TEST(MshAdpHandshakeTest, RejectsPayloadKemKeyMismatchingHello) {
  auto client_identity = MakeIdentity();
  auto server_identity = MakeIdentity();

  Pump pump;
  Roe<MshAdpEstablished> server_result = Error("not completed");

  MshAdpHandshake client(
      MshAdpHandshake::Role::Initiator, client_identity,
      pump.SendToServer([&](std::vector<uint8_t> wire) {
        auto type = AmpAdpCarrier::DecodeMshType(wire);
        if (type && *type == MshMessageType::ClientPayload) {
          return RebindPayloadToOtherKemKey(client_identity, wire);
        }
        return wire;
      }),
      [&](Roe<MshAdpEstablished>) {},
      /*chunked_wire=*/false);
  MshAdpHandshake server(
      MshAdpHandshake::Role::Responder, server_identity, pump.SendToClient(),
      [&](Roe<MshAdpEstablished> result) { server_result = std::move(result); },
      /*chunked_wire=*/false);
  pump.SetTargets(&client, &server);

  auto start = client.Start();
  auto drained = pump.Drain();
  // Rejection can surface from either the responder's HandleMsh return or its completion
  // callback; require a definite failure either way, and never a successful establishment.
  const bool rejected = !start || !drained || !server_result;
  EXPECT_TRUE(rejected);
  EXPECT_FALSE(static_cast<bool>(server_result));
}

} // namespace
} // namespace pp::amp
