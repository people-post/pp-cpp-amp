#include "amp/L1/Clock.h"
#include "amp/L1/Connection.h"
#include "amp/L1/Endpoint.h"
#include "amp/L1/MemoryDatagramIo.h"
#include "amp/L1/OsUdpDatagramIo.h"
#include "amp/L1/Types.h"
#include "amp/L3/ChannelPolicy.h"
#include "amp/L3/ChannelWire.h"
#include "amp/L3/Types.h"
#include "amp/link/AdpMultiaddr.h"
#include "amp/link/MeshRuntime.h"
#include "amp/link/Types.h"
#include "crypto/MlDsa.h"
#include "perf_report.h"
#include "support/amp_integration_harness.h"
#include "support/mesh_harness_support.h"

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

pp::adp::PeerKey L1Key(const uint8_t fill = 0x55) {
  pp::adp::PeerKey k;
  k.bytes.fill(fill);
  return k;
}

pp::adp::AssocId L1Aid(const uint8_t fill = 0x66) {
  pp::adp::AssocId id;
  id.bytes.fill(fill);
  return id;
}

struct L1Pair {
  std::shared_ptr<pp::adp::VirtualClock> clock;
  std::shared_ptr<pp::adp::MemoryDatagramHub> hub;
  std::shared_ptr<pp::adp::MemoryDatagramIo> io_a;
  std::shared_ptr<pp::adp::MemoryDatagramIo> io_b;
  std::unique_ptr<pp::adp::Endpoint> ep_a;
  std::unique_ptr<pp::adp::Endpoint> ep_b;
  pp::adp::IpEndpoint addr_a;
  pp::adp::IpEndpoint addr_b;
};

L1Pair MakeL1Pair() {
  L1Pair p;
  p.clock = std::make_shared<pp::adp::VirtualClock>(5'000'000);
  p.hub = pp::adp::MemoryDatagramIo::MakeHub();
  p.addr_a = pp::adp::IpEndpoint::V4(127, 0, 0, 1, 4101);
  p.addr_b = pp::adp::IpEndpoint::V4(127, 0, 0, 1, 4102);
  p.io_a = std::make_shared<pp::adp::MemoryDatagramIo>(p.hub, p.addr_a);
  p.io_b = std::make_shared<pp::adp::MemoryDatagramIo>(p.hub, p.addr_b);
  p.ep_a = std::make_unique<pp::adp::Endpoint>(p.io_a, p.clock);
  p.ep_b = std::make_unique<pp::adp::Endpoint>(p.io_b, p.clock);
  return p;
}

void PumpL1(L1Pair& p) {
  p.ep_a->Pump();
  p.ep_b->Pump();
  p.ep_a->Tick();
  p.ep_b->Tick();
}

bool TransferReliable(L1Pair& p, std::shared_ptr<pp::adp::Connection> ca, std::shared_ptr<pp::adp::Connection> cb,
                      const size_t packet_bytes, const size_t packet_count, const double drop_rate,
                      double* out_elapsed_ns) {
  p.io_a->SetDropRate(drop_rate);
  p.io_a->SetRngSeed(42);
  p.io_b->SetDropRate(0);

  size_t received_bytes = 0;
  cb->OnMessage([&](const pp::adp::Message& m) {
    if (m.qos == pp::adp::QosClass::Reliable) {
      received_bytes += m.payload.size();
    }
  });

  const size_t total = packet_bytes * packet_count;
  std::vector<uint8_t> payload(packet_bytes, 0xAB);
  const auto t0 = std::chrono::steady_clock::now();

  size_t sent = 0;
  for (size_t round = 0; round < 200000 && received_bytes < total; ++round) {
    while (sent < packet_count) {
      auto r = ca->Send(pp::adp::QosClass::Reliable, payload);
      if (!r) {
        if (r.error().GetCode() == pp::adp::Connection::Err::WindowFull) {
          break;
        }
        std::fprintf(stderr, "B1 Send failed: %s\n", r.error().message.c_str());
        return false;
      }
      ++sent;
    }
    PumpL1(p);
    if (received_bytes < total) {
      p.clock->Advance(pp::adp::kDefaultRtxIntervalMs);
      PumpL1(p);
    }
  }

  const auto t1 = std::chrono::steady_clock::now();
  if (received_bytes != total) {
    std::fprintf(stderr, "B1 incomplete: got %zu / %zu (drop_rate=%.2f sent=%zu)\n", received_bytes, total, drop_rate,
                 sent);
    return false;
  }
  if (out_elapsed_ns) {
    *out_elapsed_ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
  }
  return true;
}

} // namespace

