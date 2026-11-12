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
   * Each MSH message goes as AmpAdpCarrier::EncodeMshChunked pieces (one frame when it fits).
   * @param over_carrier True over a ChannelSession (A024 nested carrier): this handshake resends (below).
   * @param on_complete Runs once, success or failure, as the last thing the handshake does: it may
   *        destroy this handshake (PeerLink drops it on failure).
   */
  MshAdpHandshake(Role role, MshIdentity identity, SendWire send, CompleteHandler on_complete,
                  bool over_carrier = false);

  /** Begin handshake (initiator only). */
  Roe<void> Start();

  /** Feed decoded MSH message body (without carrier header). */
  Roe<void> HandleMsh(MshMessageType type, std::span<const uint8_t> body);

  bool IsComplete() const { return complete_; }

  /**
   * Over a carrier nothing below resends a lost message: the initiator resends its last one on a timer
   * (PeerLink::TickCarrierHandshake), and the responder answers a repeat of the message it last answered by
   * resending its reply (HandleMsh). Each send goes as pieces that each fit one outer frame, which the receiver
   * keeps across resends: a lost piece does not lose the whole message on every try. LastChunks is the last
   * message sent, in its pieces (the responder's Finished once complete, for a repeated Finished after it).
   */
  Roe<void> Resend();
  Roe<std::vector<std::vector<uint8_t>>> LastChunks() const;

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
  bool over_carrier_ = false;
  bool complete_ = false;
  bool started_ = false;

  std::optional<KemState> local_kem_;
  std::optional<MshHello> remote_hello_;
  std::optional<MshPayload> remote_payload_;
  std::vector<ByteVector> transcript_;
  ByteVector client_ss_;
  ByteVector master_ikm_;
  /** Over a carrier only: the last message sent, its type and body. */
  std::optional<MshMessageType> last_type_;
  ByteVector last_body_;
  /** Responder: the last message it answered. */
  std::optional<MshMessageType> answered_type_;
  ByteVector answered_body_;
};

} // namespace pp::amp
