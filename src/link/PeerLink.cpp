#include "amp/link/PeerLink.h"

#include "amp/link/CodedFailure.h"
#include "amp/L3/Types.h"
#include "amp/link/AmpAdpCarrier.h"
#include "amp/link/PeerLinkManager.h"
#include "amp/link/Types.h"
#include "amp/L2/SessionControl.h"

#include <functional>

namespace pp::amp {

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
    return Failure::Of(Err::TransportFailed,
                       detail::AppendFrom("amp link: transport failed", "adp", child.message));
  }
}

PeerLink::PeerLink(std::string peer_key, std::string remote_peer_id, const bool outbound,
                   std::shared_ptr<adp::Connection> connection, MshIdentity local_identity, PeerLinkManager& owner)
    : peer_key_(std::move(peer_key)), remote_peer_id_(std::move(remote_peer_id)), outbound_(outbound),
      connection_(std::move(connection)), identity_(std::move(local_identity)), owner_(owner) {
  connection_->OnMessage([this](const adp::Message& message) {
    HandleAdpPayload(message.payload);
  });
}

PeerLink::PeerLink(std::string peer_key, std::string remote_peer_id, const bool outbound,
                   std::shared_ptr<ChannelSession> carrier, MshIdentity local_identity, PeerLinkManager& owner)
    : peer_key_(std::move(peer_key)), remote_peer_id_(std::move(remote_peer_id)), outbound_(outbound),
      carrier_(std::move(carrier)), identity_(std::move(local_identity)), owner_(owner) {
  AttachCarrierFrameHandler();
}

PeerLink::~PeerLink() {
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
  handshake_started_ms_ = owner_.GetEndpoint().GetClock().NowMs();
  const bool chunked = !IsCarrierBacked();
  handshake_ = std::make_unique<MshAdpHandshake>(
      role, identity_,
      [this](std::vector<uint8_t> payload) {
        if (IsCarrierBacked()) {
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
    FailAssociationMessage(started.error(), Err::HandshakeFailed);
  }
}

void PeerLink::StartInboundHandshake(CompleteCb on_established) {
  StartHandshakeCommon(MshAdpHandshake::Role::Responder, std::move(on_established));
}

void PeerLink::HandleCarrierFrame(const std::span<const uint8_t> payload) {
  if (phase_ == PeerLinkPhase::Connected && mux_) {
    auto kind = AmpAdpCarrier::DecodeKind(payload);
    if (!kind || *kind != AmpAdpPayloadKind::Sealed) {
      return;
    }
    auto header = AmpAdpCarrier::DecodeSealedHeader(payload);
    if (!header) {
      return;
    }
    auto body = AmpAdpCarrier::DecodeSealedBody(payload);
    if (!body) {
      return;
    }
    (void)mux_->OnSealedInbound(header->first, header->second, *body);
    return;
  }
  if (!handshake_) {
    return;
  }
  auto kind = AmpAdpCarrier::DecodeKind(payload);
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

void PeerLink::HandleAdpPayload(const std::span<const uint8_t> payload) {
  if (phase_ == PeerLinkPhase::Connected && mux_) {
    auto kind = AmpAdpCarrier::DecodeKind(payload);
    if (!kind) {
      return;
    }
    if (*kind == AmpAdpPayloadKind::Sealed) {
      auto header = AmpAdpCarrier::DecodeSealedHeader(payload);
      if (!header) {
        return;
      }
      auto body = AmpAdpCarrier::DecodeSealedBody(payload);
      if (!body) {
        return;
      }
      (void)mux_->OnSealedInbound(header->first, header->second, *body);
    }
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

Roe<std::optional<std::vector<uint8_t>>> PeerLink::PushMshChunk(const MshMessageType type, const uint16_t index,
                                                                 const uint16_t count,
                                                                 const std::span<const uint8_t> chunk) {
  if (count == 0 || index >= count) {
    return Error("amp link: bad msh chunk meta");
  }
  if (msh_chunk_count_ == 0) {
    msh_chunk_type_ = type;
    msh_chunk_count_ = count;
    msh_chunk_parts_.assign(count, {});
  }
  if (type != msh_chunk_type_ || count != msh_chunk_count_) {
    return Error("amp link: msh chunk stream mismatch");
  }
  if (msh_chunk_parts_[index].empty()) {
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
  if (!remote_identity_public_key_.empty()) {
    if (auto derived = owner_.DeriveRemotePeerId(remote_identity_public_key_); !derived.empty()) {
      remote_peer_id_ = std::move(derived);
    } else if (remote_peer_id_.empty()) {
      remote_peer_id_ = IdentityPublicKeyFingerprint(remote_identity_public_key_);
    }
  }

  auto session = Session::FromMaterial(established.local_material, master_ikm_, transcript_hash_);
  if (!session) {
    FailAssociationMessage(session.error(), Err::HandshakeFailed);
    return;
  }

  if (connection_) {
    connection_->UpgradeBinder(session->AssocKey());
  }
  session_ = std::make_unique<Session>(std::move(*session));
  mux_ = std::make_unique<ChannelMux>(*session_);
  mux_->SetClock([this]() { return owner_.GetEndpoint().GetClock().NowMs(); });
  AttachMuxTransport();
  handshake_.reset();
  phase_ = PeerLinkPhase::Connected;
  if (!owner_.OnLinkEstablished(*this)) {
    // Dual-dial loser ([A026]): tear down this link after stack unwinds. If another Connected
    // Session to the same remote remains, association still succeeded for waiters.
    phase_ = PeerLinkPhase::Backoff;
    const std::string drop_key = peer_key_;
    const std::string remote = remote_peer_id_;
    const bool assoc_ok =
        !remote.empty() && owner_.FindAnyConnectedLinkForRemotePeerId(remote) != nullptr;
    owner_.ScheduleDropLink(drop_key);
    if (assoc_ok && outbound_) {
      owner_.ScheduleAdoptDialAlias(remote, drop_key);
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

void PeerLink::AttachMuxTransport() {
  mux_->SetTransport([this](const uint32_t channel_id, const uint32_t channel_seq, const adp::QosClass qos,
                            std::vector<uint8_t> sealed) {
    auto wire = AmpAdpCarrier::EncodeSealed(channel_id, channel_seq, sealed);
    if (!wire) {
      return;
    }
    if (IsCarrierBacked()) {
      (void)SendCarrierWire(std::move(*wire));
      return;
    }
    (void)SendAdp(std::move(*wire), qos);
  });
}

void PeerLink::MarkWarm() { warm_ = true; }

void PeerLink::ClearWarm() { warm_ = false; }

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
  const int64_t now_ms = owner_.GetEndpoint().GetClock().NowMs();
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