bool RunB1(const int warmup, const int iters) {
  std::printf("\n== B1 Reliable window + loss (MemoryDatagramIo) ==\n");
  constexpr size_t kPacketBytes = 600;
  constexpr size_t kPacketCount = 64; // 4 full windows of 16
  const double rates[] = {0.0, 0.05, 0.10};
  const int case_warmup = std::min(warmup, 1);
  const int case_iters = std::max(1, iters);

  for (const double rate : rates) {
    std::vector<double> samples;
    samples.reserve(static_cast<size_t>(case_iters));
    for (int i = 0; i < case_warmup + case_iters; ++i) {
      auto p = MakeL1Pair();
      p.ep_b->SetAcceptKey(L1Key());
      p.ep_b->SetAcceptEnabled(true);

      pp::adp::OpenParams op;
      op.key = L1Key();
      op.id = L1Aid();
      op.mint_id = false;
      op.peer = p.addr_b;
      op.rtx_interval_ms = pp::adp::kDefaultRtxIntervalMs;
      auto ca = p.ep_a->Open(op);
      if (!ca) {
        std::fprintf(stderr, "B1 open A failed\n");
        return false;
      }
      pp::adp::OpenParams opb = op;
      opb.peer = p.addr_a;
      auto cb = p.ep_b->Open(opb);
      if (!cb) {
        std::fprintf(stderr, "B1 open B failed\n");
        return false;
      }

      double elapsed_ns = 0;
      if (!TransferReliable(p, *ca, *cb, kPacketBytes, kPacketCount, rate, &elapsed_ns)) {
        return false;
      }
      if (i >= case_warmup) {
        samples.push_back(elapsed_ns);
      }
    }
    const auto stats = Summarize(std::move(samples));
    const size_t total_bytes = kPacketBytes * kPacketCount;
    const double mbps = MegaBytesPerSec(stats.mean_ns, total_bytes);
    char label[64];
    std::snprintf(label, sizeof(label), "loss_%.0f_pct", rate * 100.0);
    PrintHuman("B1", label, stats, mbps, "MB/s");
    PrintCsv("B1", label, total_bytes, stats, mbps, "MB/s");
  }
  return true;
}

bool RunOsUdp(const int warmup, const int iters) {
  std::printf("\n== OsUdp loopback Reliable transfer ==\n");
  {
    auto probe_a = pp::adp::OsUdpDatagramIo::Bind(pp::adp::IpEndpoint::V4(127, 0, 0, 1, 0));
    auto probe_b = pp::adp::OsUdpDatagramIo::Bind(pp::adp::IpEndpoint::V4(127, 0, 0, 1, 0));
    if (!probe_a || !probe_b) {
      std::fprintf(stderr, "OsUdp Bind failed — skip\n");
      return true; // soft-skip in restricted environments
    }
  }

  constexpr size_t kPacketBytes = 1200;
  constexpr size_t kPacketCount = 64; // 76800 bytes
  const int case_warmup = std::min(warmup, 1);
  const int case_iters = std::max(1, iters);
  std::vector<double> samples;
  samples.reserve(static_cast<size_t>(case_iters));

  for (int i = 0; i < case_warmup + case_iters; ++i) {
    auto bound_a_i = pp::adp::OsUdpDatagramIo::Bind(pp::adp::IpEndpoint::V4(127, 0, 0, 1, 0));
    auto bound_b_i = pp::adp::OsUdpDatagramIo::Bind(pp::adp::IpEndpoint::V4(127, 0, 0, 1, 0));
    if (!bound_a_i || !bound_b_i) {
      std::fprintf(stderr, "OsUdp Bind failed on iter\n");
      return false;
    }
    std::shared_ptr<pp::adp::DatagramIo> io_a(std::move(*bound_a_i));
    std::shared_ptr<pp::adp::DatagramIo> io_b(std::move(*bound_b_i));
    auto clock = std::make_shared<pp::adp::VirtualClock>(9'000'000);
    auto ep_a = std::make_unique<pp::adp::Endpoint>(io_a, clock);
    auto ep_b = std::make_unique<pp::adp::Endpoint>(io_b, clock);
    const auto addr_a = ep_a->Io().LocalEndpoint();
    const auto addr_b = ep_b->Io().LocalEndpoint();

    pp::adp::OpenParams op;
    op.key = L1Key(0x77);
    op.id = L1Aid(0x88);
    op.mint_id = false;
    op.peer = addr_b;
    auto ca = ep_a->Open(op);
    if (!ca) {
      return false;
    }
    pp::adp::OpenParams opb = op;
    opb.peer = addr_a;
    auto cb = ep_b->Open(opb);
    if (!cb) {
      return false;
    }

    size_t received_bytes = 0;
    (*cb)->OnMessage([&](const pp::adp::Message& m) {
      if (m.qos == pp::adp::QosClass::Reliable) {
        received_bytes += m.payload.size();
      }
    });

    const size_t total = kPacketBytes * kPacketCount;
    std::vector<uint8_t> payload(kPacketBytes, 0xCD);
    const auto t0 = std::chrono::steady_clock::now();
    size_t sent = 0;
    for (size_t round = 0; round < 200000 && received_bytes < total; ++round) {
      while (sent < kPacketCount) {
        auto r = (*ca)->Send(pp::adp::QosClass::Reliable, payload);
        if (!r) {
          if (r.error().GetCode() == pp::adp::Connection::Err::WindowFull) {
            break;
          }
          std::fprintf(stderr, "OsUdp Send failed: %s\n", r.error().message.c_str());
          return false;
        }
        ++sent;
      }
      ep_a->Pump();
      ep_b->Pump();
      ep_a->Tick();
      ep_b->Tick();
      if (received_bytes < total) {
        clock->Advance(pp::adp::kDefaultRtxIntervalMs);
      }
    }
    const auto t1 = std::chrono::steady_clock::now();
    if (received_bytes != total) {
      std::fprintf(stderr, "OsUdp incomplete: got %zu / %zu\n", received_bytes, total);
      return false;
    }
    if (i >= case_warmup) {
      samples.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
    }
  }

  const auto stats = Summarize(std::move(samples));
  const size_t total_bytes = kPacketBytes * kPacketCount;
  const double mbps = MegaBytesPerSec(stats.mean_ns, total_bytes);
  PrintHuman("OsUdp", "reliable_76KiB", stats, mbps, "MB/s");
  PrintCsv("OsUdp", "reliable_76KiB", total_bytes, stats, mbps, "MB/s");
  return true;
}

