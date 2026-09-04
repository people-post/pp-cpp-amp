#include "amp/L1/HmacBinder.h"
#include "amp/L1/Types.h"
#include "amp/L1/WireCodec.h"
#include "amp/L2/SessionCrypto.h"
#include "amp/L2/Types.h"
#include "amp/L3/AmpChannelLimits.h"
#include "amp/L3/ChannelMux.h"
#include "amp/L3/ChannelPolicy.h"
#include "amp/L3/Types.h"
#include "perf_timer.h"

#include "L3/tests/amp_test_link.h"

#include <cstdio>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <sodium.h>

namespace {

using pp::amp::perf::EnvInt;
using pp::amp::perf::MegaBytesPerSec;
using pp::amp::perf::OpsPerSec;
using pp::amp::perf::SampleStats;
using pp::amp::perf::Summarize;
using pp::amp::perf::TimeLoop;

void PrintHuman(const char* case_id, const char* label, const SampleStats& s, const double throughput,
                const char* throughput_unit) {
  std::printf("%s  %-28s  n=%zu  p50=%.0fns  p95=%.0fns  p99=%.0fns  mean=%.0fns  %s=%.2f\n", case_id, label, s.n,
              s.p50_ns, s.p95_ns, s.p99_ns, s.mean_ns, throughput_unit, throughput);
}

void PrintCsv(const char* case_id, const char* label, const size_t payload_bytes, const SampleStats& s,
              const double throughput, const char* throughput_unit) {
  std::printf("csv,%s,%s,%zu,%zu,%.3f,%.3f,%.3f,%.3f,%.6f,%s\n", case_id, label, payload_bytes, s.n, s.p50_ns, s.p95_ns,
              s.p99_ns, s.mean_ns, throughput, throughput_unit);
}

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

pp::amp::ByteVector SessionKey(const uint8_t seed) { return pp::amp::ByteVector(pp::amp::kSessionKeyBytes, seed); }

pp::amp::ChannelPolicy BulkPolicy() {
  pp::amp::ChannelPolicy policy;
  policy.cls = pp::amp::ChannelClass::Bulk;
  policy.drop = pp::amp::ChannelDropPolicy::Never;
  policy.max_outbound_frames = pp::amp::AmpChannelLimits::kMaxControlOutboundFrames;
  policy.max_message_bytes = pp::amp::AmpChannelLimits::kMaxChatBlobFrameBytes;
  return policy;
}

bool RunA1(const int warmup, const int iters) {
  std::printf("\n== A1 WireCodec + HmacBinder ==\n");
  const size_t payloads[] = {0, 600, 1200};
  bool ok = true;
  for (const size_t payload_len : payloads) {
    pp::adp::WirePacket pkt;
    pkt.type = pp::adp::PacketType::DataBestEffort;
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
  std::printf("\n== A2 SessionCrypto Seal/Open ==\n");
  const size_t payloads[] = {64, 900, 4096};
  bool ok = true;
  const auto key = SessionKey(0x55);
  uint32_t seq = 1;
  for (const size_t payload_len : payloads) {
    std::vector<uint8_t> plain(payload_len, 0xCD);
    auto sealed0 = pp::amp::SessionCrypto::Seal(key, 1, 7, seq, pp::amp::Direction::InitiatorToResponder, plain);
    if (!sealed0) {
      std::fprintf(stderr, "A2 seal failed payload=%zu\n", payload_len);
      return false;
    }
    auto opened0 =
        pp::amp::SessionCrypto::Open(key, 1, 7, seq, pp::amp::Direction::InitiatorToResponder, *sealed0);
    if (!opened0 || *opened0 != plain) {
      std::fprintf(stderr, "A2 open failed payload=%zu\n", payload_len);
      return false;
    }

    auto samples = TimeLoop(warmup, iters, [&] {
      const uint32_t channel_seq = seq++;
      auto sealed =
          pp::amp::SessionCrypto::Seal(key, 1, 7, channel_seq, pp::amp::Direction::InitiatorToResponder, plain);
      if (!sealed) {
        ok = false;
        return;
      }
      auto opened =
          pp::amp::SessionCrypto::Open(key, 1, 7, channel_seq, pp::amp::Direction::InitiatorToResponder, *sealed);
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

bool RunC3(const int warmup, const int iters) {
  std::printf("\n== C3 ChannelMux FRAG (AmpTestLink) ==\n");
  // Cap at 256 KiB: OPEN does not wire max_message_bytes, so the responder keeps the
  // ChannelPolicy default (kMaxChatStreamJsonBytes). 512 KiB+ needs a product/policy fix.
  const size_t payloads[] = {900, 64 * 1024, 128 * 1024, 256 * 1024};
  bool ok = true;

  for (const size_t payload_len : payloads) {
    auto link_result = pp::amp::test::AmpTestLink::Create();
    if (!link_result) {
      std::fprintf(stderr, "C3 AmpTestLink create failed\n");
      return false;
    }
    auto& link = **link_result;
    auto ch = link.initiator.mux.OpenOutbound("/pp-browser/chat-blob/1.0.0", BulkPolicy());
    if (!ch) {
      std::fprintf(stderr, "C3 OpenOutbound failed: %s\n", ch.error().message.c_str());
      return false;
    }

    std::vector<uint8_t> received;
    link.responder.mux.SetDataHandler(*ch, [&](uint32_t, std::vector<uint8_t> payload) {
      received = std::move(payload);
    });

    std::vector<uint8_t> large(payload_len, 0xAB);
    // Correctness once before timing.
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
    const int case_iters = std::max(1, iters);
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

} // namespace

int main() {
  if (sodium_init() < 0) {
    std::fprintf(stderr, "sodium_init failed\n");
    return 1;
  }

  const int warmup = EnvInt("PP_AMP_PERF_WARMUP", 50);
  const int micro_iters = EnvInt("PP_AMP_PERF_ITERS", 2000);
  const int frag_iters = EnvInt("PP_AMP_PERF_FRAG_ITERS", 8);

  std::printf("pp_amp_perf  warmup=%d  micro_iters=%d  frag_iters=%d\n", warmup, micro_iters, frag_iters);
  std::printf("csv,case,label,payload_bytes,n,p50_ns,p95_ns,p99_ns,mean_ns,throughput,throughput_unit\n");

  bool ok = true;
  ok = RunA1(warmup, micro_iters) && ok;
  ok = RunA2(warmup, micro_iters) && ok;
  ok = RunC3(warmup, frag_iters) && ok;

  std::printf("\n%s\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
