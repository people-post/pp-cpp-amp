#pragma once

#include "amp/L1/Connection.h"
#include "amp/L1/Endpoint.h"
#include "amp/L1/Types.h"
#include "amp/L3/ChannelPolicy.h"
#include "amp/L3/ChannelWire.h"
#include "amp/L3/Types.h"
#include "amp/link/AmpAdpCarrier.h"
#include "crypto/MlDsa.h"
#include "amp/link/AdpMultiaddr.h"
#include "support/mesh_harness_support.h"
#include "support/mesh_test_harness.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace pbr::test {

enum class HarnessSide { A, B };

/** Helpers for AMP L1–L3 integration tests (Track A). */
struct AmpIntegrationHarness : AmpMeshHarness {
  pp::amp::PeerLinkManager& Mgr(const HarnessSide side) {
    return side == HarnessSide::A ? mgr_a() : mgr_b();
  }

  pp::adp::Endpoint& Ep(const HarnessSide side) { return side == HarnessSide::A ? *ep_a : *ep_b; }

  std::shared_ptr<pp::adp::MemoryDatagramIo>& Io(const HarnessSide side) {
    return side == HarnessSide::A ? io_a : io_b;
  }

  bool Associate(const std::string& alias_a = "b", const std::string& alias_b = "a") {
    ep_b->SetAcceptEnabled(true);
    if (!static_cast<bool>(mgr_a().RegisterEndpoint(alias_a, ma_b))) {
      return false;
    }
    if (!static_cast<bool>(mgr_b().RegisterEndpoint(alias_b, ma_a))) {
      return false;
    }
    bool done = false;
    bool ok = false;
    mgr_a().EnsureAssociation(alias_a, [&](pp::amp::PeerLinkManager::LinkRoe result) {
      ok = result.isOk();
      done = true;
    });
    PumpUntil([&] { return done && mgr_a().IsConnected(alias_a) && mgr_b().FindLinkByPeerId(peer_id_a) != nullptr; });
    if (!ok || !mgr_a().IsConnected(alias_a)) {
      return false;
    }
    bool ready = false;
    PumpUntil([&] {
      auto* link = mgr_a().FindLink(alias_a);
      ready = link && link->Mux() && link->Mux()->State(pp::amp::kCapabilityChannelId) == pp::amp::ChannelState::Open
              && link->RemoteCapability() != nullptr;
      return ready;
    });
    return ready;
  }

  bool DualAssociate(const std::string& alias_a = "b", const std::string& alias_b = "a") {
    ep_a->SetAcceptEnabled(true);
    ep_b->SetAcceptEnabled(true);
    if (!static_cast<bool>(mgr_a().RegisterEndpoint(alias_a, ma_b))) {
      return false;
    }
    if (!static_cast<bool>(mgr_b().RegisterEndpoint(alias_b, ma_a))) {
      return false;
    }
    bool done_a = false;
    bool done_b = false;
    bool ok_a = false;
    bool ok_b = false;
    mgr_a().EnsureAssociation(alias_a, [&](pp::amp::PeerLinkManager::LinkRoe result) {
      ok_a = result.isOk();
      done_a = true;
    });
    mgr_b().EnsureAssociation(alias_b, [&](pp::amp::PeerLinkManager::LinkRoe result) {
      ok_b = result.isOk();
      done_b = true;
    });
    PumpUntil([&] {
      return done_a && done_b && mgr_a().CountConnectedLinksForPeerId(peer_id_b) == 1
             && mgr_b().CountConnectedLinksForPeerId(peer_id_a) == 1;
    });
    return ok_a && ok_b;
  }

  std::optional<uint32_t> OpenChannel(const HarnessSide side, const std::string& alias, const std::string& protocol_id,
                                    pp::amp::ChannelPolicy policy) {
    bool done = false;
    std::optional<uint32_t> channel_id;
    Mgr(side).OpenChannel(alias, protocol_id, policy, [&](pp::amp::PeerLinkManager::ChannelRoe ch) {
      if (ch.isOk()) {
        channel_id = ch.value();
      }
      done = true;
    });
    PumpUntil([&] { return done; });
    if (!channel_id.has_value()) {
      return std::nullopt;
    }
    bool channel_open = false;
    PumpUntil([&] {
      auto* link = Mgr(side).FindLink(alias);
      channel_open = link && link->Mux() && link->Mux()->State(*channel_id) == pp::amp::ChannelState::Open;
      return channel_open;
    });
    return channel_id;
  }

