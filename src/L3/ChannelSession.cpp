#include "amp/L3/ChannelSession.h"

#include <string_view>

namespace pp::amp {
namespace {

// Compare contents: a constexpr char pointer is folded to each translation unit's own literal.
bool IsLinkDropped(const char* reason) {
  return reason && std::string_view(reason) == ChannelMux::kLinkDroppedReason;
}

} // namespace

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
        if (IsLinkDropped(reason)) {
          session->mux_ = nullptr;  // the mux is gone (DetachAllChannels): never touch it again
        }
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
    if (IsLinkDropped(reason)) {
      mux_ = nullptr;
    }
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
  FinishClosed("write_failed");
}

void ChannelSession::NotifyRemoteTerminal(const char* reason) {
  if (closed_) {
    return;
  }
  closed_ = true;
  FinishClosed(reason);
}

void ChannelSession::FinishClosed(const char* reason) {
  // A closed session never touches its mux again: the mux notifies only open channels when its
  // link drops, so a pointer kept past close dangled once the link went, and the destructor's
  // ReleaseHandlers called into the freed mux. (Handlers the mux still holds for this channel
  // capture a weak pointer and no-op.)
  mux_ = nullptr;
  // Locals keep both callables alive while the closed callback runs; whatever they own (often
  // this session) goes when they do, so nothing may touch `this` after this call.
  FrameHandler frame = std::move(on_frame_);
  ClosedCallback closed = std::move(on_closed_);
  on_frame_ = {};
  on_closed_ = {};
  if (reason && closed) {
    closed(reason);
  }
}

void ChannelSession::Close() {
  if (closed_ || !mux_) {
    return;
  }
  closed_ = true;
  (void)mux_->CloseChannel(channel_id_);
  FinishClosed("close");
}

void ChannelSession::CloseQuiet() {
  if (closed_ || !mux_) {
    return;
  }
  closed_ = true;
  (void)mux_->CloseChannel(channel_id_);
  FinishClosed(nullptr);
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
  FinishClosed("reset");
}

} // namespace pp::amp
