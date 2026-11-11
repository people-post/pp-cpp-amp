#include "amp/link/PeerLink.h"

#include "amp/link/CodedFailure.h"
#include "amp/L3/Types.h"
#include "amp/link/AmpAdpCarrier.h"
#include "amp/link/Types.h"
#include "amp/L2/SessionControl.h"

#include <algorithm>
#include <functional>
#include <limits>

namespace pp::amp {

namespace {
/** Cap on lane_backlog_: frames the CarrierLane delivered before the mux exists to drain them. */
constexpr size_t kMaxLaneBacklogFrames = 256;
} // namespace

PeerLink::Failure PeerLink::WrapConnectionFailure(const adp::Connection::Failure& child) {
  switch (child.GetCode()) {
    case adp::Connection::Err::Closed:
    case adp::Connection::Err::WindowFull:
      return Failure::Of(Err::TransportUnavailable,
                         detail::AppendFrom("amp link: transport unavailable", "adp", child.message));
    case adp::Connection::Err::PayloadTooLarge:
    case adp::Connection::Err::SeqWrap:
    case adp::Connection::Err::WireError:
    default:
      return Failure::Of(Err::TransportFailed, detail::AppendFrom("amp link: transport failed", "adp", child.message));
  }
}

PeerLink::PeerLink(std::string peer_key, std::string remote_peer_id, const bool outbound,
                   std::shared_ptr<adp::Connection> connection, MshIdentity local_identity, PeerLinkHostPorts host)
    : peer_key_(std::move(peer_key)),
      remote_peer_id_(std::move(remote_peer_id)),
      outbound_(outbound),
      connection_(std::move(connection)),
      identity_(std::move(local_identity)),
      host_(std::move(host)) {
  connection_->OnMessage([this](const adp::Message& message) {
    if (message.sealed) {
      DeliverFrame(message.payload);
      return;
    }
    HandleAdpPayload(message.payload);
  });
  connection_->OnReliableAcked([this]() {
    if (mux_) {
      mux_->OnTransportCredit(); // queued frames go at once, not at the next housekeeping walk
    }
  });
  // A Connected link whose connection closed is gone at once, not at the next housekeeping walk: a dial in between
  // would take the dead link ("send on closed connection") instead of a new one. A handshake's close is a refusal
  // its dialer hears through the sweep.
  connection_->OnClosed([this]() {
    if (phase_ == PeerLinkPhase::Connected && host_.schedule_drop_closed) {
      host_.schedule_drop_closed(Handle());
    }
  });
}

PeerLink::PeerLink(std::string peer_key, std::string remote_peer_id, const bool outbound,
                   std::shared_ptr<ChannelSession> carrier, MshIdentity local_identity, PeerLinkHostPorts host)
    : peer_key_(std::move(peer_key)),
      remote_peer_id_(std::move(remote_peer_id)),
      outbound_(outbound),
      carrier_(std::move(carrier)),
      lane_(std::make_unique<CarrierLane>()),
      identity_(std::move(local_identity)),
      host_(std::move(host)) {
  AttachCarrierFrameHandler();
}

PeerLink::~PeerLink() {
  // Drop the Connection message thunk before releasing shared_ptr — Endpoint may still
  // hold the Connection briefly after Unregister; a late Pump must not call into `this`.
  if (connection_) {
    connection_->OnMessage({});
    connection_->OnReliableAcked({});
    connection_->OnClosed({});
  }
  // Carrier is Bound to an outer Mux ([A024]). Orphan without touching mux_ — map destroy
  // order may have already freed that Mux (Windows SEH / SIGFPE on dead unordered_map).
  if (carrier_) {
    carrier_->OrphanFromMux();
    carrier_.reset();
  }
}

void PeerLink::AttachCarrierFrameHandler() {
  if (!carrier_) {
    return;
  }
  carrier_->SetFrameHandler([this](Roe<std::vector<uint8_t>> frame) {
    if (!frame) {
      FailAssociationMessage(frame.error(), Err::CarrierClosed);
      return false;
    }
    HandleCarrierFrame(*frame);
    return true;
  });
  carrier_->SetClosedCallback([this](const char* reason) {
    if (phase_ == PeerLinkPhase::Connected) {
      phase_ = PeerLinkPhase::Backoff;
      return;
    }
    FailAssociation(Failure::Of(Err::CarrierClosed, reason && reason[0] ? reason : "amp carrier closed"));
  });
}

void PeerLink::StartHandshakeCommon(const MshAdpHandshake::Role role, CompleteCb on_established) {
  establish_cb_ = std::move(on_established);
  phase_ = PeerLinkPhase::Handshaking;
  handshake_started_ms_ = host_.now_ms ? host_.now_ms() : 0;
  const bool chunked = !IsCarrierBacked();
  handshake_ = std::make_unique<MshAdpHandshake>(
      role, identity_,
      [this](std::vector<uint8_t> payload) {
        if (IsCarrierBacked()) {
          // Every handshake message carries a lane probe, so both ends know at Connected.
          SendLaneProbe();
          if (outbound_) {
            carrier_msh_resend_interval_ms_ = kCarrierHandshakeResendMs;
            carrier_msh_resend_at_ms_ = NowMs() + carrier_msh_resend_interval_ms_;
          }
          auto sent = SendCarrierWire(std::move(payload));
          if (!sent) {
            return Roe<void>(Error(sent.error().message));
          }
          return Roe<void>();
        }
        auto sent = SendAdpLink(std::move(payload), adp::QosClass::Reliable);
        if (!sent) {
          return Roe<void>(Error(sent.error().message));
        }
        return Roe<void>();
      },
      [this](Roe<MshAdpEstablished> established) { OnHandshakeComplete(std::move(established)); }, chunked);
}

void PeerLink::StartOutboundHandshake(CompleteCb on_established) {
  StartHandshakeCommon(MshAdpHandshake::Role::Initiator, std::move(on_established));
  if (auto started = handshake_->Start(); !started) {
    // MshAdpHandshake::Fail may already have invoked on_complete_ → FailAssociation.
    if (establish_cb_) {
      FailAssociationMessage(started.error(), Err::HandshakeFailed);
    }
  }
}

void PeerLink::StartInboundHandshake(CompleteCb on_established) {
  StartHandshakeCommon(MshAdpHandshake::Role::Responder, std::move(on_established));
}

void PeerLink::HandleCarrierFrame(const std::span<const uint8_t> payload) {
  auto lane_kind = AmpAdpCarrier::DecodeKind(payload);
  if (lane_kind && (*lane_kind == AmpAdpPayloadKind::LaneData || *lane_kind == AmpAdpPayloadKind::LaneAck)) {
    HandleLaneFrame(*lane_kind, payload);
    return;
  }
  if (phase_ == PeerLinkPhase::Connected && mux_) {
    auto kind = AmpAdpCarrier::DecodeKind(payload);
    if (kind && *kind == AmpAdpPayloadKind::Msh) {
      // The initiator resent its Finished: ours was lost.
      if (const auto msh = AmpAdpCarrier::DecodeMshType(payload);
          msh && *msh == MshMessageType::Finished && !carrier_final_msh_.empty()) {
        (void)SendCarrierWire(carrier_final_msh_);
      }
      return;
    }
    if (!kind || (*kind != AmpAdpPayloadKind::Sealed && *kind != AmpAdpPayloadKind::SealedReliable)) {
      return;
    }
    DeliverSealedCarrierWire(payload);
    return;
  }
  if (!handshake_) {
    return;
  }
  auto kind = AmpAdpCarrier::DecodeKind(payload);
  if (kind && *kind == AmpAdpPayloadKind::MshChunk) {
    HandleCarrierMshChunk(payload); // a resend, in pieces
    return;
  }
  if (!kind || *kind != AmpAdpPayloadKind::Msh) {
    return;
  }
  auto msh_type = AmpAdpCarrier::DecodeMshType(payload);
  if (!msh_type) {
    return;
  }
  auto body = AmpAdpCarrier::DecodeMshBody(payload);
  if (!body) {
    return;
  }
  (void)handshake_->HandleMsh(*msh_type, *body);
}

void PeerLink::HandleCarrierMshChunk(const std::span<const uint8_t> payload) {
  auto chunk = AmpAdpCarrier::DecodeMshChunk(payload);
  if (!chunk) {
    return;
  }
  const auto [type, index, count, piece] = *chunk;
  // Pieces of another message than the one assembling: that one was superseded (the peer moved on, or a resend
  // of the one before came late). Start again; never fail the handshake on a lossy carrier's leftovers.
  if (msh_chunk_count_ != 0 && (type != msh_chunk_type_ || count != msh_chunk_count_)) {
    msh_chunk_count_ = 0;
    msh_chunk_parts_.clear();
    msh_chunk_bytes_ = 0;
  }
  auto assembled = PushMshChunk(type, index, count, piece);
  if (!assembled || !assembled->has_value()) {
    return;
  }
  auto msh_type = AmpAdpCarrier::DecodeMshType(assembled->value());
  auto body = AmpAdpCarrier::DecodeMshBody(assembled->value());
  if (msh_type && body) {
    (void)handshake_->HandleMsh(*msh_type, *body);
  }
}

void PeerLink::HandleAdpPayload(const std::span<const uint8_t> payload) {
  // Plain (HMAC'd) payloads are the handshake's: once Connected, data comes sealed (DeliverFrame).
  if (phase_ == PeerLinkPhase::Connected && mux_) {
    return;
  }

  if (!handshake_) {
    return;
  }
  auto kind = AmpAdpCarrier::DecodeKind(payload);
  if (!kind) {
    return;
  }
  if (*kind == AmpAdpPayloadKind::MshChunk) {
    auto chunk = AmpAdpCarrier::DecodeMshChunk(payload);
    if (!chunk) {
      return;
    }
    auto assembled = PushMshChunk(std::get<0>(*chunk), std::get<1>(*chunk), std::get<2>(*chunk), std::get<3>(*chunk));
    if (!assembled) {
      FailAssociationMessage(assembled.error(), Err::HandshakeFailed);
      return;
    }
    if (!assembled->has_value()) {
      return;
    }
    auto msh_type = AmpAdpCarrier::DecodeMshType(assembled->value());
    if (!msh_type) {
      return;
    }
    auto body = AmpAdpCarrier::DecodeMshBody(assembled->value());
    if (!body) {
      return;
    }
    (void)handshake_->HandleMsh(*msh_type, *body);
    return;
  }
  if (*kind != AmpAdpPayloadKind::Msh) {
    return;
  }
  auto msh_type = AmpAdpCarrier::DecodeMshType(payload);
  if (!msh_type) {
    return;
  }
  auto body = AmpAdpCarrier::DecodeMshBody(payload);
  if (!body) {
    return;
  }
  (void)handshake_->HandleMsh(*msh_type, *body);
}

void PeerLink::HandleLaneFrame(const AmpAdpPayloadKind kind, const std::span<const uint8_t> payload) {
  if (!lane_) {
    return;
  }
  lane_->NotePeerSpeaksLane();
  if (kind == AmpAdpPayloadKind::LaneAck) {
    if (auto ack = AmpAdpCarrier::DecodeLaneAck(payload)) {
      lane_->OnAck(*ack, NowMs());
      if (mux_) {
        mux_->OnTransportCredit(); // the lane's window moved
      }
    }
    return;
  }
  auto data = AmpAdpCarrier::DecodeLaneData(payload);
  if (!data) {
    return;
  }
  // Taken in while still handshaking too: the peer may finish first and send at once. The ack
  // goes back either way so its sender does not wait out an RTO.
  auto received = lane_->OnData(data->first, data->second);
  (void)SendCarrierWire(AmpAdpCarrier::EncodeLaneAck(received.ack));
  for (auto& inner : received.deliver) {
    // Bounded independent of the mux existing yet: the peer may still be handshaking, and
    // nothing drains this backlog before Connected (FlushLaneBacklog no-ops until then).
    if (lane_backlog_.size() >= kMaxLaneBacklogFrames) {
      break;
    }
    lane_backlog_.push_back(std::move(inner));
  }
  FlushLaneBacklog();
}

void PeerLink::FlushLaneBacklog() {
  if (phase_ != PeerLinkPhase::Connected || !mux_) {
    return;
  }
  auto backlog = std::move(lane_backlog_);
  lane_backlog_.clear();
  for (const auto& inner : backlog) {
    DeliverSealedCarrierWire(inner);
  }
}

void PeerLink::DeliverSealedCarrierWire(const std::span<const uint8_t> wire) {
  if (!session_) {
    return;
  }
  auto header = AmpAdpCarrier::DecodeSealedHeader(wire);
  if (!header) {
    return;
  }
  auto& window = header->reliable ? carrier_rx_rel_ : carrier_rx_be_;
  if (window.Classify(header->number) != adp::ReplayWindow::Verdict::Fresh) {
    return; // a replay (or a lane resend we already took)
  }
  const auto space = header->reliable ? adp::PacketSpace::CarrierReliable : adp::PacketSpace::CarrierBestEffort;
  auto opened = session_->OpenPacket(space, header->number, wire.first(kCarrierSealedHeaderBytes),
                                     wire.subspan(kCarrierSealedHeaderBytes), NowMs());
  if (!opened) {
    return;
  }
  // Recorded only once it opened: a forged number must not shut out the real packet.
  (void)window.Accept(header->number);
  carrier_final_msh_.clear(); // the initiator has our Finished
  DeliverFrame(*opened);
}

void PeerLink::DeliverFrame(const std::span<const uint8_t> frame) {
  if (phase_ != PeerLinkPhase::Connected || !mux_) {
    return;
  }
  (void)mux_->OnFrameInbound(frame);
}

size_t PeerLink::CarrierMaxFrameBytes() const {
  // An inner packet must ride one DATA frame of the outer link whole (the hop's bridge forwards it as it is): a
  // frame larger than this would be fragmented twice, once by each mux.
  const size_t outer =
      carrier_ && carrier_->Mux() ? carrier_->Mux()->MaxFrameBytes() : ChannelMux::kDirectMaxFrameBytes;
  const size_t wrapping =
      kChannelHeaderBytes + kCarrierLaneHeaderBytes + kCarrierSealedHeaderBytes + adp::kPacketTagBytes;
  return outer > wrapping ? outer - wrapping : 0; // 0: no budget (SetMaxFrameBytes refuses it)
}

Roe<std::vector<uint8_t>> PeerLink::SealCarrierFrame(const adp::QosClass qos, const std::span<const uint8_t> frame) {
  if (!session_) {
    return Error("amp link: no session to seal with");
  }
  const bool reliable = qos == adp::QosClass::Reliable;
  uint32_t& counter = reliable ? carrier_tx_rel_ : carrier_tx_be_;
  if (counter == 0xffffffffu) {
    return Error("amp link: carrier packet numbers ran out");
  }
  const uint32_t number = ++counter;
  const auto header = AmpAdpCarrier::EncodeSealedHeader(reliable, number);
  std::vector<uint8_t> out;
  out.reserve(header.size() + frame.size() + adp::kPacketTagBytes);
  out.assign(header.begin(), header.end());
  const auto space = reliable ? adp::PacketSpace::CarrierReliable : adp::PacketSpace::CarrierBestEffort;
  if (auto sealed = session_->SealPacket(space, number, header, frame, out); !sealed) {
    return sealed.error();
  }
  return out;
}

void PeerLink::SendLaneProbe() {
  if (lane_) {
    (void)SendCarrierWire(AmpAdpCarrier::EncodeLaneAck({}));
  }
}

void PeerLink::TickCarrierHandshake(const int64_t now_ms) {
  if (!IsCarrierBacked() || !outbound_ || phase_ != PeerLinkPhase::Handshaking || !handshake_ ||
      carrier_msh_resend_at_ms_ == 0 || now_ms < carrier_msh_resend_at_ms_) {
    return;
  }
  carrier_msh_resend_interval_ms_ = std::min(carrier_msh_resend_interval_ms_ * 2, kCarrierHandshakeResendMaxMs);
  carrier_msh_resend_at_ms_ = now_ms + carrier_msh_resend_interval_ms_;
  SendLaneProbe();
  if (auto chunks = handshake_->LastChunks()) {
    for (auto& chunk : *chunks) {
      (void)SendCarrierWire(std::move(chunk));
    }
  }
}

void PeerLink::TickCarrierLane(const int64_t now_ms) {
  if (!lane_ || phase_ != PeerLinkPhase::Connected) {
    return;
  }
  for (auto& wire : lane_->DueResends(now_ms)) {
    (void)SendCarrierWire(std::move(wire));
  }
}

Roe<std::optional<std::vector<uint8_t>>> PeerLink::PushMshChunk(const MshMessageType type, const uint16_t index,
                                                                const uint16_t count,
                                                                const std::span<const uint8_t> chunk) {
  if (count == 0 || index >= count) {
    return Error("amp link: bad msh chunk meta");
  }
  // A peer-declared count backs a msh_chunk_parts_.assign(count, {}) below: bound it to what the
  // largest real MSH message could ever need, not the full uint16_t range.
  if (count > kMaxMshChunkCount) {
    return Error("amp link: msh chunk count exceeds limit");
  }
  if (msh_chunk_count_ == 0) {
    msh_chunk_type_ = type;
    msh_chunk_count_ = count;
    msh_chunk_parts_.assign(count, {});
    msh_chunk_bytes_ = 0;
  }
  if (type != msh_chunk_type_ || count != msh_chunk_count_) {
    return Error("amp link: msh chunk stream mismatch");
  }
  if (msh_chunk_parts_[index].empty()) {
    if (msh_chunk_bytes_ + chunk.size() > kMaxMshMessageBytes) {
      return Error("amp link: msh chunk stream too large");
    }
    msh_chunk_bytes_ += chunk.size();
    msh_chunk_parts_[index].assign(chunk.begin(), chunk.end());
  }
  for (const auto& part : msh_chunk_parts_) {
    if (part.empty()) {
      return std::optional<std::vector<uint8_t>>{};
    }
  }
  std::vector<uint8_t> body;
  for (const auto& part : msh_chunk_parts_) {
    body.insert(body.end(), part.begin(), part.end());
  }
  msh_chunk_count_ = 0;
  msh_chunk_parts_.clear();
  msh_chunk_bytes_ = 0;
  auto wire = AmpAdpCarrier::EncodeMsh(msh_chunk_type_, body);
  if (!wire) {
    return wire.error();
  }
  return std::optional<std::vector<uint8_t>>{std::move(*wire)};
}

PeerLink::LinkRoe PeerLink::SendAdpLink(std::vector<uint8_t> payload, const adp::QosClass qos) {
  if (!connection_) {
    return LinkRoe::error(Failure::Of(Err::NoConnection, "amp link: no connection"));
  }
  auto sent = connection_->Send(qos, payload);
  if (!sent) {
    return LinkRoe::error(WrapConnectionFailure(sent.error()));
  }
  return LinkRoe();
}

Roe<void> PeerLink::SendAdp(std::vector<uint8_t> payload, const adp::QosClass qos) {
  auto sent = SendAdpLink(std::move(payload), qos);
  if (!sent) {
    return Error(sent.error().message);
  }
  return Roe<void>();
}

PeerLink::LinkRoe PeerLink::SendCarrierWire(std::vector<uint8_t> payload) {
  if (!carrier_ || carrier_->IsClosed()) {
    return LinkRoe::error(Failure::Of(Err::CarrierClosed, "amp link: carrier closed"));
  }
  if (!carrier_->EnqueueOutbound(std::move(payload))) {
    return LinkRoe::error(Failure::Of(Err::CarrierEnqueueFailed, "amp link: carrier enqueue failed"));
  }
  return LinkRoe();
}

void PeerLink::OnHandshakeComplete(Roe<MshAdpEstablished> established) {
  if (!established) {
    FailAssociationMessage(established.error(), Err::HandshakeFailed);
    return;
  }
  FinishEstablishment(std::move(*established));
}

void PeerLink::FinishEstablishment(MshAdpEstablished established) {
  master_ikm_ = std::move(established.master_ikm);
  transcript_hash_ = std::move(established.transcript_hash);
  remote_identity_public_key_ = std::move(established.remote_identity_public_key);
  // Outbound ADP dials to a known PeerId carry it as remote_peer_id_ from construction (inbound
  // accepts start it empty). Snapshot it before the handshake result overwrites the field, so a
  // handshake that authenticates as a *different* identity can be rejected below instead of
  // silently rebinding the dial to whoever answered. Carrier-backed (nested) links key by a
  // caller-chosen join key that is not necessarily a pre-known PeerId (FinishNestedCarrier
  // handles their identity/alias policy separately) — do not apply that check here.
  const std::string expected_peer_id = (outbound_ && !carrier_) ? remote_peer_id_ : std::string{};
  if (!remote_identity_public_key_.empty()) {
    if (host_.derive_peer_id) {
      if (auto derived = host_.derive_peer_id(remote_identity_public_key_); !derived.empty()) {
        remote_peer_id_ = std::move(derived);
      } else if (remote_peer_id_.empty()) {
        remote_peer_id_ = IdentityPublicKeyFingerprint(remote_identity_public_key_);
      }
    } else if (remote_peer_id_.empty()) {
      remote_peer_id_ = IdentityPublicKeyFingerprint(remote_identity_public_key_);
    }
  }
  if (!expected_peer_id.empty() && remote_peer_id_ != expected_peer_id) {
    FailAssociationMessage(Error("amp link: authenticated peer id does not match dial target"), Err::HandshakeFailed);
    return;
  }

  auto session = Session::FromMaterial(established.local_material, master_ikm_, transcript_hash_);
  if (!session) {
    FailAssociationMessage(session.error(), Err::HandshakeFailed);
    return;
  }

  if (connection_) {
    connection_->UpgradeBinder(session->AssocKey());
  }
  session_ = std::make_shared<Session>(std::move(*session));
  mux_ = std::make_unique<ChannelMux>(session_->Material().initiator);
  if (IsCarrierBacked()) {
    if (auto budget = mux_->SetMaxFrameBytes(CarrierMaxFrameBytes()); !budget) {
      FailAssociationMessage(budget.error(), Err::HandshakeFailed); // the carrier's frames are too small to nest in
      return;
    }
  }
  if (connection_) {
    connection_->SetProtection(session_); // data goes sealed from here (A029 H1)
  }
  mux_->SetClock([this]() { return host_.now_ms ? host_.now_ms() : 0; });
  AttachMuxTransport();
  if (IsCarrierBacked() && !outbound_ && handshake_) {
    carrier_final_msh_ = handshake_->LastWire();
  }
  handshake_.reset();
  phase_ = PeerLinkPhase::Connected;
  if (!host_.on_established || !host_.on_established(*this)) {
    // Dual-dial loser ([A026]): tear down this link after stack unwinds. If another Connected
    // Session to the same remote remains, association still succeeded for waiters.
    phase_ = PeerLinkPhase::Backoff;
    const std::string drop_key = peer_key_; // dial alias for the survivor
    const std::string remote = remote_peer_id_;
    const bool assoc_ok = !remote.empty() && host_.has_other_connected && host_.has_other_connected(remote);
    if (host_.schedule_drop) {
      host_.schedule_drop(Handle());
    }
    if (assoc_ok && outbound_ && host_.schedule_adopt_alias) {
      host_.schedule_adopt_alias(remote, drop_key);
    }
    if (establish_cb_) {
      if (assoc_ok) {
        establish_cb_(LinkRoe());
      } else {
        establish_cb_(LinkRoe::error(Failure::Of(Err::DualDialLost, "amp link: dual-dial election lost")));
      }
      establish_cb_ = nullptr;
    }
    return;
  }

  if (establish_cb_) {
    establish_cb_(LinkRoe());
    establish_cb_ = nullptr;
  }
  if (lane_) {
    SendLaneProbe(); // a peer that lost every handshake-time probe still learns of the lane
    FlushLaneBacklog();
  }
}

void PeerLink::FailAssociation(const Failure& failure) {
  phase_ = PeerLinkPhase::Backoff;
  handshake_.reset();
  if (establish_cb_) {
    establish_cb_(LinkRoe::error(failure));
    establish_cb_ = nullptr;
  }
}

void PeerLink::FailAssociationMessage(const Error& error, const Err code) {
  FailAssociation(Failure::Of(code, error.message));
}

void PeerLink::FailHandshakeTimeout() {
  FailAssociation(Failure::Of(Err::DialTimeout, "amp link: dial timeout"));
}

void PeerLink::FailRefused() {
  FailAssociation(Failure::Of(Err::TransportFailed, "amp link: refused by the peer (its link table is full)"));
}

void PeerLink::FailVersionMismatch(const std::vector<uint8_t>& versions) {
  std::string spoken;
  for (const uint8_t v : versions) {
    spoken += (spoken.empty() ? "v" : ", v") + std::to_string(v);
  }
  FailAssociation(Failure::Of(Err::VersionMismatch, "amp link: the peer speaks wire " + spoken + ", not v" +
                                                        std::to_string(adp::kWireVersion)));
}

void PeerLink::DemoteForScheduledDrop() {
  if (phase_ == PeerLinkPhase::Connected || phase_ == PeerLinkPhase::Handshaking || phase_ == PeerLinkPhase::Dialing) {
    phase_ = PeerLinkPhase::Backoff;
  }
  if (mux_) {
    mux_->ClearProtocolHandlers();
  }
}

void PeerLink::AttachMuxTransport() {
  mux_->SetTransportCredits([this]() -> size_t {
    if (CarrierLaneActive()) {
      return lane_->Credits();
    }
    if (IsCarrierBacked() || !connection_) {
      return std::numeric_limits<size_t>::max();
    }
    return connection_->ReliableCreditsRemaining();
  });
  mux_->SetTransport([this](const adp::QosClass qos, std::vector<uint8_t> frame) -> Roe<void> {
    if (IsCarrierBacked()) {
      auto wire = SealCarrierFrame(qos, frame);
      if (!wire) {
        return wire.error();
      }
      // Reliable-class frames ride the lane when the peer speaks it; best-effort (media) as is.
      if (qos == adp::QosClass::Reliable && CarrierLaneActive()) {
        auto laned = lane_->Wrap(*wire, NowMs());
        if (!laned) {
          return Error("amp link: carrier lane window full");
        }
        wire = std::move(*laned);
      }
      auto sent = SendCarrierWire(std::move(*wire));
      if (!sent) {
        return Error(sent.error().message);
      }
      return Roe<void>();
    }
    return SendAdp(std::move(frame), qos); // the connection seals it
  });
}

KeepaliveTier PeerLink::GetKeepaliveTier() const {
  return std::max(
      {keepalive_tier_, carried_keepalive_tier_, mux_ ? mux_->ChannelsKeepaliveTier() : KeepaliveTier::None});
}

void PeerLink::StopKeepaliveIfUnheld() {
  if (!connection_ || IsCarrierBacked() || connection_->LocalKeepaliveIntervalMs() == 0 ||
      GetKeepaliveTier() != KeepaliveTier::None) {
    return;
  }
  last_keepalive_tx_ms_ = 0;
  (void)connection_->StopKeepalive(host_.now_ms ? host_.now_ms() : 0);
}

void PeerLink::MarkWarm() {
  keepalive_tier_ = KeepaliveTier::Warm;
}

void PeerLink::MarkHot() {
  keepalive_tier_ = KeepaliveTier::Hot;
}

void PeerLink::ClearWarm() {
  keepalive_tier_ = KeepaliveTier::None;
  // Back to the cold liveness window on both sides — unless a channel open on the link still keeps it up
  // (a call's path on a link chat had warmed): the mark is cleared, the channel's tier stays.
  StopKeepaliveIfUnheld();
}

PeerLink::LinkRoe PeerLink::SendKeepalive(const int64_t now_ms, const uint32_t interval_ms) {
  if (IsCarrierBacked() || !connection_) {
    return LinkRoe::error(Failure::Of(Err::TransportUnavailable, "amp link: keepalive unavailable"));
  }
  auto sent = connection_->SendKeepalive(now_ms, interval_ms);
  if (!sent) {
    return LinkRoe::error(WrapConnectionFailure(sent.error()));
  }
  last_keepalive_tx_ms_ = now_ms;
  return LinkRoe();
}

PeerLink::LinkRoe PeerLink::SendProbe(const int64_t now_ms) {
  if (IsCarrierBacked() || !connection_) {
    return LinkRoe::error(Failure::Of(Err::TransportUnavailable, "amp link: probe unavailable"));
  }
  last_probe_ms_ = now_ms;
  auto sent = connection_->SendProbe(now_ms);
  if (!sent) {
    return LinkRoe::error(WrapConnectionFailure(sent.error()));
  }
  return LinkRoe();
}

void PeerLink::RequestSessionRekey(std::function<void(Roe<void>)> on_complete) {
  if (phase_ != PeerLinkPhase::Connected || !mux_ || !session_) {
    if (on_complete) {
      on_complete(Error("amp link: not connected"));
    }
    return;
  }
  if (rekey_cb_) {
    if (on_complete) {
      on_complete(Error("amp link: rekey already in flight"));
    }
    return;
  }
  rekey_cb_ = std::move(on_complete);
  SessionRekeyMessage msg;
  msg.kind = SessionControlKind::RekeyRequest;
  msg.target_epoch = session_->Material().session_epoch + 1;
  auto encoded = SessionControlCodec::Encode(msg);
  if (!encoded) {
    rekey_cb_ = nullptr;
    if (on_complete) {
      on_complete(encoded.error());
    }
    return;
  }
  if (mux_->State(kCapabilityChannelId) != ChannelState::Open) {
    rekey_cb_ = nullptr;
    if (on_complete) {
      on_complete(Error("amp link: ch0 not open"));
    }
    return;
  }
  const auto sent = mux_->SendData(kCapabilityChannelId, std::move(*encoded));
  if (!sent) {
    rekey_cb_ = nullptr;
    if (on_complete) {
      on_complete(sent.error());
    }
  }
}

void PeerLink::HandleSessionControl(const std::span<const uint8_t> payload) {
  if (phase_ != PeerLinkPhase::Connected || !mux_ || !session_) {
    return;
  }
  auto decoded = SessionControlCodec::Decode(payload);
  if (!decoded) {
    return;
  }
  const int64_t now_ms = host_.now_ms ? host_.now_ms() : 0;
  const uint32_t expected = session_->Material().session_epoch + 1;

  if (decoded->kind == SessionControlKind::RekeyRequest) {
    if (decoded->target_epoch != expected) {
      return;
    }
    SessionRekeyMessage ack;
    ack.kind = SessionControlKind::RekeyAck;
    ack.target_epoch = decoded->target_epoch;
    if (auto encoded = SessionControlCodec::Encode(ack)) {
      (void)mux_->SendData(kCapabilityChannelId, std::move(*encoded));
    }
    (void)session_->ApplyRekey(decoded->target_epoch, now_ms);
    return;
  }

  if (decoded->kind == SessionControlKind::RekeyAck) {
    if (decoded->target_epoch != expected) {
      return;
    }
    if (auto applied = session_->ApplyRekey(decoded->target_epoch, now_ms); !applied) {
      if (rekey_cb_) {
        rekey_cb_(applied.error());
        rekey_cb_ = nullptr;
      }
      return;
    }
    if (rekey_cb_) {
      rekey_cb_(Roe<void>());
      rekey_cb_ = nullptr;
    }
  }
}

} // namespace pp::amp
