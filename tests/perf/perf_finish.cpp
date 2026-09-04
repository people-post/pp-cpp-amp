#include "amp/L1/Clock.h"
#include "amp/L1/Endpoint.h"
#include "amp/L1/OsUdpDatagramIo.h"
#include "amp/L1/Types.h"
#include "amp/L3/AmpChannelLimits.h"
#include "amp/L3/ChannelPolicy.h"
#include "amp/L3/ChannelSession.h"
#include "amp/L3/Types.h"
#include "amp/link/AdpMultiaddr.h"
#include "amp/link/MeshRuntime.h"
#include "amp/link/Types.h"
#include "crypto/MlDsa.h"
#include "perf_report.h"
#include "support/amp_integration_harness.h"
#include "support/mesh_harness_support.h"

#include "L3/tests/amp_test_link.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pp::amp::perf {
namespace {

using HarnessSide = pbr::test::HarnessSide;

constexpr size_t kKeepaliveWireBytes = pp::adp::kHeaderBytes + pp::adp::kHmacBytes; // empty Keepalive

} // namespace

bool RunC5(const int warmup, const int iters) {
  std::printf("\n== C5 Mux multi-channel (control + bulk + realtime) ==\n");
  const int case_warmup = std::min(warmup, 2);
  const int case_iters = std::max(1, iters);
  std::vector<double> samples;

  for (int i = 0; i < case_warmup + case_iters; ++i) {
    auto link_result = pp::amp::test::AmpTestLink::Create();
    if (!link_result) {
      return false;
    }
    auto& link = **link_result;

    auto ch_ctrl = link.initiator.mux.OpenOutbound("/pp-browser/chat/1.0.0", pp::amp::ControlJsonChannelPolicy());
    auto ch_bulk = link.initiator.mux.OpenOutbound("/pp-browser/chat-blob/1.0.0", pp::amp::ChatBlobChannelPolicy());
    auto ch_rt = link.initiator.mux.OpenOutbound("/pp-browser/call-media/1.0.0", pp::amp::CallMediaChannelPolicy());
    if (!ch_ctrl || !ch_bulk || !ch_rt) {
      std::fprintf(stderr, "C5 OpenOutbound failed\n");
      return false;
    }

    size_t got_ctrl = 0;
    size_t got_bulk = 0;
    size_t got_rt = 0;
    link.responder.mux.SetDataHandler(*ch_ctrl, [&](uint32_t, std::vector<uint8_t> p) { got_ctrl += p.size(); });
    link.responder.mux.SetDataHandler(*ch_bulk, [&](uint32_t, std::vector<uint8_t> p) { got_bulk += p.size(); });
    link.responder.mux.SetDataHandler(*ch_rt, [&](uint32_t, std::vector<uint8_t> p) { got_rt += p.size(); });

    std::vector<uint8_t> ctrl(64, 0x01);
    std::vector<uint8_t> bulk(32 * 1024, 0x02);
    std::vector<uint8_t> rt(40, 0x03);

    const auto t0 = std::chrono::steady_clock::now();
    // Interleave: bulk FRAG + control + realtime bursts (AmpTestLink is sync — checks mux fairness).
    for (int round = 0; round < 8; ++round) {
      if (!link.initiator.mux.SendData(*ch_bulk, bulk)) {
        return false;
      }
      if (!link.initiator.mux.SendData(*ch_ctrl, ctrl)) {
        return false;
      }
      for (int r = 0; r < 10; ++r) {
        if (!link.initiator.mux.SendData(*ch_rt, rt)) {
          return false;
        }
      }
    }
    const auto t1 = std::chrono::steady_clock::now();

    const size_t want_ctrl = 8 * ctrl.size();
    const size_t want_bulk = 8 * bulk.size();
    const size_t want_rt = 8 * 10 * rt.size();
    if (got_ctrl != want_ctrl || got_bulk != want_bulk || got_rt != want_rt) {
      std::fprintf(stderr, "C5 fairness miss ctrl=%zu/%zu bulk=%zu/%zu rt=%zu/%zu\n", got_ctrl, want_ctrl, got_bulk,
                   want_bulk, got_rt, want_rt);
      return false;
    }
    if (i >= case_warmup) {
      samples.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
    }
  }

  const auto stats = Summarize(std::move(samples));
  const size_t total_bytes = 8 * (64 + 32 * 1024 + 10 * 40);
  const double mbps = MegaBytesPerSec(stats.mean_ns, total_bytes);
  PrintHuman("C5", "mux_3ch_interleave", stats, mbps, "MB/s");
  PrintCsv("C5", "mux_3ch_interleave", total_bytes, stats, mbps, "MB/s");
  return true;
}

