#pragma once

#include "common/Error.h"
#include "crypto/Types.h"

#include <cstdint>
#include <vector>

namespace pp::amp {

using ByteVector = pp::ByteVector;
using pp::Error;
using pp::Roe;

inline constexpr uint8_t kMshVersion = 1;
inline constexpr size_t kAssocKeyBytes = 32;
inline constexpr size_t kSessionKeyBytes = 32;
inline constexpr size_t kAeadNonceSize = 24;
inline constexpr size_t kFinishedMacBytes = 32;
inline constexpr size_t kHandshakeNonceBytes = 32;

inline constexpr const char* kAmpHkdfSalt = "pp-amp-msh-v1";
inline constexpr const char* kAmpKAssocInfo = "pp-amp-k-assoc-v1";
inline constexpr const char* kAmpKClientInfo = "pp-amp-k-client-v1";
inline constexpr const char* kAmpKServerInfo = "pp-amp-k-server-v1";
inline constexpr const char* kAmpFinishedInfo = "pp-amp-msh-finished-v1";
inline constexpr const char* kAmpIdentityBindPrefix = "noise-libp2p-static-key:";

enum class Direction : uint8_t {
  InitiatorToResponder = 0,
  ResponderToInitiator = 1,
};

struct MshIdentity {
  ByteVector ml_dsa_secret_key;
  ByteVector ml_dsa_public_key;
};

struct SessionMaterial {
  ByteVector k_assoc;
  ByteVector k_send;
  ByteVector k_recv;
  uint32_t session_epoch = 1;
  bool initiator = false;
};

} // namespace pp::amp
