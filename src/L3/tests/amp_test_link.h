#pragma once

#include "crypto/MlDsa.h"
#include "amp/L3/ChannelMux.h"
#include "amp/L2/MshHandshake.h"
#include "amp/L2/Session.h"

#include <memory>

namespace pp::amp::test {

struct AmpTestPeer {
  Session session;
  ChannelMux mux;

  explicit AmpTestPeer(Session s) : session(std::move(s)), mux(session) {}
};

struct AmpTestLink {
  AmpTestPeer initiator;
  AmpTestPeer responder;

  static Roe<std::unique_ptr<AmpTestLink>> Create() {
    auto alice_keys = pp::MlDsa::GenerateKeyPair();
    auto bob_keys = pp::MlDsa::GenerateKeyPair();
    if (!alice_keys || !bob_keys) {
      return Error("test link: keygen failed");
    }
    MshIdentity alice;
    alice.ml_dsa_secret_key = std::move(alice_keys->secret_key);
    alice.ml_dsa_public_key = std::move(alice_keys->public_key);
    MshIdentity bob;
    bob.ml_dsa_secret_key = std::move(bob_keys->secret_key);
    bob.ml_dsa_public_key = std::move(bob_keys->public_key);

    auto established = MshHandshake::Run(alice, bob);
    if (!established) {
      return established.error();
    }

    auto initiator_session =
        Session::FromMaterial(established->initiator_material, established->master_ikm, established->transcript_hash);
    if (!initiator_session) {
      return initiator_session.error();
    }
    auto responder_session =
        Session::FromMaterial(established->responder_material, established->master_ikm, established->transcript_hash);
    if (!responder_session) {
      return responder_session.error();
    }

    auto link = std::make_unique<AmpTestLink>(std::move(*initiator_session), std::move(*responder_session));
    link->WireTransport();
    return link;
  }

  AmpTestLink(Session init, Session resp) : initiator(std::move(init)), responder(std::move(resp)) {}

private:
  void WireTransport() {
    initiator.mux.SetPeerSession(&responder.session);
    responder.mux.SetPeerSession(&initiator.session);
    initiator.mux.SetTransport([this](uint32_t ch, uint32_t seq, adp::QosClass, std::vector<uint8_t> sealed) {
      (void)initiator.mux.LastSendQos();
      (void)responder.mux.OnSealedInbound(ch, seq, sealed);
    });
    responder.mux.SetTransport([this](uint32_t ch, uint32_t seq, adp::QosClass, std::vector<uint8_t> sealed) {
      (void)initiator.mux.OnSealedInbound(ch, seq, sealed);
    });
  }
};

} // namespace pp::amp::test