bool RunD3(const int warmup, const int iters) {
  std::printf("\n== D3 Keepalive hot budget (simulated 1 hour) ==\n");
  (void)warmup;
  (void)iters;
  auto cfg = pbr::test::AmpMeshTestLinkConfig();
  cfg.keepalive_hot_interval = std::chrono::milliseconds{20'000};
  cfg.keepalive_warm_interval = std::chrono::milliseconds{60'000};
  auto created = pbr::test::MakeAmpIntegrationHarness(cfg);
  if (!created || !(*created)->Associate()) {
    std::fprintf(stderr, "D3 Associate failed\n");
    return false;
  }
  auto& h = **created;
  h.mgr_a().MarkHot("b");
  auto* link = h.mgr_a().FindLink("b");
  if (!link) {
    return false;
  }

  int64_t prev_tx = link->LastKeepaliveTxMs();
  size_t keepalive_count = 0;
  // 180 × 20 s = 3600 s simulated hour.
  for (int slot = 0; slot < 180; ++slot) {
    h.AdvanceMs(20'000);
    h.PumpBudget(4);
    const int64_t tx = link->LastKeepaliveTxMs();
    if (tx != 0 && tx != prev_tx) {
      ++keepalive_count;
      prev_tx = tx;
    }
  }

  if (keepalive_count < 170) {
    std::fprintf(stderr, "D3 expected ~180 hot keepalives, got %zu\n", keepalive_count);
    return false;
  }

  const double bytes_per_hour = static_cast<double>(keepalive_count) * static_cast<double>(kKeepaliveWireBytes);
  SampleStats s;
  s.n = 1;
  s.mean_ns = bytes_per_hour;
  s.p50_ns = bytes_per_hour;
  s.p95_ns = bytes_per_hour;
  s.p99_ns = bytes_per_hour;
  PrintHuman("D3", "hot_keepalive_Bph", s, bytes_per_hour, "B/hour");
  PrintCsv("D3", "hot_keepalive_Bph", keepalive_count, s, bytes_per_hour, "B/hour");

  // Warm tier check (same wire size, 60 s interval → ~60/hour).
  auto created_w = pbr::test::MakeAmpIntegrationHarness(cfg);
  if (!created_w || !(*created_w)->Associate()) {
    return false;
  }
  auto& hw = **created_w;
  hw.mgr_a().MarkWarm("b");
  auto* link_w = hw.mgr_a().FindLink("b");
  if (!link_w) {
    return false;
  }
  prev_tx = link_w->LastKeepaliveTxMs();
  size_t warm_count = 0;
  for (int slot = 0; slot < 60; ++slot) {
    hw.AdvanceMs(60'000);
    hw.PumpBudget(4);
    const int64_t tx = link_w->LastKeepaliveTxMs();
    if (tx != 0 && tx != prev_tx) {
      ++warm_count;
      prev_tx = tx;
    }
  }
  const double warm_bph = static_cast<double>(warm_count) * static_cast<double>(kKeepaliveWireBytes);
  SampleStats sw;
  sw.n = 1;
  sw.mean_ns = warm_bph;
  sw.p50_ns = warm_bph;
  sw.p95_ns = warm_bph;
  sw.p99_ns = warm_bph;
  PrintHuman("D3", "warm_keepalive_Bph", sw, warm_bph, "B/hour");
  PrintCsv("D3", "warm_keepalive_Bph", warm_count, sw, warm_bph, "B/hour");
  return true;
}

bool RunE4(const int warmup, const int iters) {
  std::printf("\n== E4 Nested carrier associate + DATA vs direct ==\n");
  const int case_warmup = std::min(warmup, 1);
  const int case_iters = std::max(1, iters);
  std::vector<double> direct_ns;
  std::vector<double> nested_assoc_ns;
  std::vector<double> nested_data_ns;

  for (int i = 0; i < case_warmup + case_iters; ++i) {
    auto created = pbr::test::MakeAmpIntegrationHarness();
    if (!created) {
      return false;
    }
    auto& h = **created;
    const std::vector<std::string> protos = {pp::amp::kAmpCircuitCarrierProtocolId, "/pp-browser/chat/1.0.0"};
    h.mgr_a().SetAdvertisedProtocols(protos);
    h.mgr_b().SetAdvertisedProtocols(protos);
    h.mgr_b().EnableNestedCarrierAccept(true);
    if (!h.Associate()) {
      std::fprintf(stderr, "E4 Associate failed\n");
      return false;
    }

    // Direct open+data (same as C2 shape).
    {
      const auto t0 = std::chrono::steady_clock::now();
      const auto ch = h.OpenChannel(HarnessSide::A, "b", "/pp-browser/chat/1.0.0", pp::amp::ControlJsonChannelPolicy());
      if (!ch) {
        return false;
      }
      std::vector<uint8_t> received;
      auto* inbound = h.mgr_b().FindLinkByPeerId(h.peer_id_a);
      if (!inbound || !inbound->Mux()) {
        return false;
      }
      inbound->Mux()->SetDataHandler(*ch, [&](uint32_t, std::vector<uint8_t> p) { received = std::move(p); });
      std::vector<uint8_t> msg = {'d', 'i', 'r'};
      if (!h.SendMuxData(HarnessSide::A, "b", *ch, msg) ||
          !h.PumpUntilReceived(received, [&] { return received == msg; })) {
        return false;
      }
      if (i >= case_warmup) {
        direct_ns.push_back(std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count());
      }
    }

    // Nested: open carrier, establish nested MSH, open chat on nested mux, DATA.
    std::vector<uint8_t> nested_received;
    h.mgr_b().SetProtocolHandler("/pp-browser/chat/1.0.0", [&](pp::amp::PeerLink& link, uint32_t channel_id) {
      if (!link.IsCarrierBacked() || !link.Mux()) {
        return;
      }
      link.Mux()->SetDataHandler(channel_id, [&](uint32_t, std::vector<uint8_t> p) { nested_received = std::move(p); });
    });

    const auto t_nested0 = std::chrono::steady_clock::now();
    bool open_done = false;
    std::optional<uint32_t> carrier_ch;
    h.mgr_a().OpenChannel("b", pp::amp::kAmpCircuitCarrierProtocolId, pp::amp::CircuitCarrierChannelPolicy(),
                          [&](pp::amp::PeerLinkManager::ChannelRoe ch) {
                            if (ch.isOk()) {
                              carrier_ch = ch.value();
                            }
                            open_done = true;
                          });
    h.PumpUntil([&] {
      if (!open_done || !carrier_ch) {
        return false;
      }
      auto* outer_wait = h.mgr_a().FindLink("b");
      return outer_wait && outer_wait->Mux() &&
             outer_wait->Mux()->State(*carrier_ch) == pp::amp::ChannelState::Open;
    });
    if (!carrier_ch) {
      std::fprintf(stderr, "E4 carrier open failed\n");
      return false;
    }
    auto* outer = h.mgr_a().FindLink("b");
    if (!outer || !outer->Mux()) {
      return false;
    }
    auto carrier = std::make_shared<pp::amp::ChannelSession>();
    carrier->Bind(*outer->Mux(), *carrier_ch, pp::amp::CircuitCarrierChannelPolicy(),
                  [](pp::Roe<std::vector<uint8_t>>) { return true; });

    bool nested_done = false;
    bool nested_ok = false;
    const std::string nested_key = "nested-b";
    h.mgr_a().EstablishNestedOverCarrier(nested_key, carrier, true, [&](pp::amp::PeerLinkManager::LinkRoe r) {
      nested_ok = r.isOk();
      nested_done = true;
    });
    h.PumpUntil([&] { return nested_done && h.mgr_a().IsConnected(nested_key); }, 3000);
    if (!nested_ok || !h.mgr_a().IsConnected(nested_key)) {
      std::fprintf(stderr, "E4 nested handshake failed\n");
      return false;
    }
    const auto t_assoc1 = std::chrono::steady_clock::now();
    if (i >= case_warmup) {
      nested_assoc_ns.push_back(std::chrono::duration<double, std::nano>(t_assoc1 - t_nested0).count());
    }

    pp::amp::PeerLink* nested = h.mgr_a().FindLink(nested_key);
    if ((!nested || !nested->IsCarrierBacked())) {
      // May have rekeyed to peer id — find carrier-backed Session to bob.
      nested = nullptr;
      if (auto* by_id = h.mgr_a().FindLinkByPeerId(h.peer_id_b)) {
        if (by_id->IsCarrierBacked()) {
          nested = by_id;
        }
      }
    }
    if (!nested || !nested->Mux() || !nested->IsCarrierBacked()) {
      std::fprintf(stderr, "E4 nested link not carrier-backed\n");
      return false;
    }

    bool ch_done = false;
    std::optional<uint32_t> nested_ch;
    h.mgr_a().OpenChannelOnLink(*nested, "/pp-browser/chat/1.0.0", pp::amp::ControlJsonChannelPolicy(),
                                [&](pp::amp::PeerLinkManager::ChannelRoe ch) {
                                  if (ch.isOk()) {
                                    nested_ch = ch.value();
                                  }
                                  ch_done = true;
                                });
    h.PumpUntil(
        [&] {
          if (!ch_done || !nested_ch || !nested->Mux()) {
            return false;
          }
          return nested->Mux()->State(*nested_ch) == pp::amp::ChannelState::Open;
        },
        2000);
    if (!nested_ch) {
      std::fprintf(stderr, "E4 nested chat open failed\n");
      return false;
    }
    std::vector<uint8_t> msg = {'n', 's', 't'};
    bool sent = false;
    for (int attempt = 0; attempt < 32; ++attempt) {
      if (static_cast<bool>(nested->Mux()->SendData(*nested_ch, msg))) {
        sent = true;
        break;
      }
      h.PumpBoth();
    }
    if (!sent) {
      std::fprintf(stderr, "E4 nested SendData failed\n");
      return false;
    }
    if (!h.PumpUntilReceived(nested_received, [&] { return nested_received == msg; }, 3000)) {
      std::fprintf(stderr, "E4 nested DATA missing (got %zu bytes)\n", nested_received.size());
      return false;
    }
    if (i >= case_warmup) {
      nested_data_ns.push_back(std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t_nested0).count());
    }
  }

  const auto dstats = Summarize(std::move(direct_ns));
  const auto astats = Summarize(std::move(nested_assoc_ns));
  const auto nstats = Summarize(std::move(nested_data_ns));
  PrintHuman("E4", "direct_open_data", dstats, dstats.mean_ns / 1e6, "ms");
  PrintCsv("E4", "direct_open_data", 3, dstats, dstats.mean_ns / 1e6, "ms");
  PrintHuman("E4", "nested_assoc", astats, astats.mean_ns / 1e6, "ms");
  PrintCsv("E4", "nested_assoc", 0, astats, astats.mean_ns / 1e6, "ms");
  PrintHuman("E4", "nested_end_to_end", nstats, nstats.mean_ns / 1e6, "ms");
  PrintCsv("E4", "nested_end_to_end", 3, nstats, nstats.mean_ns / 1e6, "ms");
  return true;
}

bool RunOsUdpAmp(const int warmup, const int iters) {
  std::printf("\n== OsUdp AMP paced bulk (64 KiB) ==\n");
  {
    auto probe_a = pp::adp::OsUdpDatagramIo::Bind(pp::adp::IpEndpoint::V4(127, 0, 0, 1, 0));
    auto probe_b = pp::adp::OsUdpDatagramIo::Bind(pp::adp::IpEndpoint::V4(127, 0, 0, 1, 0));
    if (!probe_a || !probe_b) {
      std::fprintf(stderr, "OsUdpAmp Bind failed — skip\n");
      return true;
    }
  }

  constexpr size_t kTotal = 64 * 1024;
  constexpr size_t kChunk = 900;
  const int case_warmup = std::min(warmup, 1);
  const int case_iters = std::max(1, iters);
  std::vector<double> samples;

  for (int i = 0; i < case_warmup + case_iters; ++i) {
    auto bound_a = pp::adp::OsUdpDatagramIo::Bind(pp::adp::IpEndpoint::V4(127, 0, 0, 1, 0));
    auto bound_b = pp::adp::OsUdpDatagramIo::Bind(pp::adp::IpEndpoint::V4(127, 0, 0, 1, 0));
    if (!bound_a || !bound_b) {
      return false;
    }
    std::shared_ptr<pp::adp::DatagramIo> io_a(std::move(*bound_a));
    std::shared_ptr<pp::adp::DatagramIo> io_b(std::move(*bound_b));
    auto clock = std::make_shared<pp::adp::VirtualClock>(2'000'000);
    auto ep_a = std::make_unique<pp::adp::Endpoint>(io_a, clock);
    auto ep_b = std::make_unique<pp::adp::Endpoint>(io_b, clock);
    ep_b->SetAcceptEnabled(true);

    auto alice_keys = pp::MlDsa::GenerateKeyPair();
    auto bob_keys = pp::MlDsa::GenerateKeyPair();
    if (!alice_keys || !bob_keys) {
      return false;
    }
    pp::amp::MshIdentity alice;
    alice.ml_dsa_secret_key = std::move(alice_keys->secret_key);
    alice.ml_dsa_public_key = std::move(alice_keys->public_key);
    pp::amp::MshIdentity bob;
    bob.ml_dsa_secret_key = std::move(bob_keys->secret_key);
    bob.ml_dsa_public_key = std::move(bob_keys->public_key);
    auto peer_a = pbr::test::DeriveTestPeerId(alice.ml_dsa_public_key);
    auto peer_b = pbr::test::DeriveTestPeerId(bob.ml_dsa_public_key);
    if (!peer_a || !peer_b) {
      return false;
    }
    auto cfg = pbr::test::AmpMeshTestLinkConfig();
    auto rt_a = std::make_unique<pp::amp::MeshRuntime>(*ep_a, alice, *peer_a, cfg);
    auto rt_b = std::make_unique<pp::amp::MeshRuntime>(*ep_b, bob, *peer_b, cfg);
    rt_a->Start();
    rt_b->Start();

    auto ma_b = pp::amp::FormatAdpMultiaddr(ep_b->Io().LocalEndpoint(), *peer_b);
    if (!ma_b || !static_cast<bool>(rt_a->Links().RegisterEndpoint("b", *ma_b))) {
      return false;
    }

    auto pump = [&] {
      rt_a->Drive();
      rt_b->Drive();
    };

    bool done = false;
    bool ok = false;
    rt_a->Links().EnsureAssociation("b", [&](pp::amp::PeerLinkManager::LinkRoe r) {
      ok = r.isOk();
      done = true;
    });
    for (size_t round = 0; round < 5000 && !(done && rt_a->Links().IsConnected("b")); ++round) {
      pump();
      clock->Advance(1);
    }
    if (!ok || !rt_a->Links().IsConnected("b")) {
      std::fprintf(stderr, "OsUdpAmp Associate failed\n");
      return false;
    }

    // Wait capability / ch0
    for (size_t round = 0; round < 2000; ++round) {
      pump();
      auto* link = rt_a->Links().FindLink("b");
      if (link && link->Mux() && link->Mux()->State(pp::amp::kCapabilityChannelId) == pp::amp::ChannelState::Open &&
          link->RemoteCapability()) {
        break;
      }
      clock->Advance(1);
    }

    bool ch_done = false;
    std::optional<uint32_t> ch;
    rt_a->Links().OpenChannel("b", "/pp-browser/chat-blob/1.0.0", pp::amp::ChatBlobChannelPolicy(),
                              [&](pp::amp::PeerLinkManager::ChannelRoe r) {
                                if (r.isOk()) {
                                  ch = r.value();
                                }
                                ch_done = true;
                              });
    for (size_t round = 0; round < 2000 && !ch_done; ++round) {
      pump();
      clock->Advance(1);
    }
    if (!ch) {
      std::fprintf(stderr, "OsUdpAmp OpenChannel failed\n");
      return false;
    }

    size_t received = 0;
    auto* inbound = rt_b->Links().FindLinkByPeerId(*peer_a);
    if (!inbound || !inbound->Mux()) {
      std::fprintf(stderr, "OsUdpAmp inbound missing\n");
      return false;
    }
    inbound->Mux()->SetDataHandler(*ch, [&](uint32_t, std::vector<uint8_t> p) { received += p.size(); });

    const auto t0 = std::chrono::steady_clock::now();
    size_t sent = 0;
    while (sent < kTotal) {
      bool made_progress = false;
      while (sent < kTotal) {
        const size_t n = std::min(kChunk, kTotal - sent);
        std::vector<uint8_t> chunk(n, 0x5A);
        auto* link = rt_a->Links().FindLink("b");
        if (!link || !link->Mux()) {
          return false;
        }
        auto r = link->Mux()->SendData(*ch, chunk);
        if (!r) {
          break;
        }
        sent += n;
        made_progress = true;
      }
      pump();
      if (!made_progress) {
        clock->Advance(pp::adp::kDefaultRtxIntervalMs);
        pump();
      }
    }
    for (size_t round = 0; round < 10000 && received < kTotal; ++round) {
      pump();
      clock->Advance(1);
    }
    const auto t1 = std::chrono::steady_clock::now();
    if (received != kTotal) {
      std::fprintf(stderr, "OsUdpAmp incomplete %zu/%zu\n", received, kTotal);
      return false;
    }
    if (i >= case_warmup) {
      samples.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
    }
  }

  const auto stats = Summarize(std::move(samples));
  const double mbps = MegaBytesPerSec(stats.mean_ns, kTotal);
  PrintHuman("OsUdp", "amp_paced_64KiB", stats, mbps, "MB/s");
  PrintCsv("OsUdp", "amp_paced_64KiB", kTotal, stats, mbps, "MB/s");
  return true;
}

} // namespace pp::amp::perf
