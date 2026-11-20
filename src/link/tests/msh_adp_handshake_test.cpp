#include "amp/link/MshAdpHandshake.h"

#include "amp/link/AmpAdpCarrier.h"

#include "crypto/MlDsa.h"
#include "crypto/MlKem.h"

#include <deque>
#include <functional>
#include <optional>
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

/**
 * Queues wire deliveries instead of recursing into HandleMsh, matching real async transport. A message too large
 * for one frame goes in MshChunk pieces; the pump joins them (as PeerLink does) and queues the whole message.
 */
class Pump {
public:
  using Rewrite = std::function<std::vector<uint8_t>(std::vector<uint8_t>)>;

  void SetTargets(MshAdpHandshake* to_client, MshAdpHandshake* to_server) {
    to_client_ = to_client;
    to_server_ = to_server;
  }

  MshAdpHandshake::SendWire SendToServer(Rewrite rewrite = nullptr) {
    return [this, rewrite](std::vector<uint8_t> wire) -> Roe<void> {
      if (auto whole = Join(to_server_parts_, std::move(wire))) {
        queue_.push_back({true, rewrite ? rewrite(std::move(*whole)) : std::move(*whole)});
      }
      return Roe<void>();
    };
  }

  MshAdpHandshake::SendWire SendToClient(Rewrite rewrite = nullptr) {
    return [this, rewrite](std::vector<uint8_t> wire) -> Roe<void> {
      if (auto whole = Join(to_client_parts_, std::move(wire))) {
        queue_.push_back({false, rewrite ? rewrite(std::move(*whole)) : std::move(*whole)});
      }
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
  struct Parts {
    MshMessageType type{};
    std::vector<std::vector<uint8_t>> pieces;
    size_t have = 0;
  };

  /** The whole message once `wire` completes it (a single frame is whole already). */
  static std::optional<std::vector<uint8_t>> Join(Parts& parts, std::vector<uint8_t> wire) {
    auto kind = AmpAdpCarrier::DecodeKind(wire);
    if (!kind || *kind != AmpAdpPayloadKind::MshChunk) {
      return wire;
    }
    auto chunk = AmpAdpCarrier::DecodeMshChunk(wire);
    if (!chunk) {
      return std::nullopt;
    }
    const auto [type, index, count, piece] = *chunk;
    if (parts.pieces.size() != count) {
      parts = Parts{type, std::vector<std::vector<uint8_t>>(count), 0};
    }
    if (parts.pieces[index].empty()) {
      parts.pieces[index].assign(piece.begin(), piece.end());
      ++parts.have;
    }
    if (parts.have != count) {
      return std::nullopt;
    }
    std::vector<uint8_t> body;
    for (const auto& p : parts.pieces) {
      body.insert(body.end(), p.begin(), p.end());
    }
    parts = Parts{};
    auto whole = AmpAdpCarrier::EncodeMsh(type, body);
    return whole ? std::optional<std::vector<uint8_t>>(std::move(*whole)) : std::nullopt;
  }

  MshAdpHandshake* to_client_ = nullptr;
  MshAdpHandshake* to_server_ = nullptr;
  Parts to_server_parts_;
  Parts to_client_parts_;
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
      /*over_carrier=*/true);
  MshAdpHandshake server(
      MshAdpHandshake::Role::Responder, server_identity, pump.SendToClient(),
      [&](Roe<MshAdpEstablished> result) { server_result = std::move(result); },
      /*over_carrier=*/true);
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
      MshAdpHandshake::Role::Initiator, client_identity, pump.SendToServer([&](std::vector<uint8_t> wire) {
        auto type = AmpAdpCarrier::DecodeMshType(wire);
        if (type && *type == MshMessageType::ClientPayload) {
          return RebindPayloadToOtherKemKey(client_identity, wire);
        }
        return wire;
      }),
      [&](Roe<MshAdpEstablished>) {},
      /*over_carrier=*/true);
  MshAdpHandshake server(
      MshAdpHandshake::Role::Responder, server_identity, pump.SendToClient(),
      [&](Roe<MshAdpEstablished> result) { server_result = std::move(result); },
      /*over_carrier=*/true);
  pump.SetTargets(&client, &server);

  auto start = client.Start();
  auto drained = pump.Drain();
  // Rejection can surface from either the responder's HandleMsh return or its completion
  // callback; require a definite failure either way, and never a successful establishment.
  const bool rejected = !start || !drained || !server_result;
  EXPECT_TRUE(rejected);
  EXPECT_FALSE(static_cast<bool>(server_result));
}

/** `wire` ([kind][type][hello …]) with its hello's version byte set to `version`. */
std::vector<uint8_t> WithHelloVersion(std::vector<uint8_t> wire, const uint8_t version) {
  auto type = AmpAdpCarrier::DecodeMshType(wire);
  if (type && (*type == MshMessageType::ClientHello || *type == MshMessageType::ServerHello) && wire.size() > 3) {
    wire[3] = version; // [kind][type] then the hello: [type][version]
  }
  return wire;
}

