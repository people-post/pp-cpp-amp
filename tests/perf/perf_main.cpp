#include "amp/L1/HmacBinder.h"
#include "amp/L1/Types.h"
#include "amp/L1/WireCodec.h"
#include "amp/L2/SessionCrypto.h"
#include "amp/L2/Types.h"
#include "amp/L3/AmpChannelLimits.h"
#include "amp/L3/ChannelMux.h"
#include "amp/L3/ChannelPolicy.h"
#include "amp/L3/ChannelSession.h"
#include "amp/L3/Types.h"
#include "perf_report.h"
#include "support/amp_integration_harness.h"

#include "L3/tests/amp_test_link.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <sodium.h>

namespace {

using pp::amp::perf::EnvInt;
using pp::amp::perf::MegaBytesPerSec;
using pp::amp::perf::OpsPerSec;
using pp::amp::perf::PrintCsv;
using pp::amp::perf::PrintHuman;
using pp::amp::perf::SampleStats;
using pp::amp::perf::Summarize;
using pp::amp::perf::TimeLoop;
using HarnessSide = pbr::test::HarnessSide;

pp::adp::PeerKey AdpKey(const uint8_t fill = 0x42) {
  pp::adp::PeerKey k;
  k.bytes.fill(fill);
  return k;
}

pp::adp::AssocId AdpAssoc(const uint8_t fill = 0x11) {
  pp::adp::AssocId id;
  id.bytes.fill(fill);
  return id;
}

pp::amp::ByteVector SessionKey(const uint8_t seed) {
  return pp::amp::ByteVector(pp::amp::kSessionKeyBytes, seed);
}

pp::amp::ChannelPolicy BulkPolicy() {
  return pp::amp::BulkChannelPolicy();
}

bool RunA1(const int warmup, const int iters) {
  std::printf("\n== A1 WireCodec + HmacBinder ==\n");
  const size_t payloads[] = {0, 600, pp::adp::kMaxPayload};
  bool ok = true;
  for (const size_t payload_len : payloads) {
    pp::adp::WirePacket pkt;
    pkt.type = pp::adp::PacketType::PlainBestEffort;
    pkt.assoc = AdpAssoc();
    pkt.seq = 1;
    pkt.timestamp_ms = 1;
    pkt.payload.assign(payload_len, 0xAB);
    auto encoded = pp::adp::WireCodec::Encode(pkt);
    if (!encoded) {
      std::fprintf(stderr, "A1 encode failed payload=%zu\n", payload_len);
      return false;
    }
    pp::adp::HmacBinder binder(AdpKey());
    auto sealed = binder.Seal(*encoded);
    if (!sealed) {
      std::fprintf(stderr, "A1 seal failed payload=%zu\n", payload_len);
      return false;
    }
    if (!binder.Verify(*sealed)) {
      std::fprintf(stderr, "A1 verify failed payload=%zu\n", payload_len);
      return false;
    }

    auto samples = TimeLoop(warmup, iters, [&] {
      auto enc = pp::adp::WireCodec::Encode(pkt);
      if (!enc) {
        ok = false;
        return;
      }
      auto s = binder.Seal(*enc);
      if (!s) {
        ok = false;
        return;
      }
      if (!binder.Verify(*s)) {
        ok = false;
      }
    });
    const auto stats = Summarize(std::move(samples));
    const double ops = OpsPerSec(stats.mean_ns);
    const std::string label = "payload_" + std::to_string(payload_len);
    PrintHuman("A1", label.c_str(), stats, ops, "pkt/s");
    PrintCsv("A1", label.c_str(), payload_len, stats, ops, "pkt/s");
  }
  return ok;
}

bool RunA2(const int warmup, const int iters) {
  std::printf("\n== A2 SessionCrypto SealPacket/OpenPacket ==\n");
  const size_t payloads[] = {64, 900, 4096};
  bool ok = true;
  const auto key = SessionKey(0x55);
  uint32_t seq = 1;
  for (const size_t payload_len : payloads) {
    std::vector<uint8_t> plain(payload_len, 0xCD);
    const std::array<uint8_t, pp::adp::kSealedHeaderBytes> header{};
    const auto dir = pp::amp::Direction::InitiatorToResponder;
    const auto space = pp::adp::PacketSpace::BestEffort;
    std::vector<uint8_t> sealed0;
    if (!pp::amp::SessionCrypto::SealPacket(key, dir, 1, space, seq, header, plain, sealed0)) {
      std::fprintf(stderr, "A2 seal failed payload=%zu\n", payload_len);
      return false;
    }
    auto opened0 = pp::amp::SessionCrypto::OpenPacket(key, dir, 1, space, seq, header, sealed0);
    if (!opened0 || *opened0 != plain) {
      std::fprintf(stderr, "A2 open failed payload=%zu\n", payload_len);
      return false;
    }

    auto samples = TimeLoop(warmup, iters, [&] {
      const uint32_t number = seq++;
      std::vector<uint8_t> sealed;
      if (!pp::amp::SessionCrypto::SealPacket(key, dir, 1, space, number, header, plain, sealed)) {
        ok = false;
        return;
      }
      auto opened = pp::amp::SessionCrypto::OpenPacket(key, dir, 1, space, number, header, sealed);
      if (!opened || *opened != plain) {
        ok = false;
      }
    });
    const auto stats = Summarize(std::move(samples));
    const double mbps = MegaBytesPerSec(stats.mean_ns, payload_len);
    const std::string label = "payload_" + std::to_string(payload_len);
    PrintHuman("A2", label.c_str(), stats, mbps, "MB/s");
    PrintCsv("A2", label.c_str(), payload_len, stats, mbps, "MB/s");
  }
  return ok;
}

bool RunC1(const int warmup, const int iters) {
  std::printf("\n== C1 EnsureAssociation (MemoryDatagramIo) ==\n");
  bool ok = true;
  const int case_warmup = std::min(warmup, 1);
  const int case_iters = std::max(1, iters);
  auto samples = TimeLoop(case_warmup, case_iters, [&] {
    auto created = pbr::test::MakeAmpIntegrationHarness();
    if (!created) {
      ok = false;
      return;
    }
    if (!(*created)->Associate()) {
      ok = false;
    }
  });
  if (!ok) {
    std::fprintf(stderr, "C1 Associate failed\n");
    return false;
  }
  const auto stats = Summarize(std::move(samples));
  PrintHuman("C1", "associate_cap_open", stats, stats.mean_ns / 1e6, "ms");
  PrintCsv("C1", "associate_cap_open", 0, stats, stats.mean_ns / 1e6, "ms");
  return true;
}

bool RunC2(const int warmup, const int iters) {
  std::printf("\n== C2 OpenChannel + first DATA ==\n");
  const int case_warmup = std::min(warmup, 1);
  const int case_iters = std::max(1, iters);
  std::vector<double> open_data_ns;
  open_data_ns.reserve(static_cast<size_t>(case_iters));

  for (int i = 0; i < case_warmup + case_iters; ++i) {
    auto created = pbr::test::MakeAmpIntegrationHarness();
    if (!created || !(*created)->Associate()) {
      std::fprintf(stderr, "C2 setup Associate failed\n");
      return false;
    }
    auto& h = **created;
    const auto t0 = std::chrono::steady_clock::now();
    const auto ch = h.OpenChannel(HarnessSide::A, "b", "/test/chat/1.0.0", pp::amp::ControlJsonChannelPolicy());
    if (!ch) {
      std::fprintf(stderr, "C2 OpenChannel failed\n");
      return false;
    }
    std::vector<uint8_t> received;
    auto* inbound = h.mgr_b().FindLinkByPeerId(h.peer_id_a);
    if (!inbound || !inbound->Mux()) {
      std::fprintf(stderr, "C2 inbound mux missing\n");
      return false;
    }
    inbound->Mux()->SetDataHandler(*ch, [&](uint32_t, std::vector<uint8_t> payload) { received = std::move(payload); });
    std::vector<uint8_t> msg = {'p', 'i', 'n', 'g'};
    if (!h.SendMuxData(HarnessSide::A, "b", *ch, msg) ||
        !h.PumpUntilReceived(received, [&] { return received == msg; })) {
      std::fprintf(stderr, "C2 first DATA failed\n");
      return false;
    }
    const auto t1 = std::chrono::steady_clock::now();
    if (i >= case_warmup) {
      open_data_ns.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
    }
  }

  const auto stats = Summarize(std::move(open_data_ns));
  PrintHuman("C2", "open_first_data", stats, stats.mean_ns / 1e6, "ms");
  PrintCsv("C2", "open_first_data", 4, stats, stats.mean_ns / 1e6, "ms");
  return true;
}

bool RunC3(const int warmup, const int iters) {
  std::printf("\n== C3 ChannelMux FRAG (AmpTestLink) ==\n");
  const size_t payloads[] = {900, 64 * 1024, 256 * 1024, 512 * 1024, 4ull * 1024ull * 1024ull};
  bool ok = true;

  for (const size_t payload_len : payloads) {
    auto link_result = pp::amp::test::AmpTestLink::Create();
    if (!link_result) {
      std::fprintf(stderr, "C3 AmpTestLink create failed\n");
      return false;
    }
    auto& link = **link_result;
    auto ch = link.initiator.mux.OpenOutbound("/test/chat-blob/1.0.0", BulkPolicy());
    if (!ch) {
      std::fprintf(stderr, "C3 OpenOutbound failed: %s\n", ch.error().message.c_str());
      return false;
    }

    std::vector<uint8_t> received;
    link.responder.mux.SetDataHandler(*ch,
                                      [&](uint32_t, std::vector<uint8_t> payload) { received = std::move(payload); });

    std::vector<uint8_t> large(payload_len, 0xAB);
    auto sent = link.initiator.mux.SendData(*ch, large);
    if (!sent) {
      std::fprintf(stderr, "C3 SendData failed payload=%zu: %s\n", payload_len, sent.error().message.c_str());
      return false;
    }
    if (received != large) {
      std::fprintf(stderr, "C3 reassembly mismatch payload=%zu received=%zu\n", payload_len, received.size());
      return false;
    }

    const int case_warmup = std::min(warmup, 2);
    const int case_iters =
        payload_len >= 4ull * 1024ull * 1024ull ? std::max(1, std::min(iters, 2)) : std::max(1, iters);
    auto samples = TimeLoop(case_warmup, case_iters, [&] {
      received.clear();
      if (!static_cast<bool>(link.initiator.mux.SendData(*ch, large))) {
        ok = false;
        return;
      }
      if (received != large) {
        ok = false;
      }
    });
    const auto stats = Summarize(std::move(samples));
    const double mbps = MegaBytesPerSec(stats.mean_ns, payload_len);
    const size_t frags = (payload_len + 899) / 900;
    const std::string label = "frag_" + std::to_string(payload_len) + "_frags_" + std::to_string(frags);
    PrintHuman("C3", label.c_str(), stats, mbps, "MB/s");
    PrintCsv("C3", label.c_str(), payload_len, stats, mbps, "MB/s");
  }
  return ok;
}

bool RunD1(const int warmup, const int iters) {
  std::printf("\n== D1 MeshRuntime Drive cost (1 link) ==\n");
  auto created = pbr::test::MakeAmpIntegrationHarness();
  if (!created || !(*created)->Associate()) {
    std::fprintf(stderr, "D1 Associate failed\n");
    return false;
  }
  auto& h = **created;
  h.mgr_a().MarkHot("b");

  const int case_warmup = std::min(warmup, 20);
  const int case_iters = std::max(1, iters);
  auto samples = TimeLoop(case_warmup, case_iters, [&] {
    h.runtime_a->Drive();
    h.runtime_b->Drive();
  });
  const auto stats = Summarize(std::move(samples));
  PrintHuman("D1", "drive_pair_1link", stats, stats.mean_ns / 1e3, "us");
  PrintCsv("D1", "drive_pair_1link", 1, stats, stats.mean_ns / 1e3, "us");
  return true;
}

} // namespace

