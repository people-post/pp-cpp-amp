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

  explicit AmpTestPeer(Session s) : session(std::move(s)), mux(session.Material().initiator) {}
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
    // Frames cross in the clear: the link seals them on a real stack (L1 / PeerLink tests cover that).
    initiator.mux.SetTransport([this](adp::QosClass, std::vector<uint8_t> frame) {
      (void)responder.mux.OnFrameInbound(frame);
      return Roe<void>();
    });
    responder.mux.SetTransport([this](adp::QosClass, std::vector<uint8_t> frame) {
      (void)initiator.mux.OnFrameInbound(frame);
      return Roe<void>();
    });
  }
};

} // namespace pp::amp::test
