#pragma once

#include "amp/L1/PacketProtection.h"
#include "amp/L1/Types.h"
#include "amp/L3/AmpChannelLimits.h"
#include "amp/L3/Capability.h"
#include "amp/L3/ChannelPolicy.h"
#include "amp/L3/ChannelWire.h"
#include "amp/L3/MessageReassembly.h"
#include "amp/L3/Types.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace pp::amp {

/** Channel-mux policy a deployment may tune (see docs/TUNING.md). */
struct MuxTuning {
  size_t max_concurrent_channels = AmpChannelLimits::kMaxConcurrentChannels;
  /** Reliable bytes waiting for transport window space; SendData beyond it fails. */
  size_t max_queued_bytes = 32 * 1024 * 1024;
  /** A fragmented message not completed this long after its first fragment is dropped. */
  int64_t frag_assembly_timeout_ms = kDefaultFragAssemblyTimeoutMs;
};

/**
 * Multiplexes L3 channels over one AMP Session. Io-thread affine. Frames go to and come from its transport in the
 * clear: the link seals and opens them (with the session's keys, the packet number as nonce — A029 H1).
 */
class ChannelMux {
public:
  /** One encoded frame (ChannelWire), to be sealed and sent by the link. */
  using TransportSend = std::function<Roe<void>(adp::QosClass qos, std::vector<uint8_t> frame)>;
  /** Remaining ADP reliable slots (SIZE_MAX = unlimited / BestEffort-only transport). */
  using TransportCredits = std::function<size_t()>;
  using DataHandler = std::function<void(uint32_t channel_id, std::vector<uint8_t> payload)>;
  using TerminalHandler = std::function<void(uint32_t channel_id, const char* reason)>;
  using InboundOpenHandler = std::function<void(uint32_t channel_id, const std::string& protocol_id)>;

  /** `initiator`: the end that dialed (odd channel ids; the responder's are even). */
  explicit ChannelMux(bool initiator);
  /** Tells each channel still holding a terminal handler kMuxDestroyedReason (see there). */
  ~ChannelMux();
  ChannelMux(const ChannelMux&) = delete;
  ChannelMux& operator=(const ChannelMux&) = delete;

  void SetTransport(TransportSend send);
  void SetTransportCredits(TransportCredits credits);
  void SetClock(std::function<int64_t()> now_ms);
  void SetTuning(const MuxTuning& tuning) { tuning_ = tuning; }

  /**
   * The largest encoded frame its transport carries in one packet (the link's budget, A029 H1): a payload that fits
   * with a frame header goes as one DATA frame, a larger one as FRAG chunks that fill the budget. A budget below
   * kMinFrameBytes is refused and the old one kept.
   */
  Roe<void> SetMaxFrameBytes(size_t bytes);
  size_t MaxFrameBytes() const { return max_frame_bytes_; }
  /** A direct ADP link's budget: one sealed packet's payload. */
  static constexpr size_t kDirectMaxFrameBytes = adp::kMaxSealedPayload;
  /**
   * The smallest budget a link may set: a fragment then carries 39 B. Below it the frame header and fragment fields
   * outweigh what a fragment carries, and below 26 B a fragment carries nothing.
   */
  static constexpr size_t kMinFrameBytes = 64;
  /** The most fragments one message may take (a FRAG frame counts them in 16 bits). */
  static constexpr size_t kMaxFragmentsPerMessage = 0xFFFF;

  /** Allocate id and send OPEN (local initiator). Pass fixed_id for reserved channels (e.g. 0). */
  Roe<uint32_t> OpenOutbound(const std::string& protocol_id, ChannelPolicy policy,
                             std::optional<uint32_t> fixed_id = std::nullopt);

  /** Register handler for inbound DATA on a channel. */
  void SetDataHandler(uint32_t channel_id, DataHandler handler);

  /**
   * Update local channel policy (e.g. ChannelSession::Bind after inbound OPEN).
   * Recreates MessageReassembly from policy.max_message_bytes (drops in-flight partials).
   */
  Roe<void> ApplyChannelPolicy(uint32_t channel_id, ChannelPolicy policy);

  /** Invoked on inbound CLOSE/RESET for a channel. */
  void SetTerminalHandler(uint32_t channel_id, TerminalHandler handler);

  /** Invoked after inbound OPEN + OpenAck for registered protocol_id (L4 entry). */
  void SetProtocolHandler(const std::string& protocol_id, InboundOpenHandler handler);
  void ClearProtocolHandlers();

