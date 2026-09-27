#pragma once

#include "amp/link/AmpAdpCarrier.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace pp::amp {

struct CarrierLaneConfig {
  /** Unacked frames in flight; a full window refuses the send (like ADP WindowFull). */
  size_t window = 256;
  int64_t initial_rto_ms = 500;
  int64_t min_rto_ms = 200;
  int64_t max_rto_ms = 3000;
  /** Transmissions per frame (first send included) before the lane gives up. */
  int max_attempts = 10;
  /** A frame is resent at once when this many acks passed it by (reordering tolerance). */
  int fast_retransmit_skips = 3;
};

/**
 * End-to-end reliable delivery for a nested link's Reliable-class frames over a best-effort
 * circuit carrier ([A024] follow-on; ADR_LINK_PLANE §11). The relay only splices bytes, so ADP's
 * per-hop reliability ends at the relay; a Reliable mux channel needs every frame, in order —
 * one loss or reorder wedged it for good.
 *
 * Pure logic (no I/O, caller's clock). TX: `Wrap` numbers a frame and keeps it until acked;
 * `DueResends` returns what to send again (adaptive RTO, RFC 6298 estimator, Karn's rule, fast
 * resend on selective acks). RX: `OnData` holds out-of-order frames and releases them in order;
 * every data frame is answered with `Ack` (cumulative + 64 selective bits).
 *
 * Negotiation: both ends announce the lane with probes (`EncodeLaneAck` with nothing acked)
 * during the handshake; a peer that never sends a lane frame is an older build, and frames go
 * to it as before (`PeerSpeaksLane` false).
 */
class CarrierLane {
public:
  explicit CarrierLane(CarrierLaneConfig config = {});

  // --- negotiation
  void NotePeerSpeaksLane() { peer_speaks_lane_ = true; }
  bool PeerSpeaksLane() const { return peer_speaks_lane_; }

  // --- TX
  /** LaneData wire for `inner`, kept until acked; nullopt when the window is full. */
  std::optional<std::vector<uint8_t>> Wrap(std::span<const uint8_t> inner, int64_t now_ms);
  size_t Credits() const;
  void OnAck(const LaneAckFields& ack, int64_t now_ms);
  /** Frames to send again now (LaneData wires). */
  std::vector<std::vector<uint8_t>> DueResends(int64_t now_ms);
  /** A frame ran out of attempts: the path is gone, the link must go. */
  bool Failed() const { return failed_; }
  size_t InFlight() const { return outstanding_.size(); }
  int64_t RtoMs() const { return rto_ms_; }

  // --- RX
  struct Received {
    /** Inner wires now deliverable, in order. */
    std::vector<std::vector<uint8_t>> deliver;
    /** Ack to send back (always — a duplicate means our last ack was lost). */
    LaneAckFields ack;
  };
  Received OnData(uint32_t seq, std::span<const uint8_t> inner);

private:
  struct Outstanding {
    uint32_t seq = 0;
    std::vector<uint8_t> wire;
    int64_t sent_ms = 0;
    int64_t next_resend_ms = 0;
    int attempts = 1;
    int skips = 0;
  };

  LaneAckFields CurrentAck() const;
  void SampleRtt(int64_t sample_ms);
  int64_t BackoffRto(int attempts) const;

  CarrierLaneConfig config_;
  bool peer_speaks_lane_ = false;
  bool failed_ = false;

  uint32_t next_tx_seq_ = 1;
  std::deque<Outstanding> outstanding_;  // ascending seq
  int64_t rto_ms_;
  int64_t srtt_ms_ = 0;
  int64_t rttvar_ms_ = 0;
  bool have_rtt_ = false;

  uint32_t rx_cumulative_ = 0;  // every seq ≤ this was delivered
  std::map<uint32_t, std::vector<uint8_t>> rx_hold_;
};

} // namespace pp::amp