  bool WaitCh0Open(const HarnessSide side, const std::string& alias) {
    bool ch0_open = false;
    PumpUntil([&] {
      auto* link = Mgr(side).FindLink(alias);
      ch0_open = link && link->Mux() && link->Mux()->State(pp::amp::kCapabilityChannelId) == pp::amp::ChannelState::Open;
      return ch0_open;
    });
    return ch0_open;
  }

  bool SendMuxData(const HarnessSide side, const std::string& alias, const uint32_t channel_id,
                   std::vector<uint8_t> payload) {
    auto* link = Mgr(side).FindLink(alias);
    if (!link || !link->Mux()) {
      return false;
    }
    if (!static_cast<bool>(link->Mux()->SendData(channel_id, std::move(payload)))) {
      return false;
    }
    PumpBoth();
    return true;
  }

  bool PumpUntilReceived(std::vector<uint8_t>& out, const std::function<bool()>& try_read,
                         const size_t max_rounds = 500) {
    for (size_t i = 0; i < max_rounds; ++i) {
      if (try_read()) {
        return true;
      }
      PumpBoth();
    }
    return try_read();
  }

  void ConfigureLoss(const HarnessSide side, const size_t drop_next) {
    Io(side)->DropNext(drop_next);
  }

  void AdvanceMs(const int64_t delta_ms) {
    clock->Advance(delta_ms);
    PumpBoth();
  }

  size_t CountLinks(const HarnessSide side) { return Mgr(side).CountLinks(); }

  void PumpBudget(const size_t rounds) {
    for (size_t i = 0; i < rounds; ++i) {
      PumpBoth();
    }
  }

  /** Inject arbitrary ADP payload on an existing link connection (post- or mid-handshake). */
  bool InjectAdpPayload(const HarnessSide side, const std::string& alias, std::vector<uint8_t> payload) {
    auto* link = Mgr(side).FindLink(alias);
    if (!link) {
      return false;
    }
    auto* conn = link->ConnectionOrNull();
    if (!conn) {
      return false;
    }
    return static_cast<bool>(conn->Send(pp::adp::QosClass::Reliable, payload));
  }

  /** Inject syntactically valid but cryptographically invalid sealed carrier frames. */
  bool InjectSealedGarbage(const HarnessSide side, const std::string& alias, const uint32_t channel_id,
                           const uint32_t channel_seq, const size_t garbage_len = 64) {
    std::vector<uint8_t> garbage(garbage_len, 0xDE);
    auto wire = pp::amp::AmpAdpCarrier::EncodeSealed(channel_id, channel_seq, garbage);
    if (!wire) {
      return false;
    }
    return InjectAdpPayload(side, alias, std::move(*wire));
  }

  /** Inject invalid MSH-shaped carrier garbage during handshake. */
  bool InjectMshGarbage(const HarnessSide side, const std::string& alias) {
    std::vector<uint8_t> garbage = {static_cast<uint8_t>(pp::amp::AmpAdpPayloadKind::Msh), 0xFF, 0x00, 0x01, 0x02};
    return InjectAdpPayload(side, alias, std::move(garbage));
  }

  /** Deliver a raw UDP datagram (bypasses ADP connection state). */
  bool InjectRawDatagram(const HarnessSide from, const pp::adp::IpEndpoint& to, std::vector<uint8_t> datagram) {
    return static_cast<bool>(Io(from)->SendTo(to, datagram));
  }

