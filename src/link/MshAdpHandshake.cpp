#include "amp/link/MshAdpHandshake.h"

#include "amp/link/AmpAdpCarrier.h"
#include "amp/L2/MshHandshake.h"
#include "amp/L2/SessionKeys.h"

#include "crypto/MlDsa.h"
#include "crypto/MlKem.h"
#include "crypto/SodiumUtil.h"

#include <sodium.h>

namespace pp::amp {

namespace {

void AppendPart(std::vector<ByteVector>& transcript, const std::vector<uint8_t>& wire) {
  transcript.emplace_back(wire.begin(), wire.end());
}

Roe<ByteVector> RandomNonce() {
  pp::EnsureSodiumInit();
  ByteVector nonce(kHandshakeNonceBytes);
  randombytes_buf(nonce.data(), nonce.size());
  return nonce;
}

Roe<ByteVector> CombineMasterIkm(const ByteVector& client_ss, const ByteVector& server_ss) {
  if (client_ss.size() != pp::kMlKem768SharedSecretBytes || server_ss.size() != pp::kMlKem768SharedSecretBytes) {
    return Error("amp msh adp: bad kem shared secret size");
  }
  ByteVector out;
  out.reserve(client_ss.size() + server_ss.size());
  out.insert(out.end(), client_ss.begin(), client_ss.end());
  out.insert(out.end(), server_ss.begin(), server_ss.end());
  return out;
}

Roe<void> VerifyPayload(const MshPayload& payload) {
  auto sign_msg = MshMessages::BuildIdentitySignMessage(payload.static_kem_public_key);
  if (!sign_msg) {
    return sign_msg.error();
  }
  auto ok = pp::MlDsa::Verify(payload.identity_public_key, *sign_msg, payload.identity_signature);
  if (!ok) {
    return ok.error();
  }
  if (!*ok) {
    return Error("amp msh adp: identity signature invalid");
  }
  return Roe<void>();
}

} // namespace

Roe<MshAdpHandshake::KemState> MshAdpHandshake::NewEphemeralKem() {
  auto keys = ::pp::MlKem::GenerateKeyPair();
  if (!keys) {
    return keys.error();
  }
  KemState kem;
  kem.ephemeral_secret = std::move(keys->private_key);
  kem.ephemeral_public = std::move(keys->public_key);
  return kem;
}

Roe<std::vector<uint8_t>> MshAdpHandshake::EncodeHello(const MshMessageType type, const KemState& kem) {
  auto nonce = RandomNonce();
  if (!nonce) {
    return nonce.error();
  }
  MshHello hello;
  hello.kem_public_key = kem.ephemeral_public;
  hello.nonce = std::move(*nonce);
  return MshMessages::EncodeHello(type, hello);
}

Roe<MshPayload> MshAdpHandshake::BuildLocalPayload(const KemState& kem, const ByteVector& remote_kem_pk,
                                                     ByteVector& shared_secret_out) {
  if (identity_.ml_dsa_public_key.size() != pp::kMlDsa65PublicKeyBytes
      || identity_.ml_dsa_secret_key.size() != pp::kMlDsa65SecretKeyBytes) {
    return Error("amp msh adp: bad identity key sizes");
  }
  auto encap = ::pp::MlKem::Encapsulate(remote_kem_pk);
  if (!encap) {
    return encap.error();
  }
  shared_secret_out = std::move(encap->shared_secret);
  auto sign_msg = MshMessages::BuildIdentitySignMessage(kem.ephemeral_public);
  if (!sign_msg) {
    return sign_msg.error();
  }
  auto sig = pp::MlDsa::Sign(identity_.ml_dsa_secret_key, *sign_msg);
  if (!sig) {
    return sig.error();
  }
  MshPayload payload;
  payload.kem_ciphertext = std::move(encap->ciphertext);
  payload.identity_public_key = identity_.ml_dsa_public_key;
  payload.static_kem_public_key = kem.ephemeral_public;
  payload.identity_signature = std::move(*sig);
  return payload;
}

MshAdpHandshake::MshAdpHandshake(const Role role, MshIdentity identity, SendWire send, CompleteHandler on_complete,
                                 const bool chunked_wire)
    : role_(role), identity_(std::move(identity)), send_(std::move(send)), on_complete_(std::move(on_complete)),
      chunked_wire_(chunked_wire) {}

Roe<void> MshAdpHandshake::SendMessage(const MshMessageType type, const std::span<const uint8_t> body) {
  if (!chunked_wire_) {
    auto wire = AmpAdpCarrier::EncodeMsh(type, body);
    if (!wire) {
      return wire.error();
    }
    return send_(std::move(*wire));
  }
  auto chunks = AmpAdpCarrier::EncodeMshChunked(type, body);
  if (!chunks) {
    return chunks.error();
  }
  for (auto& chunk : *chunks) {
    if (auto sent = send_(std::move(chunk)); !sent) {
      return sent.error();
    }
  }
  return Roe<void>();
}

Roe<void> MshAdpHandshake::Fail(const std::string message) {
  if (on_complete_) {
    on_complete_(Error(message));
  }
  complete_ = true;
  return Error(message);
}

Roe<void> MshAdpHandshake::Start() {
  if (role_ != Role::Initiator) {
    return Error("amp msh adp: start on non-initiator");
  }
  if (started_) {
    return Error("amp msh adp: already started");
  }
  started_ = true;
  auto kem = NewEphemeralKem();
  if (!kem) {
    return Fail(kem.error().message);
  }
  local_kem_ = std::move(*kem);
  auto hello = EncodeHello(MshMessageType::ClientHello, *local_kem_);
  if (!hello) {
    return Fail(hello.error().message);
  }
  AppendPart(transcript_, *hello);
  return SendMessage(MshMessageType::ClientHello, *hello);
}

Roe<void> MshAdpHandshake::HandleMsh(const MshMessageType type, const std::span<const uint8_t> body) {
  if (complete_) {
    return Error("amp msh adp: handshake complete");
  }

  if (role_ == Role::Responder) {
    switch (type) {
    case MshMessageType::ClientHello: {
      if (started_) {
        return Error("amp msh adp: duplicate client hello");
      }
      started_ = true;
      auto parsed = MshMessages::DecodeHello(MshMessageType::ClientHello, body);
      if (!parsed) {
        return Fail(parsed.error().message);
      }
      remote_hello_ = std::move(*parsed);
      AppendPart(transcript_, std::vector<uint8_t>(body.begin(), body.end()));

      auto kem = NewEphemeralKem();
      if (!kem) {
        return Fail(kem.error().message);
      }
      local_kem_ = std::move(*kem);
      auto server_hello = EncodeHello(MshMessageType::ServerHello, *local_kem_);
      if (!server_hello) {
        return Fail(server_hello.error().message);
      }
      AppendPart(transcript_, *server_hello);
      return SendMessage(MshMessageType::ServerHello, *server_hello);
    }
    case MshMessageType::ClientPayload: {
      if (!remote_hello_ || !local_kem_) {
        return Error("amp msh adp: client payload out of order");
      }
      auto parsed = MshMessages::DecodePayload(MshMessageType::ClientPayload, body);
      if (!parsed) {
        return Fail(parsed.error().message);
      }
      if (auto verified = VerifyPayload(*parsed); !verified) {
        return Fail(verified.error().message);
      }
      remote_payload_ = std::move(*parsed);
      AppendPart(transcript_, std::vector<uint8_t>(body.begin(), body.end()));

      auto client_ss = ::pp::MlKem::Decapsulate(local_kem_->ephemeral_secret, remote_payload_->kem_ciphertext);
      if (!client_ss) {
        return Fail(client_ss.error().message);
      }

      ByteVector server_ss;
      auto server_payload_struct =
          BuildLocalPayload(*local_kem_, remote_hello_->kem_public_key, server_ss);
      if (!server_payload_struct) {
        return Fail(server_payload_struct.error().message);
      }
      auto server_payload = MshMessages::EncodePayload(MshMessageType::ServerPayload, *server_payload_struct);
      if (!server_payload) {
        return Fail(server_payload.error().message);
      }
      AppendPart(transcript_, *server_payload);
      if (auto sent = SendMessage(MshMessageType::ServerPayload, *server_payload); !sent) {
        return sent.error();
      }

      auto master = CombineMasterIkm(*client_ss, server_ss);
      if (!master) {
        return Fail(master.error().message);
      }
      master_ikm_ = std::move(*master);
      return Roe<void>();
    }
    case MshMessageType::Finished: {
      if (master_ikm_.empty()) {
        return Error("amp msh adp: finished out of order");
      }
      if (auto ok = MshHandshake::VerifyFinished(master_ikm_, transcript_, body); !ok) {
        return Fail(ok.error().message);
      }
      AppendPart(transcript_, std::vector<uint8_t>(body.begin(), body.end()));

      auto finished = MshHandshake::BuildFinished(master_ikm_, transcript_);
      if (!finished) {
        return Fail(finished.error().message);
      }
      AppendPart(transcript_, *finished);
      if (auto sent = SendMessage(MshMessageType::Finished, *finished); !sent) {
        return sent.error();
      }
      return MaybeComplete();
    }
    default:
      return Error("amp msh adp: unexpected responder message");
    }
  }

  switch (type) {
  case MshMessageType::ServerHello: {
    if (!started_ || remote_hello_.has_value()) {
      return Error("amp msh adp: server hello out of order");
    }
    auto parsed = MshMessages::DecodeHello(MshMessageType::ServerHello, body);
    if (!parsed) {
      return Fail(parsed.error().message);
    }
    remote_hello_ = std::move(*parsed);
    AppendPart(transcript_, std::vector<uint8_t>(body.begin(), body.end()));

    ByteVector client_ss;
    auto client_payload_struct = BuildLocalPayload(*local_kem_, remote_hello_->kem_public_key, client_ss);
    if (!client_payload_struct) {
      return Fail(client_payload_struct.error().message);
    }
    client_ss_ = std::move(client_ss);
    auto client_payload = MshMessages::EncodePayload(MshMessageType::ClientPayload, *client_payload_struct);
    if (!client_payload) {
      return Fail(client_payload.error().message);
    }
    AppendPart(transcript_, *client_payload);
    return SendMessage(MshMessageType::ClientPayload, *client_payload);
  }
  case MshMessageType::ServerPayload: {
    if (!remote_hello_ || client_ss_.empty()) {
      return Error("amp msh adp: server payload out of order");
    }
    auto parsed = MshMessages::DecodePayload(MshMessageType::ServerPayload, body);
    if (!parsed) {
      return Fail(parsed.error().message);
    }
    if (auto verified = VerifyPayload(*parsed); !verified) {
      return Fail(verified.error().message);
    }
    remote_payload_ = std::move(*parsed);
    AppendPart(transcript_, std::vector<uint8_t>(body.begin(), body.end()));

    auto server_ss = ::pp::MlKem::Decapsulate(local_kem_->ephemeral_secret, remote_payload_->kem_ciphertext);
    if (!server_ss) {
      return Fail(server_ss.error().message);
    }
    auto master = CombineMasterIkm(client_ss_, *server_ss);
    if (!master) {
      return Fail(master.error().message);
    }
    master_ikm_ = std::move(*master);

    auto finished = MshHandshake::BuildFinished(master_ikm_, transcript_);
    if (!finished) {
      return Fail(finished.error().message);
    }
    AppendPart(transcript_, *finished);
    return SendMessage(MshMessageType::Finished, *finished);
  }
  case MshMessageType::Finished: {
    if (master_ikm_.empty()) {
      return Error("amp msh adp: finished out of order");
    }
    if (auto ok = MshHandshake::VerifyFinished(master_ikm_, transcript_, body); !ok) {
      return Fail(ok.error().message);
    }
    AppendPart(transcript_, std::vector<uint8_t>(body.begin(), body.end()));
    return MaybeComplete();
  }
  default:
    return Error("amp msh adp: unexpected initiator message");
  }
}

Roe<void> MshAdpHandshake::MaybeComplete() {
  if (complete_ || master_ikm_.empty() || transcript_.size() < 6) {
    return Roe<void>();
  }

  auto transcript_hash = SessionKeys::TranscriptHash(transcript_);
  if (!transcript_hash) {
    return Fail(transcript_hash.error().message);
  }
  const bool initiator = role_ == Role::Initiator;
  auto material = SessionKeys::Derive(master_ikm_, *transcript_hash, initiator, 1);
  if (!material) {
    return Fail(material.error().message);
  }

  MshAdpEstablished established;
  established.local_material = std::move(*material);
  established.master_ikm = master_ikm_;
  established.transcript_hash = std::move(*transcript_hash);
  if (remote_payload_) {
    established.remote_identity_public_key = remote_payload_->identity_public_key;
  }
  complete_ = true;
  if (on_complete_) {
    on_complete_(established);
  }
  return Roe<void>();
}

} // namespace pp::amp
