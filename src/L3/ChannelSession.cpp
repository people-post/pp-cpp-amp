#include "amp/L3/ChannelSession.h"

namespace pp::amp {

ChannelSession::~ChannelSession() {
  ReleaseHandlers();
}

void ChannelSession::InstallMuxHandlers() {
  if (!mux_) {
    return;
  }
  if (auto pinned = weak_from_this().lock()) {
    const std::weak_ptr<ChannelSession> weak = pinned;
    (void)pinned;
    mux_->SetDataHandler(channel_id_, [weak](uint32_t, std::vector<uint8_t> payload) {
      auto session = weak.lock();
      if (!session || session->closed_) {
        return;
      }
      FrameHandler frame = session->on_frame_;
      if (!frame) {
        return;
      }
      const bool keep_open = frame(std::move(payload));
      if (!session->closed_ && (!keep_open || session->policy_.read_once)) {
        session->Close();
      }
    });
    mux_->SetTerminalHandler(channel_id_, [weak](uint32_t, const char* reason) {
      if (auto session = weak.lock()) {
        session->NotifyRemoteTerminal(reason);
      }
    });
    return;
  }
  // Stack / unique ownership (unit tests): raw this; caller must outlive mux callbacks.
  mux_->SetDataHandler(channel_id_, [this](uint32_t, std::vector<uint8_t> payload) {
    if (closed_ || !on_frame_) {
      return;
    }
    FrameHandler frame = on_frame_;
    const bool keep_open = frame(std::move(payload));
    if (!closed_ && (!keep_open || policy_.read_once)) {
      Close();
    }
  });
  mux_->SetTerminalHandler(channel_id_, [this](uint32_t, const char* reason) {
    NotifyRemoteTerminal(reason);
  });
}

void ChannelSession::Bind(ChannelMux& mux, const uint32_t channel_id, ChannelPolicy policy, FrameHandler on_frame,
                          ClosedCallback on_closed) {
  mux_ = &mux;
  channel_id_ = channel_id;
  policy_ = std::move(policy);
  on_frame_ = std::move(on_frame);
  on_closed_ = std::move(on_closed);
  closed_ = false;
  outbound_.clear();
  write_inflight_ = false;
  // Align mux reassembly / send limits with L4 policy (OPEN may have omitted or understated max).
  (void)mux_->ApplyChannelPolicy(channel_id_, policy_);
  InstallMuxHandlers();
}

void ChannelSession::SetFrameHandler(FrameHandler on_frame) {
  on_frame_ = std::move(on_frame);
}

void ChannelSession::SetClosedCallback(ClosedCallback on_closed) {
  on_closed_ = std::move(on_closed);
}

bool ChannelSession::EnqueueOutbound(std::vector<uint8_t> body) {
  if (closed_ || !mux_) {
    return false;
  }
  if (policy_.max_outbound_frames > 0 && outbound_.size() >= policy_.max_outbound_frames) {
    if (policy_.drop == ChannelDropPolicy::Never) {
      return false;
    }
    if (policy_.drop == ChannelDropPolicy::Oldest && !outbound_.empty()) {
      outbound_.pop_front();
      if (policy_.on_outbound_drop) {
        policy_.on_outbound_drop();
      }
    }
  }
  outbound_.push_back(std::move(body));
  PumpWrite();
  return true;
}

void ChannelSession::PumpWrite() {
  if (write_inflight_ || closed_ || !mux_ || outbound_.empty()) {
    return;
  }
  write_inflight_ = true;
  auto body = std::move(outbound_.front());
  outbound_.pop_front();
  auto sent = mux_->SendData(channel_id_, std::move(body));
  write_inflight_ = false;
  if (!sent) {
    FailOutbound(sent.error());
    return;
  }
  if (!outbound_.empty()) {
    PumpWrite();
  }
}

void ChannelSession::FailOutbound(const Error& error) {
  (void)error;
  closed_ = true;
  if (on_closed_) {
    on_closed_("write_failed");
  }
}

void ChannelSession::NotifyRemoteTerminal(const char* reason) {
  if (closed_) {
    return;
  }
  closed_ = true;
  if (on_closed_) {
    on_closed_(reason);
  }
}

void ChannelSession::Close() {
  if (closed_ || !mux_) {
    return;
  }
  closed_ = true;
  (void)mux_->CloseChannel(channel_id_);
  if (on_closed_) {
    on_closed_("close");
  }
}

void ChannelSession::CloseQuiet() {
  if (closed_ || !mux_) {
    return;
  }
  closed_ = true;
  (void)mux_->CloseChannel(channel_id_);
}

void ChannelSession::ReleaseHandlers() {
  on_frame_ = {};
  on_closed_ = {};
  if (!mux_) {
    return;
  }
  ChannelMux* mux = mux_;
  const uint32_t channel_id = channel_id_;
  mux_ = nullptr;
  mux->SetDataHandler(channel_id, {});
  mux->SetTerminalHandler(channel_id, {});
}

void ChannelSession::OrphanFromMux() {
  on_frame_ = {};
  on_closed_ = {};
  mux_ = nullptr;
  channel_id_ = 0;
  closed_ = true;
  outbound_.clear();
  write_inflight_ = false;
}

void ChannelSession::Reset(const uint32_t code) {
  if (closed_ || !mux_) {
    return;
  }
  closed_ = true;
  (void)mux_->ResetChannel(channel_id_, code);
  if (on_closed_) {
    on_closed_("reset");
  }
}

} // namespace pp::amp
