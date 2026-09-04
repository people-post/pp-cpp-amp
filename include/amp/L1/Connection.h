#pragma once

#include "amp/L1/Clock.h"
#include "amp/L1/HmacBinder.h"
#include "amp/L1/Types.h"
#include "amp/L1/CodedFailure.h"
#include "amp/L1/ReplayWindow.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace pp::adp {

class Endpoint;

struct OpenParams {
  PeerKey key;
  AssocId id{};
  bool mint_id = true; // if true, Endpoint mints id when id is all-zero
  IpEndpoint peer{};
  int64_t skew_ms = kDefaultSkewMs;
  size_t replay_window = kDefaultReplayWindow;
  size_t reliable_window = kDefaultReliableWindow;
  int64_t rtx_interval_ms = kDefaultRtxIntervalMs;
  int max_rtx = kDefaultMaxRtx;
};

class Connection : public std::enable_shared_from_this<Connection> {
public:
  enum class Err : int32_t {
    Ok = 0,
    Closed,
    PayloadTooLarge,
    SeqWrap,
    WindowFull,
    WireError,
  };

  using Failure = CodedFailure<Err>;
  template <typename T>
  using Roe = CodedRoe<T, Err>;

  static Roe<std::shared_ptr<Connection>> Open(Endpoint& endpoint, OpenParams params);

  void Close();
  bool IsClosed() const { return closed_; }

  void SetPeerEndpoint(IpEndpoint peer);
  void UpgradeBinder(PeerKey key);
  IpEndpoint PeerEndpoint() const { return peer_; }
  AssocId Id() const { return id_; }

  Roe<void> Send(QosClass qos, std::span<const uint8_t> payload);

  /** Slots left before the next Reliable Send returns WindowFull. */
  size_t ReliableCreditsRemaining() const {
    if (outstanding_.size() >= params_.reliable_window) {
      return 0;
    }
    return params_.reliable_window - outstanding_.size();
  }

  void OnMessage(MessageHandler handler) { on_message_ = std::move(handler); }
  void OnPathChange(PathChangeHandler handler) { on_path_change_ = std::move(handler); }

  bool LooksAlive(int64_t now_ms) const;

  /** Send an empty Keepalive packet (BestEffort-style; no app payload). */
  Roe<void> SendKeepalive(int64_t now_ms);

  /** Drive retransmits / close drain. */
  void Tick(int64_t now_ms);

  /** Called by Endpoint after HMAC+decode success. */
  void HandleAuthenticated(const WirePacket& pkt, const IpEndpoint& from, int64_t now_ms);

  /** Verify HMAC, decode, then HandleAuthenticated. */
  void HandleDatagram(const IpEndpoint& from, std::span<const uint8_t> datagram, int64_t now_ms);

  const HmacBinder& Binder() const { return binder_; }

private:
  Connection(Endpoint& endpoint, OpenParams params);

  Roe<void> SendPacket(PacketType type, uint32_t seq, std::span<const uint8_t> payload,
                       int64_t now_ms);
  Roe<void> SendPacketAsFailure(PacketType type, uint32_t seq, std::span<const uint8_t> payload,
                                int64_t now_ms);
  uint32_t TruncTs(int64_t now_ms) const;
  bool AcceptSkew(uint32_t ts, int64_t now_ms) const;
  void MaybeLearnPath(const IpEndpoint& from);

  Endpoint* endpoint_ = nullptr;
  AssocId id_{};
  HmacBinder binder_;
  IpEndpoint peer_{};
  OpenParams params_;
  bool closed_ = false;
  bool peer_closed_ = false;

  uint32_t tx_seq_be_ = 0;
  uint32_t tx_seq_rel_ = 0;
  ReplayWindow rx_be_;
  ReplayWindow rx_rel_;

  MessageHandler on_message_;
  PathChangeHandler on_path_change_;
  int64_t last_auth_rx_ms_ = 0;

  struct Outstanding {
    uint32_t seq = 0;
    std::vector<uint8_t> payload;
    int64_t next_rtx_ms = 0;
    int attempts = 0;
  };
  std::deque<Outstanding> outstanding_;
};

} // namespace pp::adp