  /** Terminal reason when the link (and this mux) is being destroyed. */
  static constexpr const char* kLinkDroppedReason = "link-dropped";
  /**
   * Terminal reason, from the mux's destructor, for channels a link drop did not detach: the mux
   * dies with its owner (a PeerLinkManager torn down with links up). A session forgets the mux and
   * ends quietly — no closed callback, as its owner is mid-destruction. Without it a served session
   * whose own handlers own it was never told and leaked, and one that outlived the mux kept a
   * dangling pointer to it.
   */
  static constexpr const char* kMuxDestroyedReason = "mux-destroyed";
  /** Terminal reason when a channel's ChannelPolicy::read_timeout expired (the mux reset it). */
  static constexpr const char* kReadTimeoutReason = "read-timeout";
  /**
   * Terminal reason when an inbound message could not be reassembled — larger than the channel's
   * max_message_bytes, malformed fragments, too many partial messages — and the mux reset the
   * channel: the sender learns at the first fragment instead of sending the rest to be dropped.
   */
  static constexpr const char* kBadMessageReason = "bad-message";
  /**
   * The mux is about to be destroyed with its link: close every channel record and hand back each
   * channel's terminal notice (reason kLinkDroppedReason) for the caller to run **after** the mux is
   * gone. Sessions bound to it forget the mux in that notice and report the channel closed — without
   * this they kept a dangling mux pointer (use-after-free on the next write or on destruction).
   */
  std::vector<std::function<void()>> DetachAllChannels();

  /** OpenAck result: the responder has no handler for the channel's protocol. */
  static constexpr uint8_t kOpenAckNoHandler = 1;
  /**
   * The terminal reason an opener's channel gets for kOpenAckNoHandler: the peer does not serve the protocol (an older
   * release), as opposed to a refusal it may lift or an open that ran out of time.
   */
  static constexpr const char* kOpenRejectedNoHandler = "open rejected: no handler";
  /** OpenAck result: too many concurrent channels already open on this mux. */
  static constexpr uint8_t kOpenAckTooManyChannels = 2;
  /** OpenAck result: channel id parity does not match the opener's role (dual-open glare guard). */
  static constexpr uint8_t kOpenAckBadIdParity = 3;
  /**
   * When true, an inbound OPEN is refused (OpenAck kOpenAckNoHandler, no channel record) unless a
   * protocol handler is registered for its protocol, a data handler was bound to its id before it
   * arrived, or it is the capability channel. Without this, the opener saw an open channel whose
   * requests were silently dropped and waited out its own timeout. Off by default;
   * PeerLinkManager::SetRefuseUnhandledOpens applies it to every link's mux.
   */
  void SetRefuseUnhandledOpens(bool refuse) { refuse_unhandled_opens_ = refuse; }

  Roe<void> SendData(uint32_t channel_id, std::vector<uint8_t> payload);
  /** The same from the caller's bytes: a payload that fits one frame is encoded straight from them (one copy). */
  Roe<void> SendData(uint32_t channel_id, std::span<const uint8_t> payload);
  Roe<void> ResetChannel(uint32_t channel_id, uint32_t code = 1);
  Roe<void> CloseChannel(uint32_t channel_id, std::string reason = {});

  /** One inbound frame the link opened (ChannelWire bytes). */
  Roe<void> OnFrameInbound(std::span<const uint8_t> frame);

  ChannelState State(uint32_t channel_id) const;
  /** Add this mux's Open channels to `by_protocol` (protocol id → count). */
  void CountOpenChannels(std::unordered_map<std::string, size_t>& by_protocol) const;
  /**
   * Whether any channel but the capability channel is open (the link is in use). The capability
   * channel is told by its number, not its protocol id: a peer on another release may name it
   * differently.
   */
  bool HasOpenChannelsBesidesCapability() const;
  ChannelClass Class(uint32_t channel_id) const;
  /** The highest ChannelPolicy::keepalive among channels open or opening on this mux. */
  KeepaliveTier ChannelsKeepaliveTier() const;
  adp::QosClass LastSendQos() const { return last_send_qos_; }

  static Roe<void> SendCapabilityOffer(ChannelMux& mux, const CapabilityPayload& offer);

  /** Test hook — send an encoded frame on the mux transport as if `channel_id` sent it. */
  Roe<void> InjectFrameForTest(uint32_t channel_id, std::vector<uint8_t> frame);

  /**
   * Sweep expired FRAG partial-assembly state on every channel, reset channels
   * whose read_timeout expired, and send reliable frames waiting for transport
   * window space (drive periodically).
   */
  void Tick(int64_t now_ms);

