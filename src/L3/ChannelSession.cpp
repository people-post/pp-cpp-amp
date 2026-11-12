#include "amp/L3/ChannelSession.h"

#include <string_view>

namespace pp::amp {
namespace {

// Compare contents: a constexpr char pointer is folded to each translation unit's own literal.
bool IsLinkDropped(const char* reason) {
  return reason && std::string_view(reason) == ChannelMux::kLinkDroppedReason;
}

bool IsMuxDestroyed(const char* reason) {
  return reason && std::string_view(reason) == ChannelMux::kMuxDestroyedReason;
}

std::shared_ptr<const ChannelSession::FrameHandler> SharedFrameHandler(ChannelSession::FrameHandler handler) {
  return handler ? std::make_shared<const ChannelSession::FrameHandler>(std::move(handler)) : nullptr;
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
      const auto frame = session->on_frame_;
      if (!frame) {
        return;
      }
      const bool keep_open = (*frame)(std::move(payload));
      if (!session->closed_ && (!keep_open || session->policy_.read_once)) {
        session->Close();
      }
    });
    mux_->SetTerminalHandler(channel_id_, [weak](uint32_t, const char* reason) {
      if (auto session = weak.lock()) {
        session->OnMuxTerminal(reason);
      }
    });
    return;
  }
  // Stack / unique ownership (unit tests): raw this; caller must outlive mux callbacks.
  mux_->SetDataHandler(channel_id_, [this](uint32_t, std::vector<uint8_t> payload) {
    if (closed_ || !on_frame_) {
      return;
    }
    const auto frame = on_frame_;
    const bool keep_open = (*frame)(std::move(payload));
    if (!closed_ && (!keep_open || policy_.read_once)) {
      Close();
    }
  });
  mux_->SetTerminalHandler(channel_id_, [this](uint32_t, const char* reason) { OnMuxTerminal(reason); });
}

void ChannelSession::OnMuxTerminal(const char* reason) {
  if (IsLinkDropped(reason) || IsMuxDestroyed(reason)) {
    mux_ = nullptr; // the mux is gone (DetachAllChannels, ~ChannelMux): never touch it again
  }
  if (IsMuxDestroyed(reason)) {
    // Its owner is mid-destruction: drop the handlers (often what owns this session), call nothing.
    if (!closed_) {
      closed_ = true;
      FinishClosed(nullptr);
    }
    return;
  }
  NotifyRemoteTerminal(reason);
}

void ChannelSession::Bind(ChannelMux& mux, const uint32_t channel_id, ChannelPolicy policy, FrameHandler on_frame,
                          ClosedCallback on_closed) {
  mux_ = &mux;
  channel_id_ = channel_id;
  policy_ = std::move(policy);
  on_frame_ = SharedFrameHandler(std::move(on_frame));
  on_closed_ = std::move(on_closed);
  closed_ = false;
  reentrant_.clear();
  write_inflight_ = false;
  // Align mux reassembly / send limits with L4 policy (OPEN may have omitted or understated max).
  (void)mux_->ApplyChannelPolicy(channel_id_, policy_);
  InstallMuxHandlers();
}

void ChannelSession::SetFrameHandler(FrameHandler on_frame) {
  on_frame_ = SharedFrameHandler(std::move(on_frame));
}

void ChannelSession::SetClosedCallback(ClosedCallback on_closed) {
  on_closed_ = std::move(on_closed);
}

bool ChannelSession::EnqueueOutbound(std::vector<uint8_t> body) {
  if (closed_ || !mux_) {
    return false;
  }
  if (write_inflight_) {
    reentrant_.push_back(std::move(body)); // sent from inside a write: right after it, in order
    return true;
  }
  write_inflight_ = true;
  auto sent = mux_->SendData(channel_id_, std::move(body));
  write_inflight_ = false;
  if (!sent) {
    FailOutbound(sent.error()); // the last use of `this`
    return true;
  }
  SendReentrant();
  return true;
}

bool ChannelSession::EnqueueOutbound(const std::span<const uint8_t> body) {
  if (closed_ || !mux_) {
    return false;
  }
  if (write_inflight_) {
    reentrant_.emplace_back(body.begin(), body.end());
    return true;
  }
  write_inflight_ = true;
  auto sent = mux_->SendData(channel_id_, body);
  write_inflight_ = false;
  if (!sent) {
    FailOutbound(sent.error());
    return true;
  }
  SendReentrant();
  return true;
}

void ChannelSession::SendReentrant() {
  while (!reentrant_.empty() && !closed_ && mux_) {
    auto body = std::move(reentrant_.front());
    reentrant_.pop_front();
    write_inflight_ = true;
    auto sent = mux_->SendData(channel_id_, std::move(body));
    write_inflight_ = false;
    if (!sent) {
      FailOutbound(sent.error());
      return;
    }
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
  auto frame = std::move(on_frame_);
  ClosedCallback closed = std::move(on_closed_);
  on_frame_ = nullptr;
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
  on_frame_ = nullptr;
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
  on_frame_ = nullptr;
  on_closed_ = {};
  mux_ = nullptr;
  channel_id_ = 0;
  closed_ = true;
  reentrant_.clear();
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
