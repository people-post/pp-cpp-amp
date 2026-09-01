#pragma once

#include "amp/L3/ChannelSession.h"

#include <atomic>
#include <functional>
#include <memory>

namespace pp::amp {

/**
 * Bidirectional DATA splice between two ChannelSessions (AMP StreamBridge).
 * Io-thread affine. Replaces frame handlers on both sessions.
 */
class ChannelBridge {
public:
  using CancelCheck = std::function<bool()>;
  using ClosedCallback = std::function<void()>;

  ChannelBridge() = default;
  ChannelBridge(const ChannelBridge&) = delete;
  ChannelBridge& operator=(const ChannelBridge&) = delete;

  /**
   * Arm forwarders on already-bound sessions. Either side close / cancel stops both.
   * `on_closed` runs at most once.
   */
  void Attach(std::shared_ptr<ChannelSession> left, std::shared_ptr<ChannelSession> right,
              CancelCheck is_cancelled = {}, ClosedCallback on_closed = {});

  void Stop();

private:
  void ArmOneWay(std::shared_ptr<ChannelSession> from, std::shared_ptr<ChannelSession> to);

  std::shared_ptr<ChannelSession> left_;
  std::shared_ptr<ChannelSession> right_;
  CancelCheck user_cancel_;
  std::shared_ptr<std::atomic<bool>> cancelled_;
  ClosedCallback on_closed_;
  std::shared_ptr<std::atomic<bool>> closed_fired_;
};

} // namespace pp::amp
