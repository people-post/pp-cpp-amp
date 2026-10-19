#include "amp/link/PeerId.h"

#include "crypto/PeerId.h"

namespace pp::amp {

std::string PeerIdFromIdentityKey(const ByteVector& identity_public_key) {
  auto peer_id = pp::PeerIdFromMlDsaPublicKey(identity_public_key);
  return peer_id ? std::move(*peer_id) : std::string{};
}

std::string PeerIdFromMultiaddr(const std::string_view multiaddr) {
  constexpr std::string_view kMarker = "/p2p/";
  const auto pos = multiaddr.rfind(kMarker);
  if (pos == std::string_view::npos) {
    return {};
  }
  std::string_view id = multiaddr.substr(pos + kMarker.size());
  if (const auto slash = id.find('/'); slash != std::string_view::npos) {
    id = id.substr(0, slash);
  }
  return std::string(id);
}

} // namespace pp::amp
