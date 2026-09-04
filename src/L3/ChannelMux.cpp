#include "amp/L3/ChannelMux.h"

#include "amp/L3/AmpChannelLimits.h"
#include "amp/L3/Capability.h"

namespace pp::amp {

namespace {

inline constexpr size_t kMaxSingleDataBytes = 900;

int64_t DefaultNowMs() { return 0; }

} // namespace

ChannelMux::ChannelMux(Session& session) : session_(session), now_ms_(DefaultNowMs) {
  // Dual-open on one Session: initiator allocates odd ids, responder even — avoids OPEN glare.
  next_dynamic_id_ = session.Material().initiator ? 1u : 2u;
}

void ChannelMux::SetPeerSession(Session* peer_session) { peer_session_ = peer_session; }

void ChannelMux::SetTransport(TransportSend send) { transport_ = std::move(send); }

void ChannelMux::SetTransportCredits(TransportCredits credits) { transport_credits_ = std::move(credits); }

void ChannelMux::SetClock(std::function<int64_t()> now_ms) { now_ms_ = std::move(now_ms); }

ChannelMux::ChannelRecord* ChannelMux::ChannelById(const uint32_t channel_id) {
  auto it = channels_.find(channel_id);
  if (it == channels_.end()) {
    return nullptr;
  }
  return &it->second;
}

const ChannelMux::ChannelRecord* ChannelMux::ChannelById(const uint32_t channel_id) const {
  auto it = channels_.find(channel_id);
  if (it == channels_.end()) {
    return nullptr;
  }
  return &it->second;
}

Roe<void> ChannelMux::SendFrame(const ChannelFrame& frame, ChannelRecord& channel) {
  if (!transport_) {
    return Error("amp mux: no transport");
  }
  auto wire = ChannelWire::Encode(frame);
  if (!wire) {
    return wire.error();
  }
  last_send_qos_ = QosForClass(channel.policy.cls);
  auto sealed = session_.Seal(frame.header.channel_id, frame.header.channel_seq, *wire);
  if (!sealed) {
    return sealed.error();
  }
  return transport_(frame.header.channel_id, frame.header.channel_seq, last_send_qos_, std::move(*sealed));
}

Roe<uint32_t> ChannelMux::OpenOutbound(const std::string& protocol_id, ChannelPolicy policy,
                                       std::optional<uint32_t> fixed_id) {
  uint32_t id = 0;
  if (fixed_id.has_value()) {
    id = *fixed_id;
    if (channels_.contains(id)) {
      return Error("amp mux: channel id in use");
    }
  } else {
    id = next_dynamic_id_;
    next_dynamic_id_ += 2;
    if (id == kIllegalChannelId) {
      id = next_dynamic_id_;
      next_dynamic_id_ += 2;
    }
  }
  ChannelRecord rec;
  rec.id = id;
  rec.protocol_id = protocol_id;
  rec.policy = std::move(policy);
  rec.state = ChannelState::Opening;
  rec.reassembly = MessageReassembly(rec.policy.max_message_bytes);
  channels_.emplace(id, std::move(rec));
  if (auto it = pending_handlers_.find(id); it != pending_handlers_.end()) {
    channels_.at(id).on_data = std::move(it->second);
    pending_handlers_.erase(it);
  }
  if (auto it = pending_terminal_handlers_.find(id); it != pending_terminal_handlers_.end()) {
    channels_.at(id).on_terminal = std::move(it->second);
    pending_terminal_handlers_.erase(it);
  }

  ChannelFrame frame;
  frame.header.frame_type = ChannelFrameType::Open;
  frame.header.channel_id = id;
  frame.header.channel_seq = 0;
  frame.open.protocol_id = protocol_id;
  frame.open.channel_class = channels_.at(id).policy.cls;
  const size_t max_bytes = channels_.at(id).policy.max_message_bytes;
  frame.open.max_message_bytes =
      max_bytes > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<uint32_t>(max_bytes);

  auto* channel = ChannelById(id);
  if (!channel) {
    return Error("amp mux: channel missing after insert");
  }
  auto sent = SendFrame(frame, *channel);
  if (!sent) {
    return sent.error();
  }
  return id;
}

void ChannelMux::SetDataHandler(const uint32_t channel_id, DataHandler handler) {
  if (auto* channel = ChannelById(channel_id)) {
    channel->on_data = std::move(handler);
    return;
  }
  pending_handlers_[channel_id] = std::move(handler);
}

Roe<void> ChannelMux::ApplyChannelPolicy(const uint32_t channel_id, ChannelPolicy policy) {
  auto* channel = ChannelById(channel_id);
  if (!channel) {
    return Error("amp mux: apply policy on unknown channel");
  }
  if (policy.max_message_bytes > AmpChannelLimits::kMaxChatBlobFrameBytes) {
    policy.max_message_bytes = AmpChannelLimits::kMaxChatBlobFrameBytes;
  }
  channel->policy = std::move(policy);
  channel->reassembly = MessageReassembly(channel->policy.max_message_bytes);
  return Roe<void>();
}

void ChannelMux::SetTerminalHandler(const uint32_t channel_id, TerminalHandler handler) {
  if (auto* channel = ChannelById(channel_id)) {
    channel->on_terminal = std::move(handler);
    return;
  }
  pending_terminal_handlers_[channel_id] = std::move(handler);
}

void ChannelMux::NotifyTerminal(ChannelRecord& channel, const char* reason) {
  // Copy before invoke ([A027]): parent may unbind mid-callback.
  if (channel.on_terminal) {
    auto handler = channel.on_terminal;
    handler(channel.id, reason);
  }
}

void ChannelMux::SetProtocolHandler(const std::string& protocol_id, InboundOpenHandler handler) {
  if (handler) {
    protocol_handlers_[protocol_id] = std::move(handler);
  } else {
    protocol_handlers_.erase(protocol_id);
  }
}

void ChannelMux::ClearProtocolHandlers() { protocol_handlers_.clear(); }

Roe<void> ChannelMux::DeliverPayload(ChannelRecord& channel, std::vector<uint8_t> payload) {
  // Copy before invoke ([A027]): parent may unbind / TearDown mid-callback.
  if (channel.on_data) {
    auto handler = channel.on_data;
    handler(channel.id, std::move(payload));
    return Roe<void>();
  }
  return Roe<void>();
}

Roe<void> ChannelMux::HandleOpen(ChannelFrame frame) {
  if (channels_.contains(frame.header.channel_id)) {
    return Error("amp mux: duplicate open");
  }
  ChannelRecord rec;
  rec.id = frame.header.channel_id;
  rec.protocol_id = frame.open.protocol_id;
  rec.policy.cls = frame.open.channel_class;
  if (frame.open.max_message_bytes > 0) {
    const size_t offered = frame.open.max_message_bytes;
    rec.policy.max_message_bytes =
        offered > AmpChannelLimits::kMaxChatBlobFrameBytes ? AmpChannelLimits::kMaxChatBlobFrameBytes : offered;
  }
  rec.reassembly = MessageReassembly(rec.policy.max_message_bytes);
  rec.state = ChannelState::Open;
  channels_.emplace(rec.id, std::move(rec));
  if (auto it = pending_handlers_.find(frame.header.channel_id); it != pending_handlers_.end()) {
    channels_.at(frame.header.channel_id).on_data = std::move(it->second);
    pending_handlers_.erase(it);
  }
  if (auto it = pending_terminal_handlers_.find(frame.header.channel_id);
      it != pending_terminal_handlers_.end()) {
    channels_.at(frame.header.channel_id).on_terminal = std::move(it->second);
    pending_terminal_handlers_.erase(it);
  }

  ChannelFrame ack;
  ack.header.frame_type = ChannelFrameType::OpenAck;
  ack.header.channel_id = frame.header.channel_id;
  ack.header.channel_seq = 0;
  ack.open_ack_result = 0;
  auto sent = SendFrame(ack, channels_.at(frame.header.channel_id));
  if (!sent) {
    return sent;
  }
  if (const auto it = protocol_handlers_.find(frame.open.protocol_id); it != protocol_handlers_.end()) {
    if (it->second) {
      it->second(frame.header.channel_id, frame.open.protocol_id);
    }
  }
  return Roe<void>();
}

Roe<void> ChannelMux::HandleOpenAck(ChannelFrame frame) {
  auto* channel = ChannelById(frame.header.channel_id);
  if (!channel) {
    return Error("amp mux: open ack for unknown channel");
  }
  if (frame.open_ack_result != 0) {
    channel->state = ChannelState::Closed;
    pending_open_data_.erase(frame.header.channel_id);
    return Error("amp mux: open rejected");
  }
  channel->state = ChannelState::Open;
  FlushPendingOpenData(frame.header.channel_id);
  return Roe<void>();
}

void ChannelMux::FlushPendingOpenData(const uint32_t channel_id) {
  auto it = pending_open_data_.find(channel_id);
  if (it == pending_open_data_.end()) {
    return;
  }
  auto payload = std::move(it->second);
  pending_open_data_.erase(it);
  (void)SendData(channel_id, std::move(payload));
}

Roe<void> ChannelMux::DispatchFrame(ChannelFrame frame) {
  auto* channel = ChannelById(frame.header.channel_id);
  if (!channel && frame.header.frame_type != ChannelFrameType::Open) {
    return Error("amp mux: unknown channel");
  }

  switch (frame.header.frame_type) {
  case ChannelFrameType::Open:
    return HandleOpen(std::move(frame));
  case ChannelFrameType::OpenAck:
    return HandleOpenAck(std::move(frame));
  case ChannelFrameType::Reset:
    if (channel) {
      channel->state = ChannelState::Closed;
      NotifyTerminal(*channel, "peer_reset");
    }
    return Roe<void>();
  case ChannelFrameType::Close:
    if (channel) {
      channel->state = ChannelState::Closed;
      NotifyTerminal(*channel, "peer_close");
    }
    return Roe<void>();
  case ChannelFrameType::Data:
    if (!channel || channel->state != ChannelState::Open) {
      return Error("amp mux: data on closed channel");
    }
    if (QosForClass(channel->policy.cls) == adp::QosClass::Reliable) {
      if (frame.header.channel_seq != channel->rx_seq) {
        return Error("amp mux: out of order seq");
      }
      channel->rx_seq += 1;
    }
    return DeliverPayload(*channel, std::move(frame.payload));
  case ChannelFrameType::Frag: {
    if (!channel || channel->state != ChannelState::Open) {
      return Error("amp mux: frag on closed channel");
    }
    if (QosForClass(channel->policy.cls) == adp::QosClass::Reliable) {
      if (frame.header.channel_seq != channel->rx_seq) {
        return Error("amp mux: out of order frag seq");
      }
      channel->rx_seq += 1;
    }
    auto assembled = channel->reassembly.Push(frame.frag, now_ms_ ? now_ms_() : 0);
    if (!assembled) {
      return assembled.error();
    }
    if (!assembled->has_value()) {
      return Roe<void>();
    }
    return DeliverPayload(*channel, std::move(**assembled));
  }
  default:
    return Error("amp mux: unhandled frame type");
  }
}

Roe<void> ChannelMux::OnSealedInbound(const uint32_t channel_id, const uint32_t channel_seq,
                                      std::span<const uint8_t> sealed) {
  const int64_t now_ms = now_ms_ ? now_ms_() : 0;
  auto opened = session_.Open(channel_id, channel_seq, sealed, now_ms);
  if (!opened) {
    return opened.error();
  }
  auto frame = ChannelWire::Decode(*opened);
  if (!frame) {
    return frame.error();
  }
  if (frame->header.channel_id != channel_id) {
    return Error("amp mux: channel id mismatch");
  }
  return DispatchFrame(std::move(*frame));
}

Roe<void> ChannelMux::SendData(const uint32_t channel_id, std::vector<uint8_t> payload) {
  auto* channel = ChannelById(channel_id);
  if (!channel || channel->state != ChannelState::Open) {
    return Error("amp mux: send on closed channel");
  }
  if (payload.size() > channel->policy.max_message_bytes) {
    return Error("amp mux: payload too large");
  }

  if (payload.size() <= kMaxSingleDataBytes) {
    ChannelFrame frame;
    frame.header.frame_type = ChannelFrameType::Data;
    frame.header.channel_id = channel_id;
    frame.header.channel_seq = channel->tx_seq;
    frame.payload = std::move(payload);
    auto sent = SendFrame(frame, *channel);
    if (!sent) {
      return sent.error();
    }
    ++channel->tx_seq;
    return Roe<void>();
  }

  const uint64_t msg_id = next_frag_msg_id_++;
  const uint16_t frag_count =
      static_cast<uint16_t>((payload.size() + kMaxSingleDataBytes - 1) / kMaxSingleDataBytes);
  // Reliable ADP: refuse before any FRAG leaves so a WindowFull cannot strand a partial message.
  if (QosForClass(channel->policy.cls) == adp::QosClass::Reliable && transport_credits_) {
    const size_t credits = transport_credits_();
    if (credits < frag_count) {
      return Error("amp mux: transport window full");
    }
  }
  for (uint16_t i = 0; i < frag_count; ++i) {
    const size_t offset = static_cast<size_t>(i) * kMaxSingleDataBytes;
    const size_t chunk_len = std::min(kMaxSingleDataBytes, payload.size() - offset);
    ChannelFrame frame;
    frame.header.frame_type = ChannelFrameType::Frag;
    frame.header.channel_id = channel_id;
    frame.header.channel_seq = channel->tx_seq;
    frame.frag.msg_id = msg_id;
    frame.frag.frag_index = i;
    frame.frag.frag_count = frag_count;
    frame.frag.total_len = static_cast<uint32_t>(payload.size());
    frame.frag.chunk.assign(payload.begin() + static_cast<std::ptrdiff_t>(offset),
                            payload.begin() + static_cast<std::ptrdiff_t>(offset + chunk_len));
    auto sent = SendFrame(frame, *channel);
    if (!sent) {
      return sent.error();
    }
    ++channel->tx_seq;
  }
  return Roe<void>();
}

Roe<void> ChannelMux::ResetChannel(const uint32_t channel_id, const uint32_t code) {
  auto* channel = ChannelById(channel_id);
  if (!channel) {
    return Error("amp mux: reset unknown channel");
  }
  ChannelFrame frame;
  frame.header.frame_type = ChannelFrameType::Reset;
  frame.header.channel_id = channel_id;
  frame.header.channel_seq = channel->tx_seq++;
  frame.reset_code = code;
  channel->state = ChannelState::Closed;
  return SendFrame(frame, *channel);
}

Roe<void> ChannelMux::CloseChannel(const uint32_t channel_id, std::string reason) {
  auto* channel = ChannelById(channel_id);
  if (!channel) {
    return Error("amp mux: close unknown channel");
  }
  ChannelFrame frame;
  frame.header.frame_type = ChannelFrameType::Close;
  frame.header.channel_id = channel_id;
  frame.header.channel_seq = channel->tx_seq++;
  frame.payload.assign(reason.begin(), reason.end());
  channel->state = ChannelState::Closing;
  return SendFrame(frame, *channel);
}

ChannelState ChannelMux::State(const uint32_t channel_id) const {
  if (const auto* ch = ChannelById(channel_id)) {
    return ch->state;
  }
  return ChannelState::Closed;
}

ChannelClass ChannelMux::Class(const uint32_t channel_id) const {
  if (const auto* ch = ChannelById(channel_id)) {
    return ch->policy.cls;
  }
  return ChannelClass::Control;
}

Roe<void> ChannelMux::SendCapabilityOffer(ChannelMux& mux, const CapabilityPayload& offer) {
  auto encoded = CapabilityCodec::Encode(offer);
  if (!encoded) {
    return encoded.error();
  }
  if (!mux.ChannelById(kCapabilityChannelId)) {
    auto id = mux.OpenOutbound("/pp-browser/amp-capability/1.0.0", CapabilityChannelPolicy(), kCapabilityChannelId);
    if (!id) {
      return id.error();
    }
  }
  const auto state = mux.State(kCapabilityChannelId);
  if (state == ChannelState::Open) {
    return mux.SendData(kCapabilityChannelId, std::move(*encoded));
  }
  if (state == ChannelState::Opening) {
    // Async ADP: OpenAck arrives on a later pump; queue DATA until then.
    mux.pending_open_data_[kCapabilityChannelId] = std::move(*encoded);
    return Roe<void>();
  }
  return Error("amp mux: capability channel not usable");
}

Roe<void> ChannelMux::InjectSealedForTest(const uint32_t channel_id, const uint32_t channel_seq,
                                          std::vector<uint8_t> sealed) {
  if (!transport_) {
    return Error("amp mux: no transport");
  }
  auto* channel = ChannelById(channel_id);
  if (!channel || channel->state != ChannelState::Open) {
    return Error("amp mux: inject on closed channel");
  }
  last_send_qos_ = QosForClass(channel->policy.cls);
  return transport_(channel_id, channel_seq, last_send_qos_, std::move(sealed));
}

} // namespace pp::amp
