#pragma once

#include "amp/L2/Types.h"


#include <span>
#include <vector>

namespace pp::amp {

/** Derive AMP session keys from MSH master input + transcript hash. */
class SessionKeys {
public:
  static Roe<SessionMaterial> Derive(std::span<const uint8_t> master_ikm, std::span<const uint8_t> transcript_hash,
                                     bool initiator, uint32_t session_epoch = 1);

  static Roe<ByteVector> TranscriptHash(const std::vector<ByteVector>& transcript_parts);
};

} // namespace pp::amp