int main() {
  if (sodium_init() < 0) {
    std::fprintf(stderr, "sodium_init failed\n");
    return 1;
  }

  const int warmup = EnvInt("PP_AMP_PERF_WARMUP", 50);
  const int micro_iters = EnvInt("PP_AMP_PERF_ITERS", 2000);
  const int frag_iters = EnvInt("PP_AMP_PERF_FRAG_ITERS", 8);
  const int assoc_iters = EnvInt("PP_AMP_PERF_ASSOC_ITERS", 3);
  const int drive_iters = EnvInt("PP_AMP_PERF_DRIVE_ITERS", 2000);
  const int media_iters = EnvInt("PP_AMP_PERF_MEDIA_ITERS", 8);
  const int xfer_iters = EnvInt("PP_AMP_PERF_XFER_ITERS", 3);

  std::printf(
      "pp_amp_perf  warmup=%d  micro_iters=%d  frag_iters=%d  assoc_iters=%d  drive_iters=%d  media_iters=%d  "
      "xfer_iters=%d\n",
      warmup, micro_iters, frag_iters, assoc_iters, drive_iters, media_iters, xfer_iters);
  std::printf("csv,case,label,payload_bytes,n,p50_ns,p95_ns,p99_ns,mean_ns,throughput,throughput_unit\n");

  bool ok = true;
  ok = RunA1(warmup, micro_iters) && ok;
  ok = RunA2(warmup, micro_iters) && ok;
  ok = pp::amp::perf::RunA3(warmup, micro_iters) && ok;
  ok = pp::amp::perf::RunB1(std::min(warmup, 1), xfer_iters) && ok;
  ok = pp::amp::perf::RunB2(std::min(warmup, 1), xfer_iters) && ok;
  ok = pp::amp::perf::RunB3(warmup, media_iters) && ok;
  ok = pp::amp::perf::RunOsUdp(std::min(warmup, 1), xfer_iters) && ok;
  ok = RunC1(std::min(warmup, 1), assoc_iters) && ok;
  ok = RunC2(std::min(warmup, 1), assoc_iters) && ok;
  ok = RunC3(warmup, frag_iters) && ok;
  ok = pp::amp::perf::RunC5(std::min(warmup, 2), std::max(1, frag_iters)) && ok;
  ok = RunD1(warmup, drive_iters) && ok;
  ok = pp::amp::perf::RunD1Multi(warmup, std::min(drive_iters, 500)) && ok;
  ok = pp::amp::perf::RunD2(std::min(warmup, 1), xfer_iters) && ok;
  ok = pp::amp::perf::RunD3(std::min(warmup, 1), xfer_iters) && ok;
  ok = pp::amp::perf::RunE1(std::min(warmup, 1), xfer_iters) && ok;
  ok = pp::amp::perf::RunE2(std::min(warmup, 1), media_iters) && ok;
  ok = pp::amp::perf::RunE3(std::min(warmup, 1), xfer_iters) && ok;
  ok = pp::amp::perf::RunE4(std::min(warmup, 1), assoc_iters) && ok;
  ok = pp::amp::perf::RunF(std::min(warmup, 1), xfer_iters) && ok;
  ok = pp::amp::perf::RunOsUdpAmp(std::min(warmup, 1), xfer_iters) && ok;

  std::printf("\n%s\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
