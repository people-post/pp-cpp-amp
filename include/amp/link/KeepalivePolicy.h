#pragma once

/**
 * Warm/hot keepalive + idle eviction predicates — MarkWarm/Hot, MaybeSendKeepalives.
 * See docs/KEEPALIVE.md. Named concern per ADR_LINK_PLANE.
 */
namespace pp::amp {
struct KeepalivePolicyTag {};
} // namespace pp::amp
