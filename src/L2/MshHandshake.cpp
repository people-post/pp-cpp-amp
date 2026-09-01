#include "amp/L2/MshHandshake.h"

#include "amp/L2/SessionKeys.h"

#include "crypto/MlDsa.h"
#include "crypto/MlKem.h"
#include "crypto/SodiumUtil.h"

#include <sodium.h>

#include <cstring>

namespace pp::amp {

namespace {

struct KemState {
  ByteVector ephemeral_secret;
  ByteVector ephemeral_public;
};

void AppendPart(std::vector<ByteVector>& transcript, const std::vector<uint8_t>& wire) {
  transcript.emplace_back(wire.begin(), wire.end());
}

Roe<ByteVector> RandomNonce() {
  pp::EnsureSodiumInit();
  ByteVector nonce(kHandshakeNonceBytes);
  randombytes_buf(nonce.data(), nonce.size());
  return nonce;
}

Roe<KemState> NewEphemeralKem() {
  auto keys = ::pp::MlKem::GenerateKeyPair();
  if (!keys) {
    return keys.error();
  }
  KemState kem;
  kem.ephemeral_secret = std::move(keys->private_key);
  kem.ephemeral_public = std::move(keys->public_key);
  return kem;
}

Roe<std::vector<uint8_t>> EncodeHello(const MshMessageType type, const KemState& kem) {
  auto nonce = RandomNonce();
  if (!nonce) {
    return nonce.error();
  }
  MshHello hello;
  hello.kem_public_key = kem.ephemeral_public;
  hello.nonce = std::move(*nonce);
  return MshMessages::EncodeHello(type, hello);
}

Roe<MshPayload> BuildPayload(const MshIdentity& identity, const KemState& kem,
                              const ByteVector& remote_ephemeral_public) {
  if (identity.ml_dsa_public_key.size() != pp::kMlDsa65PublicKeyBytes
      || identity.ml_dsa_secret_key.size() != pp::kMlDsa65SecretKeyBytes) {
    return Error("amp msh: bad identity key sizes");
  }
  auto encap = ::pp::MlKem::Encapsulate(remote_ephemeral_public);
  if (!encap) {
    return encap.error();
  }
  auto sign_msg = MshMessages::BuildIdentitySignMessage(kem.ephemeral_public);
  if (!sign_msg) {
    return sign_msg.error();
  }
  auto sig = pp::MlDsa::Sign(identity.ml_dsa_secret_key, *sign_msg);
  if (!sig) {
    return sig.error();
  }
  MshPayload payload;
  payload.kem_ciphertext = std::move(encap->ciphertext);
  payload.identity_public_key = identity.ml_dsa_public_key;
  payload.static_kem_public_key = kem.ephemeral_public;
  payload.identity_signature = std::move(*sig);
  return payload;
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
    return Error("amp msh: identity signature invalid");
  }
  return Roe<void>();
}

Roe<ByteVector> CombineMasterIkm(const ByteVector& client_ss, const ByteVector& server_ss) {
  if (client_ss.size() != pp::kMlKem768SharedSecretBytes || server_ss.size() != pp::kMlKem768SharedSecretBytes) {
    return Error("amp msh: bad kem shared secret size");
  }
  ByteVector out;
  out.reserve(client_ss.size() + server_ss.size());
  out.insert(out.end(), client_ss.begin(), client_ss.end());
  out.insert(out.end(), server_ss.begin(), server_ss.end());
  return out;
}

Roe<ByteVector> FinishedMac(const ByteVector& master_ikm, const ByteVector& transcript_hash) {
  pp::EnsureSodiumInit();
  unsigned char prk[crypto_kdf_hkdf_sha256_KEYBYTES];
  if (crypto_kdf_hkdf_sha256_extract(prk, reinterpret_cast<const unsigned char*>(kAmpHkdfSalt), std::strlen(kAmpHkdfSalt),
                                     master_ikm.data(), master_ikm.size()) != 0) {
    return Error("amp msh: finished hkdf extract failed");
  }
  ByteVector finished_key(kSessionKeyBytes);
  if (crypto_kdf_hkdf_sha256_expand(finished_key.data(), finished_key.size(), kAmpFinishedInfo, std::strlen(kAmpFinishedInfo),
                                    prk) != 0) {
    return Error("amp msh: finished hkdf expand failed");
  }
  ByteVector mac(kFinishedMacBytes);
  if (crypto_auth_hmacsha256(mac.data(), transcript_hash.data(), transcript_hash.size(), finished_key.data()) != 0) {
    return Error("amp msh: finished hmac failed");
  }
  return mac;
}

} // namespace

Roe<std::vector<uint8_t>> MshHandshake::BuildFinished(const ByteVector& master_ikm,
                                                      const std::vector<ByteVector>& transcript_parts) {
  auto transcript_hash = SessionKeys::TranscriptHash(transcript_parts);
  if (!transcript_hash) {
    return transcript_hash.error();
  }
  auto mac = FinishedMac(master_ikm, *transcript_hash);
  if (!mac) {
    return mac.error();
  }
  MshFinished finished;
  finished.mac = std::move(*mac);
  return MshMessages::EncodeFinished(finished);
}