  /** Reliable frames waiting for transport window space (all channels). */
  size_t QueuedFrameCount() const;

  /** What one channel has waiting for transport window space. */
  struct ChannelBacklog {
    size_t frames = 0;
    size_t bytes = 0;
    /** How long its oldest waiting frame has waited (0 when none waits). */
    int64_t oldest_age_ms = 0;
  };
  /**
   * `channel_id`'s reliable frames still waiting. A sender of time-bound data (call media on a reliable fallback)
   * reads it to drop a frame that would only wait behind others, rather than queue it.
   */
  ChannelBacklog BacklogOf(uint32_t channel_id) const;

  /** Default cap on bytes waiting for window space (MuxTuning::max_queued_bytes). */
  static constexpr size_t kMaxQueuedBytes = 32 * 1024 * 1024;

  /** The transport has window again (an ack): send what waited for it. */
  void OnTransportCredit() { FlushQueued(); }

private:
  struct ChannelRecord {
    uint32_t id = 0;
    std::string protocol_id;
    ChannelPolicy policy;
    ChannelState state = ChannelState::Closed;
    uint32_t tx_seq = 1;
    uint32_t rx_seq = 1;
    /**
     * Shared, so a frame pins it with a reference count rather than a copy (a std::function's copy allocated and
     * copied its captures on every frame: a busy relay's largest libc cost).
     */
    std::shared_ptr<const DataHandler> on_data;
    TerminalHandler on_terminal;
    MessageReassembly reassembly;
    /** Tick time the read_timeout clock last (re)started; -1 = restart on the next Tick. */
    int64_t read_clock_ms = -1;
  };

  ChannelRecord* ChannelById(uint32_t channel_id);
  const ChannelRecord* ChannelById(uint32_t channel_id) const;
  /** A reliable frame encoded but not yet sealed/sent (waiting for window space). */
  struct QueuedFrame {
    std::vector<uint8_t> wire;
    int64_t queued_at_ms = 0;
  };

  void AdvanceTxSeq(uint32_t channel_id);
  Roe<void> SendFrame(const ChannelFrame& frame, ChannelRecord& channel);
  /**
   * Send an encoded frame (the link seals it), or queue it: a reliable frame waits while
   * the transport window is full or its channel already has frames waiting, so
   * each channel's frames (and its CLOSE) stay in order.
   */
  Roe<void> SendWire(uint32_t channel_id, adp::QosClass qos, std::vector<uint8_t> wire);
  Roe<void> Transport(adp::QosClass qos, const std::vector<uint8_t>& wire);
  bool HasReliableCredit() const;
  /** Send queued frames round-robin across channels while window space lasts. */
  void FlushQueued();
  void DropQueued(uint32_t channel_id);
  Roe<void> DispatchFrame(ChannelFrame frame);
  Roe<void> DeliverPayload(ChannelRecord& channel, std::vector<uint8_t> payload);
  Roe<void> HandleOpen(ChannelFrame frame);
  Roe<void> RefuseOpen(const ChannelFrame& frame, uint8_t result);
  Roe<void> HandleOpenAck(ChannelFrame frame);
  void NotifyTerminal(ChannelRecord& channel, const char* reason);
  /** Tick: reset channels with nothing inbound for their policy's read_timeout. */
  void ExpireIdleReaders(int64_t now_ms);

  TransportSend transport_;
  size_t max_frame_bytes_ = kDirectMaxFrameBytes;
  TransportCredits transport_credits_;
  std::unordered_map<uint32_t, std::deque<QueuedFrame>> send_queues_;
  /** Channels with queued frames, in round-robin order. */
  std::deque<uint32_t> send_order_;
  size_t queued_bytes_ = 0;
  MuxTuning tuning_{};
  std::function<int64_t()> now_ms_;
  std::unordered_map<uint32_t, ChannelRecord> channels_;
  std::unordered_map<uint32_t, DataHandler> pending_handlers_;
  std::unordered_map<uint32_t, TerminalHandler> pending_terminal_handlers_;
  std::unordered_map<uint32_t, std::vector<uint8_t>> pending_open_data_;
  std::unordered_map<std::string, InboundOpenHandler> protocol_handlers_;
  // Dual-open: initiator uses odd ids, responder even (see ChannelMux ctor).
  uint32_t next_dynamic_id_ = 1;
  adp::QosClass last_send_qos_ = adp::QosClass::Reliable;
  uint64_t next_frag_msg_id_ = 1;
  bool refuse_unhandled_opens_ = false;

  void FlushPendingOpenData(uint32_t channel_id);
};

} // namespace pp::amp
