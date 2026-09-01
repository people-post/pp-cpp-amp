#include "amp/L3/ChannelBridge.h"

namespace pp::amp {

void ChannelBridge::Attach(std::shared_ptr<ChannelSession> left, std::shared_ptr<ChannelSession> right,
                           CancelCheck is_cancelled, ClosedCallback on_closed) {
  Stop();
  left_ = std::move(left);
  right_ = std::move(right);
  on_closed_ = std::move(on_closed);
  user_cancel_ = std::move(is_cancelled);
  cancelled_ = std::make_shared<std::atomic<bool>>(false);
  closed_fired_ = std::make_shared<std::atomic<bool>>(false);

  ArmOneWay(left_, right_);
  ArmOneWay(right_, left_);
}

void ChannelBridge::ArmOneWay(std::shared_ptr<ChannelSession> from, std::shared_ptr<ChannelSession> to) {
  if (!from || !to || !cancelled_ || !closed_fired_) {
    return;
  }
  auto cancel_flag = cancelled_;
  auto fired = closed_fired_;
  auto on_closed = on_closed_;
  auto user_cancel = user_cancel_;
  auto left = left_;
  auto right = right_;

  auto finish = [cancel_flag, fired, on_closed, left, right]() {
    cancel_flag->store(true, std::memory_order_release);
    if (fired->exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    if (left && !left->IsClosed()) {
      left->CloseQuiet();
    }
    if (right && !right->IsClosed()) {
      right->CloseQuiet();
    }
    if (on_closed) {
      on_closed();
    }
  };

  auto cancelled = [cancel_flag, user_cancel]() {
    if (cancel_flag->load(std::memory_order_acquire)) {
      return true;
    }
    if (user_cancel && user_cancel()) {
      cancel_flag->store(true, std::memory_order_release);
      return true;
    }
    return false;
  };

  from->SetFrameHandler([to, cancelled, finish](Roe<std::vector<uint8_t>> body) {
    if (cancelled()) {
      return false;
    }
    if (!body) {
      finish();
      return false;
    }
    if (!to || to->IsClosed() || !to->EnqueueOutbound(std::move(*body))) {
      finish();
      return false;
    }
    return true;
  });
  from->SetClosedCallback([finish](const char*) { finish(); });
}

void ChannelBridge::Stop() {
  if (cancelled_) {
    cancelled_->store(true, std::memory_order_release);
  }
  if (closed_fired_ && !closed_fired_->exchange(true, std::memory_order_acq_rel)) {
    if (left_ && !left_->IsClosed()) {
      left_->CloseQuiet();
    }
    if (right_ && !right_->IsClosed()) {
      right_->CloseQuiet();
    }
    if (on_closed_) {
      on_closed_();
    }
  }
  left_.reset();
  right_.reset();
  on_closed_ = nullptr;
  user_cancel_ = nullptr;
  cancelled_.reset();
  closed_fired_.reset();
}

} // namespace pp::amp
