#include "amp/link/AdpMultiaddr.h"

#include "amp/link/Types.h"

#include <charconv>
#include <sstream>
#include <vector>

namespace pp::amp {

namespace {

std::vector<std::string_view> Split(std::string_view text, char delim) {
  std::vector<std::string_view> out;
  size_t start = 0;
  while (start <= text.size()) {
    const size_t pos = text.find(delim, start);
    if (pos == std::string_view::npos) {
      out.push_back(text.substr(start));
      break;
    }
    out.push_back(text.substr(start, pos - start));
    start = pos + 1;
  }
  return out;
}

Roe<uint16_t> ParsePort(std::string_view text) {
  uint16_t port = 0;
  const auto* begin = text.data();
  const auto* end = begin + text.size();
  const auto res = std::from_chars(begin, end, port);
  if (res.ec != std::errc{} || res.ptr != end || port == 0) {
    return Error("amp addr: bad udp port");
  }
  return port;
}

Roe<adp::IpEndpoint> ParseIp4HostPort(std::string_view host, std::string_view port_text) {
  const auto parts = Split(host, '.');
  if (parts.size() != 4) {
    return Error("amp addr: bad ipv4 host");
  }
  adp::IpEndpoint ep;
  ep.family = adp::IpEndpoint::Family::V4;
  for (size_t i = 0; i < 4; ++i) {
    unsigned value = 0;
    const auto* begin = parts[i].data();
    const auto* end = begin + parts[i].size();
    const auto res = std::from_chars(begin, end, value);
    if (res.ec != std::errc{} || res.ptr != end || value > 255u) {
      return Error("amp addr: bad ipv4 octet");
    }
    ep.addr[i] = static_cast<uint8_t>(value);
  }
  auto port = ParsePort(port_text);
  if (!port) {
    return port.error();
  }
  ep.port = *port;
  return ep;
}

} // namespace

Roe<ParsedAdpMultiaddr> ParseAdpMultiaddr(const std::string_view multiaddr) {
  if (multiaddr.empty() || multiaddr.front() != '/') {
    return Error("amp addr: expected leading slash");
  }
  const auto parts = Split(multiaddr.substr(1), '/');
  if (parts.size() != 8) {
    return Error("amp addr: expected 8 components");
  }
  if (parts[0] != "ip4" || parts[2] != "udp" || parts[4] != kAdpMultiaddrProtocol || parts[5] != kAdpMultiaddrVersion
      || parts[6] != "p2p") {
    return Error("amp addr: unsupported multiaddr layout");
  }
  auto endpoint = ParseIp4HostPort(parts[1], parts[3]);
  if (!endpoint) {
    return endpoint.error();
  }
  if (parts[7].empty()) {
    return Error("amp addr: missing peer id");
  }
  ParsedAdpMultiaddr out;
  out.endpoint = *endpoint;
  out.peer_id.assign(parts[7].begin(), parts[7].end());
  return out;
}

Roe<std::string> FormatAdpMultiaddr(const adp::IpEndpoint& endpoint, const std::string_view peer_id) {
  if (endpoint.family != adp::IpEndpoint::Family::V4) {
    return Error("amp addr: only ipv4 supported");
  }
  if (peer_id.empty()) {
    return Error("amp addr: missing peer id");
  }
  std::ostringstream out;
  out << "/ip4/" << static_cast<int>(endpoint.addr[0]) << '.' << static_cast<int>(endpoint.addr[1]) << '.'
      << static_cast<int>(endpoint.addr[2]) << '.' << static_cast<int>(endpoint.addr[3]) << "/udp/" << endpoint.port
      << '/' << kAdpMultiaddrProtocol << '/' << kAdpMultiaddrVersion << "/p2p/" << peer_id;
  return out.str();
}

} // namespace pp::amp
