#pragma once

#include "amp/L2/Types.h"

#include <string>
#include <string_view>

namespace pp::amp {

/**
 * For `PeerLinkConfig::peer_id_from_identity`: the mesh PeerId of an authenticated MSH identity key
 * (pp-cpp-crypto's `pp::PeerIdFromMlDsaPublicKey`), or "" when it is not an ML-DSA-65 key (the link
 * then falls back to its key fingerprint).
 */
std::string PeerIdFromIdentityKey(const ByteVector& identity_public_key);

/** The PeerId after the last `/p2p/` of a multiaddr; "" when there is none. */
std::string PeerIdFromMultiaddr(std::string_view multiaddr);

} // namespace pp::amp
