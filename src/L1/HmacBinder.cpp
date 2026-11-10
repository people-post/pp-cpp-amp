#include "amp/L1/HmacBinder.h"

#include <sodium.h>

#include <cstring>

namespace pp::adp {

namespace {

std::array<uint8_t, kHmacBytes> Truncate(const HmacSha256::Tag& full) {
  std::array<uint8_t, kHmacBytes> tag{};
  std::memcpy(tag.data(), full.data(), kHmacBytes);
  return tag;
}

} // namespace

HmacBinder::HmacBinder(PeerKey key) : key_(key), mac_(key_.bytes) {}

void HmacBinder::SetKey(PeerKey key) {
  key_ = key;
  mac_ = HmacSha256(key_.bytes);
}

std::array<uint8_t, kHmacBytes> HmacBinder::ComputeTag(const PeerKey& key, std::span<const uint8_t> mac_input) {
  return Truncate(HmacSha256::Compute(key.bytes, mac_input));
}

Roe<std::vector<uint8_t>> HmacBinder::Seal(const std::span<const uint8_t> header_and_payload) const {
  const auto tag = Truncate(mac_.Mac(header_and_payload));
  std::vector<uint8_t> out;
  out.reserve(header_and_payload.size() + kHmacBytes);
  out.assign(header_and_payload.begin(), header_and_payload.end());
  out.insert(out.end(), tag.begin(), tag.end());
  return out;
}

Roe<std::vector<uint8_t>> HmacBinder::Seal(std::vector<uint8_t>&& header_and_payload) const {
  const auto tag = Truncate(mac_.Mac(header_and_payload));
  header_and_payload.insert(header_and_payload.end(), tag.begin(), tag.end());
  return std::move(header_and_payload);
}

Roe<void> HmacBinder::Verify(std::span<const uint8_t> datagram) const {
  if (datagram.size() < kHmacBytes) {
    return Error("adp: no hmac");
  }
  const auto mac_input = datagram.first(datagram.size() - kHmacBytes);
  const auto expected = Truncate(mac_.Mac(mac_input));
  const uint8_t* got = datagram.data() + mac_input.size();
  if (sodium_memcmp(expected.data(), got, kHmacBytes) != 0) {
    return Error("adp: hmac mismatch");
  }
  return {};
}

} // namespace pp::adp