bool RunE1(const int warmup, const int iters) {
  std::printf("\n== E1 Paced ledger-sized bulk (512 KiB, harness) ==\n");
  constexpr size_t kTotal = 512 * 1024;
  constexpr size_t kChunk = 900; // single DATA frames — WindowFull retries at SendData level
  const int case_warmup = std::min(warmup, 1);
  const int case_iters = std::max(1, iters);
  std::vector<double> samples;
  samples.reserve(static_cast<size_t>(case_iters));

  for (int i = 0; i < case_warmup + case_iters; ++i) {
    auto created = pbr::test::MakeAmpIntegrationHarness();
    if (!created || !(*created)->Associate()) {
      std::fprintf(stderr, "E1 Associate failed\n");
      return false;
    }
    auto& h = **created;
    const auto ch = h.OpenChannel(HarnessSide::A, "b", "/pp-ledger/rpc/1.0.0", pp::amp::ChatBlobChannelPolicy());
    if (!ch) {
      std::fprintf(stderr, "E1 OpenChannel failed\n");
      return false;
    }

    size_t received = 0;
    auto* inbound = h.mgr_b().FindLinkByPeerId(h.peer_id_a);
    if (!inbound || !inbound->Mux()) {
      std::fprintf(stderr, "E1 inbound missing\n");
      return false;
    }
    inbound->Mux()->SetDataHandler(*ch, [&](uint32_t, std::vector<uint8_t> payload) {
      received += payload.size();
    });

    const auto t0 = std::chrono::steady_clock::now();
    size_t sent = 0;
    while (sent < kTotal) {
      bool made_progress = false;
      while (sent < kTotal) {
        const size_t n = std::min(kChunk, kTotal - sent);
        std::vector<uint8_t> chunk(n, static_cast<uint8_t>(sent & 0xff));
        auto* link = h.mgr_a().FindLink("b");
        if (!link || !link->Mux()) {
          std::fprintf(stderr, "E1 outbound link missing\n");
          return false;
        }
        auto r = link->Mux()->SendData(*ch, chunk);
        if (!r) {
          break; // window full — drain below
        }
        sent += n;
        made_progress = true;
      }
      h.PumpBoth();
      if (!made_progress) {
        h.AdvanceMs(pp::adp::kDefaultRtxIntervalMs);
        h.PumpBoth();
      }
    }
    for (size_t round = 0; round < 5000 && received < kTotal; ++round) {
      h.PumpBoth();
      h.AdvanceMs(1);
    }
    const auto t1 = std::chrono::steady_clock::now();
    if (received != kTotal) {
      std::fprintf(stderr, "E1 incomplete: got %zu / %zu\n", received, kTotal);
      return false;
    }
    if (i >= case_warmup) {
      samples.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
    }
  }

  const auto stats = Summarize(std::move(samples));
  const double mbps = MegaBytesPerSec(stats.mean_ns, kTotal);
  PrintHuman("E1", "paced_512KiB", stats, mbps, "MB/s");
  PrintCsv("E1", "paced_512KiB", kTotal, stats, mbps, "MB/s");
  return true;
}

bool RunE2(const int warmup, const int iters) {
  std::printf("\n== E2 Paced call-media (40 B @ 50 pps, harness) ==\n");
  constexpr size_t kPacketBytes = 40;
  constexpr size_t kPacketCount = 100; // 2 s @ 50 pps
  constexpr int64_t kCadenceMs = 20;
  const int case_warmup = std::min(warmup, 1);
  const int case_iters = std::max(1, iters);
  std::vector<double> samples;
  samples.reserve(static_cast<size_t>(case_iters));

  for (int i = 0; i < case_warmup + case_iters; ++i) {
    auto created = pbr::test::MakeAmpIntegrationHarness();
    if (!created || !(*created)->Associate()) {
      std::fprintf(stderr, "E2 Associate failed\n");
      return false;
    }
    auto& h = **created;
    const auto ch = h.OpenChannel(HarnessSide::A, "b", "/pp-browser/call-media/1.0.0",
                                  pp::amp::CallMediaChannelPolicy());
    if (!ch) {
      std::fprintf(stderr, "E2 OpenChannel failed\n");
      return false;
    }

    size_t received = 0;
    auto* inbound = h.mgr_b().FindLinkByPeerId(h.peer_id_a);
    if (!inbound || !inbound->Mux()) {
      std::fprintf(stderr, "E2 inbound missing\n");
      return false;
    }
    inbound->Mux()->SetDataHandler(*ch, [&](uint32_t, std::vector<uint8_t> payload) {
      received += payload.size() >= kPacketBytes ? 1 : 0;
    });

    const auto t0 = std::chrono::steady_clock::now();
    std::vector<uint8_t> frame(kPacketBytes, 0xE2);
    for (size_t n = 0; n < kPacketCount; ++n) {
      auto* link = h.mgr_a().FindLink("b");
      if (!link || !link->Mux()) {
        std::fprintf(stderr, "E2 outbound link missing\n");
        return false;
      }
      for (int attempt = 0; attempt < 32; ++attempt) {
        auto r = link->Mux()->SendData(*ch, frame);
        h.PumpBoth();
        if (r) {
          break;
        }
        // Drop-Oldest Realtime: brief pump/retry is enough under MemoryDatagramIo.
        if (attempt == 31) {
          std::fprintf(stderr, "E2 SendData failed: %s\n", r.error().message.c_str());
          return false;
        }
      }
      h.AdvanceMs(kCadenceMs);
    }
    for (size_t round = 0; round < 2000 && received < kPacketCount; ++round) {
      h.PumpBoth();
      h.AdvanceMs(1);
    }
    const auto t1 = std::chrono::steady_clock::now();
    if (received != kPacketCount) {
      std::fprintf(stderr, "E2 incomplete: got %zu / %zu frames\n", received, kPacketCount);
      return false;
    }
    if (i >= case_warmup) {
      samples.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
    }
  }

  const auto stats = Summarize(std::move(samples));
  const double pkt_s = OpsPerSec(stats.mean_ns) * static_cast<double>(kPacketCount);
  PrintHuman("E2", "paced_media_50pps", stats, pkt_s, "pkt/s");
  PrintCsv("E2", "paced_media_50pps", kPacketBytes * kPacketCount, stats, pkt_s, "pkt/s");
  return true;
}

