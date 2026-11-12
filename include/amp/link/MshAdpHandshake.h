#pragma once

#include "amp/L2/MshHandshake.h"
#include "amp/L2/Types.h"

#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace pp::amp {

struct MshAdpEstablished {
  SessionMaterial local_material;
  ByteVector master_ikm;
  ByteVector transcript_hash;
  ByteVector remote_identity_public_key;
};

/** One-sided MSH v1 driver over ADP Reliable payloads. */
class MshAdpHandshake {
public:
  enum class Role { Initiator, Responder };

  using SendWire = std::function<Roe<void>(std::vector<uint8_t> adp_payload)>;
  using CompleteHandler = std::function<void(Roe<MshAdpEstablished>)>;

  /**
   * @param chunked_wire When true (ADP datagrams), MSH bodies are AmpAdpCarrier-chunked.
   *        When false (ChannelSession / A024 nested carrier), each MSH is one EncodeMsh frame.
   * @param on_complete Runs once, success or failure, as the last thing the handshake does: it may
   *        destroy this handshake (PeerLink drops it on failure).
   */
  MshAdpHandshake(Role role, MshIdentity identity, SendWire send, CompleteHandler on_complete,
                  bool chunked_wire = true);

  /** Begin handshake (initiator only). */
  Roe<void> Start();

  /** Feed decoded MSH message body (without carrier header). */
  Roe<void> HandleMsh(MshMessageType type, std::span<const uint8_t> body);

  bool IsComplete() const { return complete_; }

  /**
   * Over a carrier (chunked_wire false) nothing below resends a lost message: the initiator resends its last one
   * on a timer (PeerLink::TickCarrierHandshake), and the responder answers a repeat of the message it last
   * answered by resending its reply (HandleMsh). A first send is one frame, as it always was (the outer link
   * fragments it); a resend goes as MshChunk pieces that each fit one outer frame, which the receiver keeps
   * across resends: a lost fragment no longer loses the whole message on every try. A peer from before this
   * ignores the pieces, as it got no resend before. LastChunks is the last message in pieces; LastWire is it
   * whole (the responder's Finished once complete, for a repeated Finished that arrives after it).
   */
  Roe<void> Resend();
  Roe<std::vector<std::vector<uint8_t>>> LastChunks() const;
  const std::vector<uint8_t>& LastWire() const { return last_wire_; }

private:
  struct KemState {
    ByteVector ephemeral_secret;
    ByteVector ephemeral_public;
  };

  Roe<KemState> NewEphemeralKem();
  Roe<std::vector<uint8_t>> EncodeHello(MshMessageType type, const KemState& kem);
  Roe<MshPayload> BuildLocalPayload(const KemState& kem, const ByteVector& remote_kem_pk,
                                    ByteVector& shared_secret_out);
  Roe<void> SendMessage(MshMessageType type, std::span<const uint8_t> body);
  /** Marks the handshake complete, then runs on_complete_ (which may destroy `this`). */
  Roe<void> Finish(Roe<MshAdpEstablished> outcome);
  Roe<void> Fail(std::string message);
  Roe<void> MaybeComplete();
  /** Responder: `type` / `body` is the message it last answered, sent again (its reply was lost). */
  bool IsRepeatOfLastAnswered(MshMessageType type, std::span<const uint8_t> body) const;

  Role role_;
  MshIdentity identity_;
  SendWire send_;
  CompleteHandler on_complete_;
  bool chunked_wire_ = true;
  bool complete_ = false;
  bool started_ = false;

  std::optional<KemState> local_kem_;
  std::optional<MshHello> remote_hello_;
  std::optional<MshPayload> remote_payload_;
  std::vector<ByteVector> transcript_;
  ByteVector client_ss_;
  ByteVector master_ikm_;
  /** Over a carrier only: the last message sent, as it went on the wire, and its type and body. */
  std::vector<uint8_t> last_wire_;
  MshMessageType last_type_{};
  ByteVector last_body_;
  /** Responder: the last message it answered. */
  std::optional<MshMessageType> answered_type_;
  ByteVector answered_body_;
};

} // namespace pp::amp
