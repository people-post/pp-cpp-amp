#include "amp/L1/Clock.h"
#include "amp/L1/Endpoint.h"
#include "amp/L1/MemoryDatagramIo.h"
#include "amp/L1/PacketProtection.h"
#include "amp/L1/Types.h"
#include "amp/L1/WireCodec.h"

#include <gtest/gtest.h>
#include <sodium.h>

#include <algorithm>
#include <vector>

// Wire v2 sealed data (A029 H1): what L1 does once the session hands it keys. The session's real AEAD lives in L2;
// here a test protection (XChaCha20-Poly1305 under a fixed key, the nonce from space and number) stands in for it.

namespace {

using pp::adp::PacketSpace;
using pp::adp::PacketType;

class TestProtection : public pp::adp::IPacketProtection {
public:
  explicit TestProtection(uint8_t key_fill = 0x5a) { key_.fill(key_fill); }

  pp::Roe<void> SealPacket(PacketSpace space, uint32_t number, std::span<const uint8_t> header,
                           std::span<const uint8_t> plaintext, std::vector<uint8_t>& out) const override {
    const auto nonce = Nonce(space, number);
    const size_t at = out.size();
    out.resize(at + plaintext.size() + pp::adp::kPacketTagBytes);
    unsigned long long len = 0;
    crypto_aead_xchacha20poly1305_ietf_encrypt(out.data() + at, &len, plaintext.data(), plaintext.size(), header.data(),
                                               header.size(), nullptr, nonce.data(), key_.data());
    out.resize(at + len);
    ++seals;
    return {};
  }

  pp::Roe<std::vector<uint8_t>> OpenPacket(PacketSpace space, uint32_t number, std::span<const uint8_t> header,
                                           std::span<const uint8_t> sealed, int64_t) const override {
    if (sealed.size() < pp::adp::kPacketTagBytes) {
      return pp::Error("short");
    }
    const auto nonce = Nonce(space, number);
    std::vector<uint8_t> plain(sealed.size());
    unsigned long long len = 0;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(plain.data(), &len, nullptr, sealed.data(), sealed.size(),
                                                   header.data(), header.size(), nonce.data(), key_.data()) != 0) {
      return pp::Error("tag");
    }
    plain.resize(len);
    return plain;
  }

  mutable size_t seals = 0;

private:
  static std::array<uint8_t, 24> Nonce(PacketSpace space, uint32_t number) {
    std::array<uint8_t, 24> nonce{};
    nonce[0] = static_cast<uint8_t>(space);
    std::memcpy(nonce.data() + 1, &number, sizeof(number));
    return nonce;
  }
  std::array<uint8_t, 32> key_{};
};

/** Forwards to an in-memory IO and keeps every datagram it sent. */
class RecordingIo : public pp::adp::DatagramIo {
public:
  explicit RecordingIo(std::shared_ptr<pp::adp::MemoryDatagramIo> inner) : inner_(std::move(inner)) {}
  pp::Roe<void> SendTo(const pp::adp::IpEndpoint& peer, std::span<const uint8_t> datagram) override {
    sent.emplace_back(datagram.begin(), datagram.end());
    if (drop_sends) {
      return {};
    }
    return inner_->SendTo(peer, datagram);
  }
  pp::Roe<std::optional<std::pair<pp::adp::IpEndpoint, std::vector<uint8_t>>>> RecvFrom() override {
    return inner_->RecvFrom();
  }
  pp::adp::IpEndpoint LocalEndpoint() const override { return inner_->LocalEndpoint(); }

  std::vector<std::vector<uint8_t>> sent;
  bool drop_sends = false;

private:
  std::shared_ptr<pp::adp::MemoryDatagramIo> inner_;
};

