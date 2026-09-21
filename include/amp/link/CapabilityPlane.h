#pragma once

/**
 * Capability / ch0 identify plane (A016) — owned by PeerLinkManager façade methods
 * StartCapabilityExchange / OnCh0Data / OnCapabilityData / IngestRemoteCapabilityAddrs.
 * Extracted as a named concern in ADR_LINK_PLANE; logic remains in PeerLinkManager.cpp
 * until a second consumer needs a standalone type.
 */
namespace pp::amp {
struct CapabilityPlaneTag {};
} // namespace pp::amp