  /** Send one sealed FRAG frame (for partial-assembly adversarial tests). */
  bool InjectPartialFrag(const HarnessSide side, const std::string& alias, const uint32_t channel_id,
                         const uint32_t channel_seq, const uint64_t msg_id, const uint16_t frag_index,
                         const uint16_t frag_count, const uint32_t total_len, std::vector<uint8_t> chunk) {
    auto* link = Mgr(side).FindLink(alias);
    if (!link || !link->GetSession() || !link->Mux()) {
      return false;
    }
    pp::amp::ChannelFrame frame;
    frame.header.frame_type = pp::amp::ChannelFrameType::Frag;
    frame.header.channel_id = channel_id;
    frame.header.channel_seq = channel_seq;
    frame.frag.msg_id = msg_id;
    frame.frag.frag_index = frag_index;
    frame.frag.frag_count = frag_count;
    frame.frag.total_len = total_len;
    frame.frag.chunk = std::move(chunk);
    auto wire = pp::amp::ChannelWire::Encode(frame);
    if (!wire) {
      return false;
    }
    auto sealed = link->GetSession()->Seal(channel_id, channel_seq, *wire);
    if (!sealed) {
      return false;
    }
    return static_cast<bool>(link->Mux()->InjectSealedForTest(channel_id, channel_seq, std::move(*sealed)));
  }

  /** Send one sealed L3 DATA frame from an alternate local UDP path (NAT handoff). */
  bool SendSealedFromAlternatePath(const HarnessSide sender, const std::string& alias, const uint32_t channel_id,
                                   const pp::adp::IpEndpoint& alt_local, std::vector<uint8_t> payload,
                                   uint32_t channel_seq = 1) {
    auto* link = Mgr(sender).FindLink(alias);
    if (!link || !link->GetSession() || !link->ConnectionOrNull()) {
      return false;
    }
    pp::amp::ChannelFrame frame;
    frame.header.frame_type = pp::amp::ChannelFrameType::Data;
    frame.header.channel_id = channel_id;
    frame.header.channel_seq = channel_seq;
    frame.payload = std::move(payload);
    auto wire = pp::amp::ChannelWire::Encode(frame);
    if (!wire) {
      return false;
    }
    auto sealed = link->GetSession()->Seal(channel_id, channel_seq, *wire);
    if (!sealed) {
      return false;
    }
    auto carrier = pp::amp::AmpAdpCarrier::EncodeSealed(channel_id, channel_seq, *sealed);
    if (!carrier) {
      return false;
    }

    auto* conn = link->ConnectionOrNull();
    pp::adp::OpenParams params;
    params.key = link->GetSession()->AssocKey();
    params.id = conn->Id();
    params.mint_id = false;
    params.peer = sender == HarnessSide::A ? addr_b : addr_a;

    migrate_io_ = std::make_shared<pp::adp::MemoryDatagramIo>(hub, alt_local);
    migrate_ep_ = std::make_unique<pp::adp::Endpoint>(migrate_io_, clock);
    auto opened = migrate_ep_->Open(params);
    if (!opened) {
      migrate_ep_.reset();
      migrate_io_.reset();
      return false;
    }
    migrate_conn_ = *opened;
    if (!static_cast<bool>(migrate_conn_->Send(pp::adp::QosClass::Reliable, *carrier))) {
      return false;
    }
    migrate_ep_->Tick();
    migrate_conn_->Tick(clock->NowMs());
    PumpBoth();
    return true;
  }

  std::shared_ptr<pp::adp::MemoryDatagramIo> migrate_io_;
  std::unique_ptr<pp::adp::Endpoint> migrate_ep_;
  std::shared_ptr<pp::adp::Connection> migrate_conn_;

  bool RequestRekey(const HarnessSide side, const std::string& alias) {
    if (!WaitCh0Open(side, alias)) {
      return false;
    }
    auto* link = Mgr(side).FindLink(alias);
    if (!link) {
      return false;
    }
    bool done = false;
    bool ok = false;
    link->RequestSessionRekey([&](pp::Roe<void> result) {
      ok = result.isOk();
      done = true;
    });
    PumpUntil([&] { return done; });
    return ok;
  }
};

pp::Roe<std::unique_ptr<AmpIntegrationHarness>> MakeAmpIntegrationHarness(
    std::optional<pp::amp::PeerLinkConfig> link_config = std::nullopt);

} // namespace pbr::test