bool RunE3(const int warmup, const int iters) {
  std::printf("\n== E3 Paced 4 MiB FRAG bulk (harness) ==\n");
  constexpr size_t kTotal = 4ull * 1024ull * 1024ull;
  // FRAG msgs sized under ADP reliable_window (default 128) so preflight admits each SendData.
  constexpr size_t kFragPayload = 900;
  constexpr size_t kFragsPerMsg = 64;
  constexpr size_t kChunk = kFragPayload * kFragsPerMsg;
  const int case_warmup = std::min(warmup, 1);
  const int case_iters = std::max(1, iters);
  std::vector<double> samples;
  samples.reserve(static_cast<size_t>(case_iters));

  for (int i = 0; i < case_warmup + case_iters; ++i) {
    auto created = pbr::test::MakeAmpIntegrationHarness();
    if (!created || !(*created)->Associate()) {
      std::fprintf(stderr, "E3 Associate failed\n");
      return false;
    }
    auto& h = **created;
    const auto ch = h.OpenChannel(HarnessSide::A, "b", "/pp-browser/chat-blob/1.0.0",
                                  pp::amp::ChatBlobChannelPolicy());
    if (!ch) {
      std::fprintf(stderr, "E3 OpenChannel failed\n");
      return false;
    }

    size_t received = 0;
    auto* inbound = h.mgr_b().FindLinkByPeerId(h.peer_id_a);
    if (!inbound || !inbound->Mux()) {
      std::fprintf(stderr, "E3 inbound missing\n");
      return false;
    }
    inbound->Mux()->SetDataHandler(*ch, [&](uint32_t, std::vector<uint8_t> payload) {
      received += payload.size();
    });

    const auto t0 = std::chrono::steady_clock::now();
    size_t sent = 0;
    while (sent < kTotal) {
      bool made_progress = false;
      while (sent < kTotal) {
        const size_t n = std::min(kChunk, kTotal - sent);
        std::vector<uint8_t> chunk(n, static_cast<uint8_t>((sent / kChunk) & 0xff));
        auto* link = h.mgr_a().FindLink("b");
        if (!link || !link->Mux()) {
          std::fprintf(stderr, "E3 outbound link missing\n");
          return false;
        }
        auto r = link->Mux()->SendData(*ch, chunk);
        if (!r) {
          break; // credits / window — drain below
        }
        sent += n;
        made_progress = true;
      }
      h.PumpBoth();
      if (!made_progress) {
        h.AdvanceMs(pp::adp::kDefaultRtxIntervalMs);
        h.PumpBoth();
      }
    }
    for (size_t round = 0; round < 20000 && received < kTotal; ++round) {
      h.PumpBoth();
      h.AdvanceMs(1);
    }
    const auto t1 = std::chrono::steady_clock::now();
    if (received != kTotal) {
      std::fprintf(stderr, "E3 incomplete: got %zu / %zu\n", received, kTotal);
      return false;
    }
    if (i >= case_warmup) {
      samples.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
    }
  }

  const auto stats = Summarize(std::move(samples));
  const double mbps = MegaBytesPerSec(stats.mean_ns, kTotal);
  PrintHuman("E3", "paced_4MiB_frag", stats, mbps, "MB/s");
  PrintCsv("E3", "paced_4MiB_frag", kTotal, stats, mbps, "MB/s");
  return true;
}

