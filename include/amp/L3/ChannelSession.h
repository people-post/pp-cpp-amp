#pragma once

#include "amp/L3/ChannelMux.h"
#include "amp/L3/ChannelPolicy.h"


#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <vector>

namespace pp::amp {

/**
 * Single-channel L3 pipe (io-thread affine) — AMP counterpart to DuplexFrameSession.
 *
 * Ownership (repo OWNERSHIP.md; mesh A027): durable owner is the L4 parent slot;
 * prefer `std::shared_ptr` so Bind can pin across mux dispatch. Callbacks request
 * close; parent destroys / unbinds.
 */
class ChannelSession : public std::enable_shared_from_this<ChannelSession> {
public:
  using FrameHandler = std::function<bool(Roe<std::vector<uint8_t>> body)>;
  using ClosedCallback = std::function<void(const char* reason)>;

  ChannelSession() = default;
  ~ChannelSession();

  ChannelSession(const ChannelSession&) = delete;
  ChannelSession& operator=(const ChannelSession&) = delete;

  void Bind(ChannelMux& mux, uint32_t channel_id, ChannelPolicy policy, FrameHandler on_frame,
            ClosedCallback on_closed = {});

  /** Replace the DATA handler after Bind (e.g. circuit tunnel: JSON handshake → forward). */
  void SetFrameHandler(FrameHandler on_frame);
  /** Replace the terminal/close callback after Bind. */
  void SetClosedCallback(ClosedCallback on_closed);

  /** Queue an L4 payload. Returns false if session closed or queue full (drop policy applied). */
  bool EnqueueOutbound(std::vector<uint8_t> body);

  void Close();
  /** Close the mux channel without invoking `on_closed` (provisional / abandoned roles). */
  void CloseQuiet();
  void Reset(uint32_t code = 1);

  /**
   * Drop Bind callbacks + mux handlers (parent unbind — [A027]).
   * Safe during an on_frame_ callback when Bind holds a dispatch pin.
   * Prefer after CloseQuiet from the owning L4 slot helper.
   */
  void ReleaseHandlers();

  /**
   * Detach from a mux that is already gone or about to be destroyed (PeerLink drop).
   * Does not call into mux_ — only clears local state ([A027] / dangling mux safety).
   */
  void OrphanFromMux();

  uint32_t ChannelId() const { return channel_id_; }
  ChannelMux* Mux() { return mux_; }
  size_t OutboundBacklog() const { return outbound_.size() + (write_inflight_ ? 1 : 0); }
  bool IsClosed() const { return closed_; }

private:
  void PumpWrite();
  void FailOutbound(const Error& error);
  void NotifyRemoteTerminal(const char* reason);
  void InstallMuxHandlers();

  ChannelMux* mux_ = nullptr;
  uint32_t channel_id_ = 0;
  ChannelPolicy policy_;
  FrameHandler on_frame_;
  ClosedCallback on_closed_;
  std::deque<std::vector<uint8_t>> outbound_;
  bool write_inflight_ = false;
  bool closed_ = false;
};

} // namespace pp::amp