struct Link {
  std::shared_ptr<pp::adp::VirtualClock> clock = std::make_shared<pp::adp::VirtualClock>(1'000'000);
  std::shared_ptr<pp::adp::MemoryDatagramHub> hub = pp::adp::MemoryDatagramIo::MakeHub();
  pp::adp::IpEndpoint addr_a = pp::adp::IpEndpoint::V4(10, 0, 0, 1, 1000);
  pp::adp::IpEndpoint addr_b = pp::adp::IpEndpoint::V4(10, 0, 0, 2, 2000);
  std::shared_ptr<RecordingIo> io_a =
      std::make_shared<RecordingIo>(std::make_shared<pp::adp::MemoryDatagramIo>(hub, addr_a));
  std::shared_ptr<RecordingIo> io_b =
      std::make_shared<RecordingIo>(std::make_shared<pp::adp::MemoryDatagramIo>(hub, addr_b));
  std::unique_ptr<pp::adp::Endpoint> ep_a = std::make_unique<pp::adp::Endpoint>(io_a, clock);
  std::unique_ptr<pp::adp::Endpoint> ep_b = std::make_unique<pp::adp::Endpoint>(io_b, clock);
  std::shared_ptr<pp::adp::Connection> a;
  std::shared_ptr<pp::adp::Connection> b;
  std::vector<pp::adp::Message> at_b;
  std::vector<pp::adp::Message> at_a;

  Link() {
    pp::adp::OpenParams op;
    op.key.bytes.fill(9);
    op.id.bytes.fill(3);
    op.mint_id = false;
    op.peer = addr_b;
    a = *ep_a->Open(op);
    op.peer = addr_a;
    b = *ep_b->Open(op);
    a->OnMessage([this](const pp::adp::Message& m) { at_a.push_back(m); });
    b->OnMessage([this](const pp::adp::Message& m) { at_b.push_back(m); });
  }
  void Keys(std::shared_ptr<TestProtection> pa = std::make_shared<TestProtection>(),
            std::shared_ptr<TestProtection> pb = std::make_shared<TestProtection>()) {
    a->SetProtection(std::move(pa));
    b->SetProtection(std::move(pb));
  }
  void Pump() {
    for (int i = 0; i < 4; ++i) {
      ep_a->Pump();
      ep_b->Pump();
    }
  }
};

PacketType TypeOf(const std::vector<uint8_t>& datagram) {
  return *pp::adp::WireCodec::PeekType(datagram);
}

class AdpSealedTest : public ::testing::Test {
protected:
  void SetUp() override { ASSERT_GE(sodium_init(), 0); }
};

TEST_F(AdpSealedTest, BeforeKeysDataGoesPlainAfterKeysSealed) {
  Link l;
  const std::vector<uint8_t> hello = {'h', 'i'};
  ASSERT_TRUE(l.a->Send(pp::adp::QosClass::Reliable, hello));
  ASSERT_EQ(TypeOf(l.io_a->sent.back()), PacketType::PlainReliable);
  l.Pump();
  ASSERT_EQ(l.at_b.size(), 1u);
  EXPECT_FALSE(l.at_b[0].sealed);

  l.Keys();
  const std::vector<uint8_t> secret = {'s', 'e', 'c', 'r', 'e', 't'};
  ASSERT_TRUE(l.a->Send(pp::adp::QosClass::Reliable, secret));
  ASSERT_TRUE(l.a->Send(pp::adp::QosClass::BestEffort, secret));
  const auto& rel = l.io_a->sent[l.io_a->sent.size() - 2];
  const auto& be = l.io_a->sent.back();
  EXPECT_EQ(TypeOf(rel), PacketType::DataReliable);
  EXPECT_EQ(TypeOf(be), PacketType::DataBestEffort);
  // Clear header, sealed payload, tag: no HMAC, no timestamp, and the payload is not on the wire.
  EXPECT_EQ(rel.size(), pp::adp::kSealedHeaderBytes + secret.size() + pp::adp::kPacketTagBytes);
  EXPECT_EQ(std::search(rel.begin(), rel.end(), secret.begin(), secret.end()), rel.end());
  l.Pump();
  ASSERT_EQ(l.at_b.size(), 3u);
  EXPECT_TRUE(l.at_b[1].sealed);
  EXPECT_EQ(l.at_b[1].payload, secret);
  EXPECT_TRUE(l.at_b[2].sealed);
  EXPECT_EQ(l.at_b[2].qos, pp::adp::QosClass::BestEffort);
}