bool RunD1Multi(const int warmup, const int drive_iters) {
  std::printf("\n== D1 multi-link Drive (hub fan-in) ==\n");
  // Default ladder stops at 16; set PP_AMP_PERF_MAX_LINKS=32 or 48 for heavier runs.
  const int max_links = EnvInt("PP_AMP_PERF_MAX_LINKS", 16);
  std::vector<int> link_counts = {1, 4, 8};
  if (max_links >= 16) {
    link_counts.push_back(16);
  }
  if (max_links >= 32) {
    link_counts.push_back(32);
  }
  if (max_links >= 48) {
    link_counts.push_back(48);
  }
  const int case_warmup = std::min(warmup, 20);
  const int case_iters = std::max(1, drive_iters);

  for (const int n_links : link_counts) {
    if (n_links > max_links) {
      continue;
    }
    auto clock = std::make_shared<pp::adp::VirtualClock>(1'000'000);
    auto hub = pp::adp::MemoryDatagramIo::MakeHub();
    const auto hub_addr = pp::adp::IpEndpoint::V4(10, 9, 0, 1, 9000);
    auto hub_io = std::make_shared<pp::adp::MemoryDatagramIo>(hub, hub_addr);
    auto hub_ep = std::make_unique<pp::adp::Endpoint>(hub_io, clock);
    hub_ep->SetAcceptEnabled(true);

    auto hub_keys = pp::MlDsa::GenerateKeyPair();
    if (!hub_keys) {
      return false;
    }
    pp::amp::MshIdentity hub_id;
    hub_id.ml_dsa_secret_key = std::move(hub_keys->secret_key);
    hub_id.ml_dsa_public_key = std::move(hub_keys->public_key);
    auto hub_peer = pbr::test::DeriveTestPeerId(hub_id.ml_dsa_public_key);
    if (!hub_peer) {
      return false;
    }
    auto cfg = pbr::test::AmpMeshTestLinkConfig();
    cfg.max_links = 48;
    auto hub_rt = std::make_unique<pp::amp::MeshRuntime>(*hub_ep, hub_id, *hub_peer, cfg);
    hub_rt->Start();
    auto hub_ma = pp::amp::FormatAdpMultiaddr(hub_addr, *hub_peer);
    if (!hub_ma) {
      return false;
    }

    struct Client {
      std::shared_ptr<pp::adp::MemoryDatagramIo> io;
      std::unique_ptr<pp::adp::Endpoint> ep;
      std::unique_ptr<pp::amp::MeshRuntime> rt;
      std::string alias;
    };
    std::vector<Client> clients;
    clients.reserve(static_cast<size_t>(n_links));

    for (int i = 0; i < n_links; ++i) {
      Client c;
      const auto addr = pp::adp::IpEndpoint::V4(10, 9, static_cast<uint8_t>(1 + i / 200),
                                                 static_cast<uint8_t>(1 + (i % 200)),
                                                 static_cast<uint16_t>(10000 + i));
      c.io = std::make_shared<pp::adp::MemoryDatagramIo>(hub, addr);
      c.ep = std::make_unique<pp::adp::Endpoint>(c.io, clock);
      auto keys = pp::MlDsa::GenerateKeyPair();
      if (!keys) {
        return false;
      }
      pp::amp::MshIdentity id;
      id.ml_dsa_secret_key = std::move(keys->secret_key);
      id.ml_dsa_public_key = std::move(keys->public_key);
      auto peer = pbr::test::DeriveTestPeerId(id.ml_dsa_public_key);
      if (!peer) {
        return false;
      }
      c.rt = std::make_unique<pp::amp::MeshRuntime>(*c.ep, id, *peer, cfg);
      c.rt->Start();
      c.alias = "hub";
      if (!static_cast<bool>(c.rt->Links().RegisterEndpoint(c.alias, *hub_ma))) {
        std::fprintf(stderr, "D1Multi RegisterEndpoint failed\n");
        return false;
      }
      bool done = false;
      bool ok = false;
      c.rt->Links().EnsureAssociation(c.alias, [&](pp::amp::PeerLinkManager::LinkRoe result) {
        ok = result.isOk();
        done = true;
      });
      for (size_t round = 0; round < 2000 && !(done && c.rt->Links().IsConnected(c.alias)); ++round) {
        hub_rt->Drive();
        c.rt->Drive();
        for (auto& other : clients) {
          other.rt->Drive();
        }
      }
      if (!ok || !c.rt->Links().IsConnected(c.alias)) {
        std::fprintf(stderr, "D1Multi Associate failed for client %d\n", i);
        return false;
      }
      c.rt->Links().MarkHot(c.alias);
      clients.push_back(std::move(c));
    }

    // Wait until hub sees n_links connected.
    for (size_t round = 0; round < 500; ++round) {
      hub_rt->Drive();
      for (auto& c : clients) {
        c.rt->Drive();
      }
      if (hub_rt->Links().CountLinks() >= static_cast<size_t>(n_links)) {
        break;
      }
    }
    if (hub_rt->Links().CountLinks() < static_cast<size_t>(n_links)) {
      std::fprintf(stderr, "D1Multi hub links=%zu want=%d\n", hub_rt->Links().CountLinks(), n_links);
      return false;
    }

    auto samples = TimeLoop(case_warmup, case_iters, [&] {
      hub_rt->Drive();
      for (auto& c : clients) {
        c.rt->Drive();
      }
    });
    const auto stats = Summarize(std::move(samples));
    char label[64];
    std::snprintf(label, sizeof(label), "drive_hub_%dlinks", n_links);
    PrintHuman("D1", label, stats, stats.mean_ns / 1e3, "us");
    PrintCsv("D1", label, static_cast<size_t>(n_links), stats, stats.mean_ns / 1e3, "us");
  }
  return true;
}