Roe<void> MshHandshake::VerifyFinished(const ByteVector& master_ikm, const std::vector<ByteVector>& transcript_parts,
                                       std::span<const uint8_t> finished_wire) {
  auto finished = MshMessages::DecodeFinished(finished_wire);
  if (!finished) {
    return finished.error();
  }
  auto transcript_hash = SessionKeys::TranscriptHash(transcript_parts);
  if (!transcript_hash) {
    return transcript_hash.error();
  }
  auto expected = FinishedMac(master_ikm, *transcript_hash);
  if (!expected) {
    return expected.error();
  }
  if (finished->mac.size() != expected->size()
      || sodium_memcmp(finished->mac.data(), expected->data(), expected->size()) != 0) {
    return Error("amp msh: finished mismatch");
  }
  return Roe<void>();
}

Roe<MshEstablished> MshHandshake::Run(const MshIdentity& initiator, const MshIdentity& responder) {
  auto initiator_kem = NewEphemeralKem();
  if (!initiator_kem) {
    return initiator_kem.error();
  }
  auto client_hello = EncodeHello(MshMessageType::ClientHello, *initiator_kem);
  if (!client_hello) {
    return client_hello.error();
  }

  auto client_parsed = MshMessages::DecodeHello(MshMessageType::ClientHello, *client_hello);
  if (!client_parsed) {
    return client_parsed.error();
  }

  auto responder_kem = NewEphemeralKem();
  if (!responder_kem) {
    return responder_kem.error();
  }
  auto server_hello = EncodeHello(MshMessageType::ServerHello, *responder_kem);
  if (!server_hello) {
    return server_hello.error();
  }

  std::vector<ByteVector> transcript;
  AppendPart(transcript, *client_hello);
  AppendPart(transcript, *server_hello);

  auto client_payload_struct = BuildPayload(initiator, *initiator_kem, responder_kem->ephemeral_public);
  if (!client_payload_struct) {
    return client_payload_struct.error();
  }
  auto client_payload = MshMessages::EncodePayload(MshMessageType::ClientPayload, *client_payload_struct);
  if (!client_payload) {
    return client_payload.error();
  }
  auto client_ss = ::pp::MlKem::Decapsulate(responder_kem->ephemeral_secret, client_payload_struct->kem_ciphertext);
  if (!client_ss) {
    return client_ss.error();
  }

  auto server_payload_struct = BuildPayload(responder, *responder_kem, client_parsed->kem_public_key);
  if (!server_payload_struct) {
    return server_payload_struct.error();
  }
  auto server_payload = MshMessages::EncodePayload(MshMessageType::ServerPayload, *server_payload_struct);
  if (!server_payload) {
    return server_payload.error();
  }
  auto server_ss = ::pp::MlKem::Decapsulate(initiator_kem->ephemeral_secret, server_payload_struct->kem_ciphertext);
  if (!server_ss) {
    return server_ss.error();
  }

  auto client_verified = VerifyPayload(*client_payload_struct);
  if (!client_verified) {
    return client_verified.error();
  }
  auto server_verified = VerifyPayload(*server_payload_struct);
  if (!server_verified) {
    return server_verified.error();
  }

  AppendPart(transcript, *client_payload);
  AppendPart(transcript, *server_payload);

  auto master_ikm = CombineMasterIkm(*client_ss, *server_ss);
  if (!master_ikm) {
    return master_ikm.error();
  }

  auto client_finished = BuildFinished(*master_ikm, transcript);
  if (!client_finished) {
    return client_finished.error();
  }
  auto client_finished_ok = VerifyFinished(*master_ikm, transcript, *client_finished);
  if (!client_finished_ok) {
    return client_finished_ok.error();
  }

  auto server_finished = BuildFinished(*master_ikm, transcript);
  if (!server_finished) {
    return server_finished.error();
  }
  auto server_finished_ok = VerifyFinished(*master_ikm, transcript, *server_finished);
  if (!server_finished_ok) {
    return server_finished_ok.error();
  }

  auto transcript_hash = SessionKeys::TranscriptHash(transcript);
  if (!transcript_hash) {
    return transcript_hash.error();
  }
  auto initiator_material = SessionKeys::Derive(*master_ikm, *transcript_hash, true, 1);
  if (!initiator_material) {
    return initiator_material.error();
  }
  auto responder_material = SessionKeys::Derive(*master_ikm, *transcript_hash, false, 1);
  if (!responder_material) {
    return responder_material.error();
  }

  MshEstablished out;
  out.initiator_material = std::move(*initiator_material);
  out.responder_material = std::move(*responder_material);
  out.master_ikm = std::move(*master_ikm);
  out.transcript_hash = std::move(*transcript_hash);
  return out;
}

} // namespace pp::amp