// A sealed packet's 29 bytes of header and tag replace v1's 44 of header and HMAC (A029 H1).
TEST_F(AdpSealedTest, ASealedPacketCostsTwentyNineBytesAroundItsPayload) {
  EXPECT_EQ(pp::adp::kSealedHeaderBytes + pp::adp::kPacketTagBytes, 29u);
  EXPECT_EQ(pp::adp::kHeaderBytes + pp::adp::kHmacBytes, 33u);
}

TEST_F(AdpSealedTest, ATamperedSealedPacketIsRejectedAndMovesNothing) {
  Link l;
  l.Keys();
  l.io_a->drop_sends = true;
  ASSERT_TRUE(l.a->Send(pp::adp::QosClass::BestEffort, std::vector<uint8_t>{1, 2, 3}));
  auto forged = l.io_a->sent.back();
  forged[pp::adp::kSealedHeaderBytes] ^= 0x01; // body
  const auto elsewhere = pp::adp::IpEndpoint::V4(10, 9, 9, 9, 9);
  l.b->HandleDatagram(elsewhere, forged, l.clock->NowMs());
  auto renumbered = l.io_a->sent.back();
  renumbered[pp::adp::kSealedHeaderBytes - 1] ^= 0x01; // the packet number, under the tag as associated data
  l.b->HandleDatagram(elsewhere, renumbered, l.clock->NowMs());
  EXPECT_TRUE(l.at_b.empty());
  EXPECT_EQ(l.b->PeerEndpoint(), l.addr_a) << "a packet that did not open moves no path";
  EXPECT_EQ(l.ep_b->Stats().rx_rejected, 2u);
  l.b->HandleDatagram(elsewhere, l.io_a->sent.back(), l.clock->NowMs());
  EXPECT_EQ(l.at_b.size(), 1u);
  EXPECT_EQ(l.b->PeerEndpoint(), elsewhere) << "the genuine one does";
}

TEST_F(AdpSealedTest, ADifferentKeyDoesNotOpen) {
  Link l;
  l.Keys(std::make_shared<TestProtection>(0x01), std::make_shared<TestProtection>(0x02));
  ASSERT_TRUE(l.a->Send(pp::adp::QosClass::BestEffort, std::vector<uint8_t>{1}));
  l.Pump();
  EXPECT_TRUE(l.at_b.empty());
}

TEST_F(AdpSealedTest, AReplayedSealedPacketIsDeliveredOnce) {
  Link l;
  l.Keys();
  l.io_a->drop_sends = true;
  ASSERT_TRUE(l.a->Send(pp::adp::QosClass::BestEffort, std::vector<uint8_t>{7}));
  const auto datagram = l.io_a->sent.back();
  l.b->HandleDatagram(l.addr_a, datagram, l.clock->NowMs());
  l.b->HandleDatagram(l.addr_a, datagram, l.clock->NowMs());
  EXPECT_EQ(l.at_b.size(), 1u);
}

// The peer that finishes the handshake first may send sealed data before the other has keys: it is not taken (and
// not acked), and its resend lands once the keys are there.
TEST_F(AdpSealedTest, SealedDataBeforeTheReceiverHasKeysArrivesOnResend) {
  Link l;
  l.a->SetProtection(std::make_shared<TestProtection>());
  ASSERT_TRUE(l.a->Send(pp::adp::QosClass::Reliable, std::vector<uint8_t>{4, 2}));
  l.Pump();
  EXPECT_TRUE(l.at_b.empty());
  EXPECT_EQ(l.a->ReliableCreditsRemaining(), pp::adp::kDefaultReliableWindow - 1) << "not acked";
  l.b->SetProtection(std::make_shared<TestProtection>());
  l.clock->Advance(1000);
  l.a->Tick(l.clock->NowMs());
  l.Pump();
  ASSERT_EQ(l.at_b.size(), 1u);
  EXPECT_TRUE(l.at_b[0].sealed);
  EXPECT_EQ(l.a->ReliableCreditsRemaining(), pp::adp::kDefaultReliableWindow);
}