bool RunB2(const int warmup, const int iters) {
  std::printf("\n== B2 BestEffort flood @ 1200 B ==\n");
  constexpr size_t kPacketBytes = 1200;
  constexpr size_t kPacketCount = 2000;
  const int case_warmup = std::min(warmup, 1);
  const int case_iters = std::max(1, iters);
  std::vector<double> samples;
  samples.reserve(static_cast<size_t>(case_iters));
  size_t last_got = 0;

  for (int i = 0; i < case_warmup + case_iters; ++i) {
    auto p = MakeL1Pair();
    p.ep_b->SetAcceptKey(L1Key());
    p.ep_b->SetAcceptEnabled(true);
    pp::adp::OpenParams op;
    op.key = L1Key();
    op.id = L1Aid();
    op.mint_id = false;
    op.peer = p.addr_b;
    auto ca = p.ep_a->Open(op);
    if (!ca) {
      return false;
    }
    pp::adp::OpenParams opb = op;
    opb.peer = p.addr_a;
    auto cb = p.ep_b->Open(opb);
    if (!cb) {
      return false;
    }
    size_t got = 0;
    (*cb)->OnMessage([&](const pp::adp::Message& m) {
      if (m.qos == pp::adp::QosClass::BestEffort) {
        got += 1;
      }
    });
    std::vector<uint8_t> payload(kPacketBytes, 0xBE);
    const auto t0 = std::chrono::steady_clock::now();
    for (size_t n = 0; n < kPacketCount; ++n) {
      auto r = (*ca)->Send(pp::adp::QosClass::BestEffort, payload);
      if (!r) {
        std::fprintf(stderr, "B2 Send failed: %s\n", r.error().message.c_str());
        return false;
      }
      if ((n & 15u) == 15u) {
        PumpL1(p);
      }
    }
    for (int round = 0; round < 64; ++round) {
      PumpL1(p);
    }
    const auto t1 = std::chrono::steady_clock::now();
    last_got = got;
    if (got != kPacketCount) {
      std::fprintf(stderr, "B2 incomplete got=%zu want=%zu\n", got, kPacketCount);
      return false;
    }
    if (i >= case_warmup) {
      samples.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
    }
  }
  const auto stats = Summarize(std::move(samples));
  const double pkt_s = OpsPerSec(stats.mean_ns) * static_cast<double>(kPacketCount);
  PrintHuman("B2", "be_flood_1200", stats, pkt_s, "pkt/s");
  PrintCsv("B2", "be_flood_1200", kPacketBytes * kPacketCount, stats, pkt_s, "pkt/s");
  (void)last_got;
  return true;
}

bool RunB3(const int warmup, const int iters) {
  std::printf("\n== B3 Media cadence (~40 B @ 50 pps processing) ==\n");
  constexpr size_t kPacketBytes = 40;
  constexpr size_t kPacketCount = 50; // one second of 50 pps
  const int case_warmup = std::min(warmup, 2);
  const int case_iters = std::max(1, iters);
  std::vector<double> one_way_ns;

  for (int i = 0; i < case_warmup + case_iters; ++i) {
    auto p = MakeL1Pair();
    p.ep_b->SetAcceptKey(L1Key());
    p.ep_b->SetAcceptEnabled(true);
    pp::adp::OpenParams op;
    op.key = L1Key();
    op.id = L1Aid();
    op.mint_id = false;
    op.peer = p.addr_b;
    auto ca = p.ep_a->Open(op);
    if (!ca) {
      return false;
    }
    pp::adp::OpenParams opb = op;
    opb.peer = p.addr_a;
    auto cb = p.ep_b->Open(opb);
    if (!cb) {
      return false;
    }
    std::vector<double> frame_ns;
    frame_ns.reserve(kPacketCount);
    (*cb)->OnMessage([&](const pp::adp::Message&) {});
    std::vector<uint8_t> payload(kPacketBytes, 0x50);
    for (size_t n = 0; n < kPacketCount; ++n) {
      const auto t0 = std::chrono::steady_clock::now();
      if (!(*ca)->Send(pp::adp::QosClass::BestEffort, payload)) {
        return false;
      }
      PumpL1(p);
      const auto t1 = std::chrono::steady_clock::now();
      if (i >= case_warmup) {
        frame_ns.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
      }
      p.clock->Advance(20); // 50 pps cadence on virtual clock
    }
    one_way_ns.insert(one_way_ns.end(), frame_ns.begin(), frame_ns.end());
  }
  const auto stats = Summarize(std::move(one_way_ns));
  PrintHuman("B3", "media_40B_proc", stats, stats.mean_ns / 1e3, "us");
  PrintCsv("B3", "media_40B_proc", kPacketBytes, stats, stats.mean_ns / 1e3, "us");
  return true;
}

