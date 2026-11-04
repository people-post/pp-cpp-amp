#include "amp/L2/Session.h"

#include "amp/L2/SessionControl.h"

#include <cstring>

namespace pp::amp {

Session::Session(SessionMaterial material, ByteVector master_ikm, ByteVector transcript_hash)
    : material_(std::move(material)),
      master_ikm_(std::move(master_ikm)),
      transcript_hash_(std::move(transcript_hash)) {}

Roe<Session> Session::FromMaterial(SessionMaterial material, ByteVector master_ikm, ByteVector transcript_hash) {
  if (material.k_assoc.size() != kAssocKeyBytes || material.k_send.size() != kSessionKeyBytes
      || material.k_recv.size() != kSessionKeyBytes) {
    return Error("amp session: bad key sizes");
  }
  if (master_ikm.empty()) {
    return Error("amp session: empty master ikm");
  }
  if (transcript_hash.empty()) {
    return Error("amp session: empty transcript hash");
  }
  return Session(std::move(material), std::move(master_ikm), std::move(transcript_hash));
}

adp::PeerKey Session::AssocKey() const {
  adp::PeerKey key;
  std::memcpy(key.bytes.data(), material_.k_assoc.data(), kAssocKeyBytes);
  return key;
}

Direction Session::OutDirection() const {
  return material_.initiator ? Direction::InitiatorToResponder : Direction::ResponderToInitiator;
}

Direction Session::InDirection() const {
  return material_.initiator ? Direction::ResponderToInitiator : Direction::InitiatorToResponder;
}

Roe<void> Session::SealPacket(const adp::PacketSpace space, const uint32_t number,
                              const std::span<const uint8_t> header, const std::span<const uint8_t> plaintext,
                              std::vector<uint8_t>& out) const {
  return SessionCrypto::SealPacket(material_.k_send, OutDirection(), material_.session_epoch, space, number, header,
                                   plaintext, out);
}

Roe<std::vector<uint8_t>> Session::OpenPacket(const adp::PacketSpace space, const uint32_t number,
                                              const std::span<const uint8_t> header,
                                              const std::span<const uint8_t> sealed, const int64_t now_ms) const {
  auto opened = SessionCrypto::OpenPacket(material_.k_recv, InDirection(), material_.session_epoch, space, number,
                                          header, sealed);
  if (opened || !HasGraceRecvKey(now_ms)) {
    return opened;
  }
  return SessionCrypto::OpenPacket(*previous_recv_key_, InDirection(), material_.session_epoch - 1, space, number,
                                   header, sealed);
}

Roe<void> Session::Rekey() {
  material_.session_epoch += 1;
  auto refreshed = SessionKeys::Derive(master_ikm_, transcript_hash_, material_.initiator, material_.session_epoch);
  if (!refreshed) {
    return refreshed.error();
  }
  material_.k_send = std::move(refreshed->k_send);
  material_.k_recv = std::move(refreshed->k_recv);
  previous_recv_key_.reset();
  grace_until_ms_ = 0;
  return Roe<void>();
}

Roe<void> Session::ApplyRekey(const uint32_t target_epoch, const int64_t now_ms) {
  if (target_epoch != material_.session_epoch + 1) {
    return Error("amp session: unexpected rekey epoch");
  }
  previous_recv_key_ = material_.k_recv;
  grace_until_ms_ = now_ms + kSessionRekeyGraceMs;
  material_.session_epoch = target_epoch;
  auto refreshed = SessionKeys::Derive(master_ikm_, transcript_hash_, material_.initiator, material_.session_epoch);
  if (!refreshed) {
    return refreshed.error();
  }
  material_.k_send = std::move(refreshed->k_send);
  material_.k_recv = std::move(refreshed->k_recv);
  return Roe<void>();
}

bool Session::HasGraceRecvKey(const int64_t now_ms) const {
  return previous_recv_key_.has_value() && now_ms > 0 && now_ms <= grace_until_ms_;
}

} // namespace pp::amp
