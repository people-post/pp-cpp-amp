#pragma once

#include "amp/L3/ChannelSession.h"
#include "amp_test_link.h"

#include <memory>
#include <string>

namespace pp::amp::test {

/**
 * dialer ⇄ relay ⇄ target over two in-memory links: one channel from the dialer to the relay and
 * one from the relay to the target, each bound to a relay-side ChannelSession — what a relay
 * splices (e.g. with ChannelBridge). The dialer and target use their muxes directly.
 */
struct AmpTestRelayPath {
  std::unique_ptr<AmpTestLink> near;  // dialer = initiator, relay = responder
  std::unique_ptr<AmpTestLink> far;   // relay = initiator, target = responder
  uint32_t near_channel = 0;
  uint32_t far_channel = 0;
  std::shared_ptr<ChannelSession> relay_near = std::make_shared<ChannelSession>();
  std::shared_ptr<ChannelSession> relay_far = std::make_shared<ChannelSession>();

  static Roe<std::unique_ptr<AmpTestRelayPath>> Create(const std::string& protocol, const ChannelPolicy& policy) {
    auto path = std::make_unique<AmpTestRelayPath>();
    auto near = AmpTestLink::Create();
    auto far = AmpTestLink::Create();
    if (!near || !far) {
      return Error("relay path: link failed");
    }
    path->near = std::move(*near);
    path->far = std::move(*far);
    auto near_ch = path->near->initiator.mux.OpenOutbound(protocol, policy);
    auto far_ch = path->far->initiator.mux.OpenOutbound(protocol, policy);
    if (!near_ch || !far_ch) {
      return Error("relay path: channel open failed");
    }
    path->near_channel = *near_ch;
    path->far_channel = *far_ch;
    path->relay_near->Bind(path->near->responder.mux, path->near_channel, policy,
                           [](Roe<std::vector<uint8_t>>) { return true; });
    path->relay_far->Bind(path->far->initiator.mux, path->far_channel, policy,
                          [](Roe<std::vector<uint8_t>>) { return true; });
    return path;
  }

  ChannelMux& DialerMux() { return near->initiator.mux; }
  ChannelMux& TargetMux() { return far->responder.mux; }
};

} // namespace pp::amp::test