bool RunA3(const int warmup, const int iters) {
  std::printf("\n== A3 FRAG encode only (ChannelWire) ==\n");
  const size_t payloads[] = {900, 64 * 1024, 512 * 1024};
  const int case_warmup = std::min(warmup, 5);
  const int case_iters = std::max(1, iters);

  for (const size_t payload_len : payloads) {
    std::vector<uint8_t> payload(payload_len, 0xA3);
    constexpr size_t kChunk = 900;
    const uint16_t frag_count =
        static_cast<uint16_t>((payload_len + kChunk - 1) / kChunk);
    auto samples = TimeLoop(case_warmup, case_iters, [&] {
      for (uint16_t i = 0; i < frag_count; ++i) {
        const size_t offset = static_cast<size_t>(i) * kChunk;
        const size_t n = std::min(kChunk, payload_len - offset);
        pp::amp::ChannelFrame frame;
        frame.header.frame_type = pp::amp::ChannelFrameType::Frag;
        frame.header.channel_id = 7;
        frame.header.channel_seq = i + 1;
        frame.frag.msg_id = 1;
        frame.frag.frag_index = i;
        frame.frag.frag_count = frag_count;
        frame.frag.total_len = static_cast<uint32_t>(payload_len);
        frame.frag.chunk.assign(payload.begin() + static_cast<std::ptrdiff_t>(offset),
                               payload.begin() + static_cast<std::ptrdiff_t>(offset + n));
        auto wire = pp::amp::ChannelWire::Encode(frame);
        if (!wire) {
          std::abort();
        }
      }
    });
    const auto stats = Summarize(std::move(samples));
    const double mbps = MegaBytesPerSec(stats.mean_ns, payload_len);
    const std::string label = "encode_" + std::to_string(payload_len);
    PrintHuman("A3", label.c_str(), stats, mbps, "MB/s");
    PrintCsv("A3", label.c_str(), payload_len, stats, mbps, "MB/s");
  }
  return true;
}

bool RunD2(const int warmup, const int iters) {
  std::printf("\n== D2 hub fan-in data plane ==\n");
  constexpr int kClients = 4;
  constexpr size_t kBytesPerClient = 64 * 1024;
  constexpr size_t kChunk = 900;
  const int case_warmup = std::min(warmup, 1);
  const int case_iters = std::max(1, iters);
  std::vector<double> samples;

  for (int iter = 0; iter < case_warmup + case_iters; ++iter) {
    auto clock = std::make_shared<pp::adp::VirtualClock>(1'000'000);
    auto hub = pp::adp::MemoryDatagramIo::MakeHub();
    const auto hub_addr = pp::adp::IpEndpoint::V4(10, 8, 0, 1, 8000);
    auto hub_io = std::make_shared<pp::adp::MemoryDatagramIo>(hub, hub_addr);
    auto hub_ep = std::make_unique<pp::adp::Endpoint>(hub_io, clock);
    hub_ep->SetAcceptEnabled(true);

    auto hub_keys = pp::MlDsa::GenerateKeyPair();
    if (!hub_keys) {
      return false;
    }
    pp::amp::MshIdentity hub_id;
    hub_id.ml_dsa_secret_key = std::move(hub_keys->secret_key);
    hub_id.ml_dsa_public_key = std::move(hub_keys->public_key);
    auto hub_peer = pbr::test::DeriveTestPeerId(hub_id.ml_dsa_public_key);
    if (!hub_peer) {
      return false;
    }
    auto cfg = pbr::test::AmpMeshTestLinkConfig();
    cfg.max_links = 48;
    auto hub_rt = std::make_unique<pp::amp::MeshRuntime>(*hub_ep, hub_id, *hub_peer, cfg);
    hub_rt->Start();
    auto hub_ma = pp::amp::FormatAdpMultiaddr(hub_addr, *hub_peer);
    if (!hub_ma) {
      return false;
    }

    size_t received = 0;
    hub_rt->Links().SetProtocolHandler("/pp-perf/fanin/1.0.0", [&](pp::amp::PeerLink& link, uint32_t channel_id) {
      if (!link.Mux()) {
        return;
      }
      link.Mux()->SetDataHandler(channel_id, [&](uint32_t, std::vector<uint8_t> payload) {
        received += payload.size();
      });
    });

    struct Client {
      std::unique_ptr<pp::adp::Endpoint> ep;
      std::shared_ptr<pp::adp::MemoryDatagramIo> io;
      std::unique_ptr<pp::amp::MeshRuntime> rt;
    };
    std::vector<Client> clients;
    for (int i = 0; i < kClients; ++i) {
      Client c;
      const auto addr =
          pp::adp::IpEndpoint::V4(10, 8, 1, static_cast<uint8_t>(i + 1), static_cast<uint16_t>(8100 + i));
      c.io = std::make_shared<pp::adp::MemoryDatagramIo>(hub, addr);
      c.ep = std::make_unique<pp::adp::Endpoint>(c.io, clock);
      auto keys = pp::MlDsa::GenerateKeyPair();
      if (!keys) {
        return false;
      }
      pp::amp::MshIdentity id;
      id.ml_dsa_secret_key = std::move(keys->secret_key);
      id.ml_dsa_public_key = std::move(keys->public_key);
      auto peer = pbr::test::DeriveTestPeerId(id.ml_dsa_public_key);
      if (!peer) {
        return false;
      }
      c.rt = std::make_unique<pp::amp::MeshRuntime>(*c.ep, id, *peer, cfg);
      c.rt->Start();
      if (!static_cast<bool>(c.rt->Links().RegisterEndpoint("hub", *hub_ma))) {
        return false;
      }
      bool done = false;
      bool ok = false;
      c.rt->Links().EnsureAssociation("hub", [&](pp::amp::PeerLinkManager::LinkRoe result) {
        ok = result.isOk();
        done = true;
      });
      for (size_t round = 0; round < 2000 && !(done && c.rt->Links().IsConnected("hub")); ++round) {
        hub_rt->Drive();
        c.rt->Drive();
        for (auto& other : clients) {
          other.rt->Drive();
        }
      }
      if (!ok || !c.rt->Links().IsConnected("hub")) {
        std::fprintf(stderr, "D2 Associate failed client %d\n", i);
        return false;
      }
      clients.push_back(std::move(c));
    }

    std::vector<uint32_t> channels;
    channels.reserve(static_cast<size_t>(kClients));
    for (auto& c : clients) {
      bool done = false;
      std::optional<uint32_t> ch;
      c.rt->Links().OpenChannel("hub", "/pp-perf/fanin/1.0.0", pp::amp::ChatBlobChannelPolicy(),
                                [&](pp::amp::PeerLinkManager::ChannelRoe result) {
                                  if (result.isOk()) {
                                    ch = result.value();
                                  }
                                  done = true;
                                });
      for (size_t round = 0; round < 500 && !done; ++round) {
        hub_rt->Drive();
        for (auto& x : clients) {
          x.rt->Drive();
        }
      }
      if (!ch) {
        std::fprintf(stderr, "D2 OpenChannel failed\n");
        return false;
      }
      channels.push_back(*ch);
    }

    const size_t want = kBytesPerClient * static_cast<size_t>(kClients);
    const auto t0 = std::chrono::steady_clock::now();
    for (size_t ci = 0; ci < clients.size(); ++ci) {
      size_t sent = 0;
      while (sent < kBytesPerClient) {
        const size_t n = std::min(kChunk, kBytesPerClient - sent);
        std::vector<uint8_t> chunk(n, static_cast<uint8_t>(ci));
        auto* link = clients[ci].rt->Links().FindLink("hub");
        if (!link || !link->Mux()) {
          return false;
        }
        for (int attempt = 0; attempt < 64; ++attempt) {
          auto r = link->Mux()->SendData(channels[ci], chunk);
          hub_rt->Drive();
          for (auto& x : clients) {
            x.rt->Drive();
          }
          if (r) {
            break;
          }
          if (attempt == 63) {
            std::fprintf(stderr, "D2 SendData failed: %s\n", r.error().message.c_str());
            return false;
          }
        }
        sent += n;
      }
    }
    for (size_t round = 0; round < 5000 && received < want; ++round) {
      hub_rt->Drive();
      for (auto& x : clients) {
        x.rt->Drive();
      }
    }
    const auto t1 = std::chrono::steady_clock::now();
    if (received != want) {
      std::fprintf(stderr, "D2 incomplete got=%zu want=%zu\n", received, want);
      return false;
    }
    if (iter >= case_warmup) {
      samples.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
    }
  }

  const auto stats = Summarize(std::move(samples));
  const size_t total = kBytesPerClient * static_cast<size_t>(kClients);
  const double mbps = MegaBytesPerSec(stats.mean_ns, total);
  PrintHuman("D2", "fanin_4x64KiB", stats, mbps, "MB/s");
  PrintCsv("D2", "fanin_4x64KiB", total, stats, mbps, "MB/s");
  return true;
}

