#include "amp/link/DialBook.h"

namespace pp::amp {

Roe<void> DialBook::RegisterEndpoint(const DialKey& peer_key, const std::string& multiaddr) {
  auto parsed = ParseAdpMultiaddr(multiaddr);
  if (!parsed) {
    return parsed.error();
  }
  EndpointRecord rec;
  rec.multiaddr = multiaddr;
  rec.endpoint = parsed->endpoint;
  rec.peer_id = parsed->peer_id.empty() ? peer_key : parsed->peer_id;
  endpoints_[peer_key] = std::move(rec);
  return {};
}

std::optional<std::string> DialBook::PreferredMultiaddr(const std::string& peer_id) const {
  if (peer_id.empty()) {
    return std::nullopt;
  }
  for (const auto& [key, rec] : endpoints_) {
    if (rec.peer_id == peer_id || key == peer_id) {
      return rec.multiaddr;
    }
  }
  return std::nullopt;
}

const DialBook::EndpointRecord* DialBook::Find(const DialKey& peer_key) const {
  auto it = endpoints_.find(peer_key);
  return it == endpoints_.end() ? nullptr : &it->second;
}

DialBook::EndpointRecord* DialBook::Find(const DialKey& peer_key) {
  auto it = endpoints_.find(peer_key);
  return it == endpoints_.end() ? nullptr : &it->second;
}

void DialBook::ArmBackoff(const DialKey& peer_key) {
  dial_failed_until_[peer_key] = std::chrono::steady_clock::now() + config_.dial_failure_backoff;
}

bool DialBook::InBackoff(const DialKey& peer_key, std::chrono::steady_clock::time_point now) const {
  auto it = dial_failed_until_.find(peer_key);
  return it != dial_failed_until_.end() && it->second > now;
}

std::optional<std::chrono::milliseconds> DialBook::BackoffRemaining(
    const DialKey& peer_key, std::chrono::steady_clock::time_point now) const {
  auto it = dial_failed_until_.find(peer_key);
  if (it == dial_failed_until_.end() || it->second <= now) {
    return std::nullopt;
  }
  return std::chrono::duration_cast<std::chrono::milliseconds>(it->second - now);
}

void DialBook::EraseExpiredBackoff(const DialKey& peer_key, std::chrono::steady_clock::time_point now) {
  auto it = dial_failed_until_.find(peer_key);
  if (it != dial_failed_until_.end() && it->second <= now) {
    dial_failed_until_.erase(it);
  }
}

void DialBook::IngestRemoteAddrs(const std::string& peer_id, const std::vector<std::string>& addrs) {
  if (peer_id.empty()) {
    return;
  }
  for (const auto& ma : addrs) {
    if (auto parsed = ParseAdpMultiaddr(ma)) {
      EndpointRecord rec;
      rec.multiaddr = ma;
      rec.endpoint = parsed->endpoint;
      rec.peer_id = peer_id;
      // Prefer existing dial key for this peer_id; else register under peer_id.
      bool updated = false;
      for (auto& [key, existing] : endpoints_) {
        if (existing.peer_id == peer_id) {
          existing = rec;
          updated = true;
          break;
        }
      }
      if (!updated) {
        endpoints_[peer_id] = std::move(rec);
      }
      break;
    }
  }
}

} // namespace pp::amp
