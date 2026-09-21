#pragma once

/**
 * Nested carrier install (A024) — PeerLinkManager::EstablishNestedOverCarrier /
 * EnableNestedCarrierAccept / HandleInboundCarrierChannel / FinishNestedCarrier.
 * Named concern per ADR_LINK_PLANE; implementation stays in PeerLinkManager until split.
 */
namespace pp::amp {
struct CarrierInstallerTag {};
} // namespace pp::amp
