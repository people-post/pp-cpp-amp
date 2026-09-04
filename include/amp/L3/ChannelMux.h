#pragma once

#include "amp/L1/Types.h"
#include "amp/L3/Capability.h"
#include "amp/L3/ChannelPolicy.h"
#include "amp/L3/ChannelWire.h"
#include "amp/L3/MessageReassembly.h"
#include "amp/L2/Session.h"


#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace pp::amp {

/** Multiplexes L3 channels over one AMP Session. Io-thread affine. */
class ChannelMux {
public:
  using TransportSend =
      std::function<void(uint32_t channel_id, uint32_t channel_seq, adp::QosClass qos, std::vector<uint8_t> sealed)>;
  using DataHandler = std::function<void(uint32_t channel_id, std::vector<uint8_t> payload)>;
  using TerminalHandler = std::function<void(uint32_t channel_id, const char* reason)>;
  using InboundOpenHandler = std::function<void(uint32_t channel_id, const std::string& protocol_id)>;

  explicit ChannelMux(Session& session);

  void SetPeerSession(Session* peer_session);
  void SetTransport(TransportSend send);
  void SetClock(std::function<int64_t()> now_ms);

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

  Roe<void> SendData(uint32_t channel_id, std::vector<uint8_t> payload);
  Roe<void> ResetChannel(uint32_t channel_id, uint32_t code = 1);
  Roe<void> CloseChannel(uint32_t channel_id, std::string reason = {});

  /** After L2 Session::Open on inbound ADP payload. */
  Roe<void> OnSealedInbound(uint32_t channel_id, uint32_t channel_seq, std::span<const uint8_t> sealed);

  ChannelState State(uint32_t channel_id) const;
  ChannelClass Class(uint32_t channel_id) const;
  adp::QosClass LastSendQos() const { return last_send_qos_; }

  static Roe<void> SendCapabilityOffer(ChannelMux& mux, const CapabilityPayload& offer);

  /** Test hook — send pre-sealed L3 bytes on the mux transport. */
  Roe<void> InjectSealedForTest(uint32_t channel_id, uint32_t channel_seq, std::vector<uint8_t> sealed);

private:
  struct ChannelRecord {
    uint32_t id = 0;
    std::string protocol_id;
    ChannelPolicy policy;
    ChannelState state = ChannelState::Closed;
    uint32_t tx_seq = 1;
    uint32_t rx_seq = 1;
    DataHandler on_data;
    TerminalHandler on_terminal;
    MessageReassembly reassembly;
  };

  ChannelRecord* ChannelById(uint32_t channel_id);
  const ChannelRecord* ChannelById(uint32_t channel_id) const;
  Roe<void> SendFrame(const ChannelFrame& frame, ChannelRecord& channel);
  Roe<void> DispatchFrame(ChannelFrame frame);
  Roe<void> DeliverPayload(ChannelRecord& channel, std::vector<uint8_t> payload);
  Roe<void> HandleOpen(ChannelFrame frame);
  Roe<void> HandleOpenAck(ChannelFrame frame);
  void NotifyTerminal(ChannelRecord& channel, const char* reason);

  Session& session_;
  Session* peer_session_ = nullptr;
  TransportSend transport_;
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

  void FlushPendingOpenData(uint32_t channel_id);
};

} // namespace pp::amp
