#pragma once

#include "amp/L2/MshMessages.h"
#include "amp/L2/Types.h"


#include <vector>

namespace pp::amp {

struct MshEstablished {
  SessionMaterial initiator_material;
  SessionMaterial responder_material;
  ByteVector master_ikm;
  ByteVector transcript_hash;
};

/** In-memory MSH v1 runner for tests and future ADP Reliable carriage. */
class MshHandshake {
public:
  static Roe<MshEstablished> Run(const MshIdentity& initiator, const MshIdentity& responder);

  static Roe<std::vector<uint8_t>> BuildFinished(const ByteVector& master_ikm,
                                                 const std::vector<ByteVector>& transcript_parts);
  static Roe<void> VerifyFinished(const ByteVector& master_ikm, const std::vector<ByteVector>& transcript_parts,
                                  std::span<const uint8_t> finished_wire);
};

} // namespace pp::amp
