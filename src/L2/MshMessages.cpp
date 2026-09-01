#include "amp/L2/MshMessages.h"

#include "crypto/MlDsa.h"
#include "crypto/MlKem.h"
#include "amp/L2/Types.h"

#include <cstring>

namespace pp::amp {

namespace {

Roe<ByteVector> ReadFixed(std::span<const uint8_t>& wire, size_t n) {
  if (wire.size() < n) {
    return Error("amp msh: truncated fixed field");
  }
  ByteVector out(wire.begin(), wire.begin() + static_cast<std::ptrdiff_t>(n));
  wire = wire.subspan(n);
  return out;
}

Roe<void> ExpectEmpty(std::span<const uint8_t> wire) {
  if (!wire.empty()) {
    return Error("amp msh: trailing bytes");
  }
  return Roe<void>();
}

Roe<void> ExpectType(std::span<const uint8_t>& wire, MshMessageType expected) {
  if (wire.empty()) {
    return Error("amp msh: empty message");
  }
  if (wire[0] != static_cast<uint8_t>(expected)) {
    return Error("amp msh: unexpected message type");
  }
  wire = wire.subspan(1);
  return Roe<void>();
}

} // namespace

Roe<ByteVector> MshMessages::BuildIdentitySignMessage(const ByteVector& static_kem_public_key) {
  if (static_kem_public_key.size() != pp::kMlKem768PublicKeyBytes) {
    return Error("amp msh: bad static kem pk size");
  }
  ByteVector msg;
  const std::string prefix = kAmpIdentityBindPrefix;
  msg.insert(msg.end(), prefix.begin(), prefix.end());
  msg.insert(msg.end(), static_kem_public_key.begin(), static_kem_public_key.end());
  return msg;
}

Roe<std::vector<uint8_t>> MshMessages::EncodeHello(const MshMessageType type, const MshHello& hello) {
  if (hello.version != kMshVersion) {
    return Error("amp msh: bad hello version");
  }
  if (hello.kem_public_key.size() != pp::kMlKem768PublicKeyBytes) {
    return Error("amp msh: bad kem pk size");
  }
  if (hello.nonce.size() != kHandshakeNonceBytes) {
    return Error("amp msh: bad hello nonce size");
  }
  std::vector<uint8_t> out;
  out.push_back(static_cast<uint8_t>(type));
  out.push_back(hello.version);
  out.insert(out.end(), hello.kem_public_key.begin(), hello.kem_public_key.end());
  out.insert(out.end(), hello.nonce.begin(), hello.nonce.end());
  return out;
}

Roe<MshHello> MshMessages::DecodeHello(const MshMessageType expected, std::span<const uint8_t> wire) {
  auto span = wire;
  auto typed = ExpectType(span, expected);
  if (!typed) {
    return typed.error();
  }
  if (span.empty()) {
    return Error("amp msh: missing hello version");
  }
  MshHello hello;
  hello.version = span[0];
  span = span.subspan(1);
  if (hello.version != kMshVersion) {
    return Error("amp msh: unsupported hello version");
  }
  auto kem_pk = ReadFixed(span, pp::kMlKem768PublicKeyBytes);
  if (!kem_pk) {
    return kem_pk.error();
  }
  auto nonce = ReadFixed(span, kHandshakeNonceBytes);
  if (!nonce) {
    return nonce.error();
  }
  if (!ExpectEmpty(span)) {
    return ExpectEmpty(span).error();
  }
  hello.kem_public_key = std::move(*kem_pk);
  hello.nonce = std::move(*nonce);
  return hello;
}

Roe<std::vector<uint8_t>> MshMessages::EncodePayload(const MshMessageType type, const MshPayload& payload) {
  if (payload.kem_ciphertext.size() != pp::kMlKem768CiphertextBytes) {
    return Error("amp msh: bad kem ct size");
  }
  if (payload.identity_public_key.size() != pp::kMlDsa65PublicKeyBytes) {
    return Error("amp msh: bad identity pk size");
  }
  if (payload.static_kem_public_key.size() != pp::kMlKem768PublicKeyBytes) {
    return Error("amp msh: bad static kem pk size");
  }
  if (payload.identity_signature.size() != pp::kMlDsa65SignatureBytes) {
    return Error("amp msh: bad identity sig size");
  }
  std::vector<uint8_t> out;
  out.push_back(static_cast<uint8_t>(type));
  out.insert(out.end(), payload.kem_ciphertext.begin(), payload.kem_ciphertext.end());
  out.insert(out.end(), payload.identity_public_key.begin(), payload.identity_public_key.end());
  out.insert(out.end(), payload.static_kem_public_key.begin(), payload.static_kem_public_key.end());
  out.insert(out.end(), payload.identity_signature.begin(), payload.identity_signature.end());
  return out;
}

Roe<MshPayload> MshMessages::DecodePayload(const MshMessageType expected, std::span<const uint8_t> wire) {
  auto span = wire;
  auto typed = ExpectType(span, expected);
  if (!typed) {
    return typed.error();
  }
  MshPayload payload;
  auto kem_ct = ReadFixed(span, pp::kMlKem768CiphertextBytes);
  if (!kem_ct) {
    return kem_ct.error();
  }
  auto id_pk = ReadFixed(span, pp::kMlDsa65PublicKeyBytes);
  if (!id_pk) {
    return id_pk.error();
  }
  auto static_pk = ReadFixed(span, pp::kMlKem768PublicKeyBytes);
  if (!static_pk) {
    return static_pk.error();
  }
  auto sig = ReadFixed(span, pp::kMlDsa65SignatureBytes);
  if (!sig) {
    return sig.error();
  }
  if (!ExpectEmpty(span)) {
    return ExpectEmpty(span).error();
  }
  payload.kem_ciphertext = std::move(*kem_ct);
  payload.identity_public_key = std::move(*id_pk);
  payload.static_kem_public_key = std::move(*static_pk);
  payload.identity_signature = std::move(*sig);
  return payload;
}

Roe<std::vector<uint8_t>> MshMessages::EncodeFinished(const MshFinished& finished) {
  if (finished.mac.size() != kFinishedMacBytes) {
    return Error("amp msh: bad finished mac size");
  }
  std::vector<uint8_t> out;
  out.push_back(static_cast<uint8_t>(MshMessageType::Finished));
  out.insert(out.end(), finished.mac.begin(), finished.mac.end());
  return out;
}

Roe<MshFinished> MshMessages::DecodeFinished(std::span<const uint8_t> wire) {
  auto span = wire;
  auto typed = ExpectType(span, MshMessageType::Finished);
  if (!typed) {
    return typed.error();
  }
  auto mac = ReadFixed(span, kFinishedMacBytes);
  if (!mac) {
    return mac.error();
  }
  if (!ExpectEmpty(span)) {
    return ExpectEmpty(span).error();
  }
  MshFinished finished;
  finished.mac = std::move(*mac);
  return finished;
}

} // namespace pp::amp