// A resend under the same key is the same packet: the same bytes (the nonce is the packet number, and it seals the
// same plaintext), never the same number over other bytes.
TEST_F(AdpSealedTest, AResendIsByteForByteTheFirstSend) {
  Link l;
  l.Keys();
  l.io_a->drop_sends = true;
  ASSERT_TRUE(l.a->Send(pp::adp::QosClass::Reliable, std::vector<uint8_t>{1, 2, 3, 4}));
  const auto first = l.io_a->sent.back();
  l.clock->Advance(1000);
  l.a->Tick(l.clock->NowMs());
  ASSERT_GE(l.io_a->sent.size(), 2u);
  EXPECT_EQ(l.io_a->sent.back(), first);
}

// Once the session has keys nothing new is taken in the clear; the handshake's own reliable packet resent late is
// acked (so its sender stops) but not delivered again.
TEST_F(AdpSealedTest, PlainDataAfterKeysIsRefusedButAHandshakeDuplicateIsAcked) {
  Link l;
  l.io_a->drop_sends = true;
  ASSERT_TRUE(l.a->Send(pp::adp::QosClass::Reliable, std::vector<uint8_t>{'m', 's', 'h'}));
  const auto handshake = l.io_a->sent.back();
  l.b->HandleDatagram(l.addr_a, handshake, l.clock->NowMs());
  ASSERT_EQ(l.at_b.size(), 1u);
  l.b->SetProtection(std::make_shared<TestProtection>());

  const size_t b_sent = l.io_b->sent.size();
  l.b->HandleDatagram(l.addr_a, handshake, l.clock->NowMs());
  ASSERT_EQ(l.io_b->sent.size(), b_sent + 1);
  EXPECT_EQ(TypeOf(l.io_b->sent.back()), PacketType::Ack);
  ASSERT_TRUE(l.a->Send(pp::adp::QosClass::Reliable, std::vector<uint8_t>{'n', 'e', 'w'}));
  ASSERT_TRUE(l.a->Send(pp::adp::QosClass::BestEffort, std::vector<uint8_t>{'b', 'e'}));
  l.b->HandleDatagram(l.addr_a, l.io_a->sent[l.io_a->sent.size() - 2], l.clock->NowMs());
  l.b->HandleDatagram(l.addr_a, l.io_a->sent.back(), l.clock->NowMs());
  EXPECT_EQ(l.at_b.size(), 1u);
  EXPECT_EQ(l.io_b->sent.size(), b_sent + 1) << "a fresh plain packet is not acked";
}

// A new association starts only from a plain handshake packet: a sealed datagram for an unknown id creates nothing.
TEST_F(AdpSealedTest, ASealedDatagramForAnUnknownIdIsNoNewAssociation) {
  Link l;
  pp::adp::PeerKey accept_key;
  accept_key.bytes.fill(9);
  l.ep_b->SetAcceptKey(accept_key);
  l.ep_b->SetAcceptEnabled(true);
  size_t accepted = 0;
  l.ep_b->SetAcceptHandler([&](std::shared_ptr<pp::adp::Connection>) { ++accepted; });
  pp::adp::SealedHeader header;
  header.type = PacketType::DataReliable;
  header.assoc.bytes.fill(0x77);
  header.number = 1;
  const auto clear = pp::adp::WireCodec::EncodeSealedHeader(header);
  std::vector<uint8_t> datagram(clear.begin(), clear.end());
  datagram.resize(datagram.size() + 8 + pp::adp::kPacketTagBytes, 0x33);
  ASSERT_TRUE(l.io_a->SendTo(l.addr_b, datagram));
  l.Pump();
  EXPECT_EQ(accepted, 0u);
  EXPECT_EQ(l.ep_b->Find(header.assoc), nullptr);
}

} // namespace