bool RunF(const int warmup, const int iters) {
  std::printf("\n== F Adversarial sealed-garbage reject cost ==\n");
  constexpr size_t kFlood = 500;
  const int case_warmup = std::min(warmup, 1);
  const int case_iters = std::max(1, iters);
  std::vector<double> samples;

  for (int i = 0; i < case_warmup + case_iters; ++i) {
    auto created = pbr::test::MakeAmpIntegrationHarness();
    if (!created || !(*created)->Associate()) {
      std::fprintf(stderr, "F Associate failed\n");
      return false;
    }
    auto& h = **created;
    const auto ch = h.OpenChannel(pbr::test::HarnessSide::A, "b", "/pp-browser/chat/1.0.0",
                                   pp::amp::ControlJsonChannelPolicy());
    if (!ch) {
      return false;
    }
    const auto t0 = std::chrono::steady_clock::now();
    size_t injected = 0;
    for (uint32_t n = 0; n < kFlood; ++n) {
      if (h.InjectSealedGarbage(pbr::test::HarnessSide::A, "b", *ch, n + 1000)) {
        ++injected;
      }
      // Reliable window is 16 — pump so ACKs drain outstanding garbage frames.
      if ((n & 7u) == 7u) {
        h.PumpBoth();
      }
    }
    h.PumpBudget(40);
    const auto t1 = std::chrono::steady_clock::now();
    if (injected < kFlood / 2) {
      std::fprintf(stderr, "F too few injects: %zu / %zu\n", injected, kFlood);
      return false;
    }
    if (!h.mgr_a().IsConnected("b")) {
      std::fprintf(stderr, "F link dropped under garbage flood\n");
      return false;
    }
    if (i >= case_warmup) {
      samples.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
    }
  }

  const auto stats = Summarize(std::move(samples));
  const double ns_per = stats.mean_ns / static_cast<double>(kFlood);
  PrintHuman("F", "garbage_reject", stats, ns_per, "ns/pkt");
  PrintCsv("F", "garbage_reject", kFlood, stats, ns_per, "ns/pkt");
  return true;
}

} // namespace pp::amp::perf
