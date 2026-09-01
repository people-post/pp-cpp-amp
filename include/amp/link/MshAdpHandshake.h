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
   */
  MshAdpHandshake(Role role, MshIdentity identity, SendWire send, CompleteHandler on_complete,
                  bool chunked_wire = true);

  /** Begin handshake (initiator only). */
  Roe<void> Start();

  /** Feed decoded MSH message body (without carrier header). */
  Roe<void> HandleMsh(MshMessageType type, std::span<const uint8_t> body);

  bool IsComplete() const { return complete_; }

private:
  struct KemState {
    ByteVector ephemeral_secret;
    ByteVector ephemeral_public;
  };

  Roe<KemState> NewEphemeralKem();
  Roe<std::vector<uint8_t>> EncodeHello(MshMessageType type, const KemState& kem);
  Roe<MshPayload> BuildLocalPayload(const KemState& kem, const ByteVector& remote_kem_pk, ByteVector& shared_secret_out);
  Roe<void> SendMessage(MshMessageType type, std::span<const uint8_t> body);
  Roe<void> Fail(std::string message);
  Roe<void> MaybeComplete();

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
};

} // namespace pp::amp