// Release compatibility U4 / U5: a ServerHello of another MSH version fails the initiator at once as a version
// mismatch (its own code, the peer's version kept), not as a generic handshake failure.
TEST(MshAdpHandshakeTest, AServerHelloOfAnotherVersionFailsAsUnsupportedVersion) {
  Pump pump;
  std::optional<Roe<MshAdpEstablished>> client_result;
  MshAdpHandshake client(
      MshAdpHandshake::Role::Initiator, MakeIdentity(), pump.SendToServer(),
      [&](Roe<MshAdpEstablished> result) { client_result = std::move(result); }, /*over_carrier=*/true);
  MshAdpHandshake server(
      MshAdpHandshake::Role::Responder, MakeIdentity(),
      pump.SendToClient([](std::vector<uint8_t> wire) { return WithHelloVersion(std::move(wire), 2); }),
      [](Roe<MshAdpEstablished>) {}, /*over_carrier=*/true);
  pump.SetTargets(&client, &server);
  ASSERT_TRUE(static_cast<bool>(client.Start()));
  (void)pump.Drain();
  ASSERT_TRUE(client_result.has_value()) << "failed at once, not left to time out";
  ASSERT_FALSE(static_cast<bool>(*client_result));
  EXPECT_EQ(client_result->error().code, MshAdpHandshake::kErrUnsupportedVersion) << client_result->error().message;
  EXPECT_EQ(client.RemoteHelloVersion(), std::optional<uint8_t>(2));
  EXPECT_TRUE(client.IsComplete());
}

// The same on the responder's side: a ClientHello of another version.
TEST(MshAdpHandshakeTest, AClientHelloOfAnotherVersionFailsTheResponderAsUnsupportedVersion) {
  Pump pump;
  std::optional<Roe<MshAdpEstablished>> server_result;
  MshAdpHandshake client(
      MshAdpHandshake::Role::Initiator, MakeIdentity(),
      pump.SendToServer([](std::vector<uint8_t> wire) { return WithHelloVersion(std::move(wire), 0); }),
      [](Roe<MshAdpEstablished>) {}, /*over_carrier=*/true);
  MshAdpHandshake server(
      MshAdpHandshake::Role::Responder, MakeIdentity(), pump.SendToClient(),
      [&](Roe<MshAdpEstablished> result) { server_result = std::move(result); }, /*over_carrier=*/true);
  pump.SetTargets(&client, &server);
  ASSERT_TRUE(static_cast<bool>(client.Start()));
  (void)pump.Drain();
  ASSERT_TRUE(server_result.has_value());
  ASSERT_FALSE(static_cast<bool>(*server_result));
  EXPECT_EQ(server_result->error().code, MshAdpHandshake::kErrUnsupportedVersion);
  EXPECT_EQ(server.RemoteHelloVersion(), std::optional<uint8_t>(0));
}

// Release compatibility T9: an MSH message type this release does not know fails the handshake at once, with a
// code, instead of being ignored until the handshake times out.
TEST(MshAdpHandshakeTest, AnUnknownMessageTypeFailsTheHandshakeAtOnce) {
  for (const auto role : {MshAdpHandshake::Role::Initiator, MshAdpHandshake::Role::Responder}) {
    std::optional<Roe<MshAdpEstablished>> result;
    MshAdpHandshake handshake(
        role, MakeIdentity(), [](std::vector<uint8_t>) { return Roe<void>(); },
        [&](Roe<MshAdpEstablished> r) { result = std::move(r); });
    if (role == MshAdpHandshake::Role::Initiator) {
      ASSERT_TRUE(static_cast<bool>(handshake.Start()));
    }
    const std::vector<uint8_t> body = {6, 0, 1, 2};
    EXPECT_FALSE(static_cast<bool>(handshake.HandleMsh(static_cast<MshMessageType>(6), body)));
    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(static_cast<bool>(*result));
    EXPECT_EQ(result->error().code, MshAdpHandshake::kErrUnknownMessageType) << result->error().message;
    EXPECT_TRUE(handshake.IsComplete());
  }
}

// A known message out of turn is still ignored (a resend, a reordered packet): only an unknown type ends it.
TEST(MshAdpHandshakeTest, AKnownMessageOutOfTurnIsStillIgnored) {
  std::optional<Roe<MshAdpEstablished>> result;
  MshAdpHandshake responder(
      MshAdpHandshake::Role::Responder, MakeIdentity(), [](std::vector<uint8_t>) { return Roe<void>(); },
      [&](Roe<MshAdpEstablished> r) { result = std::move(r); });
  const std::vector<uint8_t> body = {static_cast<uint8_t>(MshMessageType::Finished), 0};
  EXPECT_FALSE(static_cast<bool>(responder.HandleMsh(MshMessageType::Finished, body)));
  EXPECT_FALSE(result.has_value());
  EXPECT_FALSE(responder.IsComplete());
}

} // namespace
} // namespace pp::amp
