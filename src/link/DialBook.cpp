#include "amp/link/DialBook.h"

#include <algorithm>

namespace pp::amp {
namespace {

void InsertCandidateFront(std::vector<std::string>& candidates, const std::string& ma) {
  candidates.erase(std::remove(candidates.begin(), candidates.end(), ma), candidates.end());
  candidates.insert(candidates.begin(), ma);
  if (candidates.size() > DialBook::kMaxCandidatesPerKey) {
    candidates.resize(DialBook::kMaxCandidatesPerKey);
  }
}

} // namespace

bool DialBook::ApplyParsed(EndpointRecord& rec, const std::string& multiaddr, const DialKey& peer_key) {
  auto parsed = ParseAdpMultiaddr(multiaddr);
  if (!parsed) {
    return false;
  }
  rec.multiaddr = multiaddr;
  rec.endpoint = parsed->endpoint;
  rec.peer_id = parsed->peer_id.empty() ? peer_key : parsed->peer_id;
  return true;
}

void DialBook::SyncActiveFromIndex(EndpointRecord& rec) {
  if (rec.candidates.empty()) {
    return;
  }
  if (rec.dial_index >= rec.candidates.size()) {
    rec.dial_index = 0;
  }
  auto parsed = ParseAdpMultiaddr(rec.candidates[rec.dial_index]);
  if (!parsed) {
    return;
  }
  rec.multiaddr = rec.candidates[rec.dial_index];
  rec.endpoint = parsed->endpoint;
  if (!parsed->peer_id.empty()) {
    rec.peer_id = parsed->peer_id;
  }
}

Roe<void> DialBook::RegisterEndpoint(const DialKey& peer_key, const std::string& multiaddr) {
  auto parsed = ParseAdpMultiaddr(multiaddr);
  if (!parsed) {
    return parsed.error();
  }
  EndpointRecord* existing = Find(peer_key);
  const std::string prev_preferred = existing ? existing->multiaddr : std::string{};
  if (!existing) {
    EndpointRecord rec;
    rec.peer_id = parsed->peer_id.empty() ? peer_key : parsed->peer_id;
    InsertCandidateFront(rec.candidates, multiaddr);
    rec.dial_index = 0;
    SyncActiveFromIndex(rec);
    endpoints_[peer_key] = std::move(rec);
  } else {
    if (existing->peer_id.empty() && !parsed->peer_id.empty()) {
      existing->peer_id = parsed->peer_id;
    }
    InsertCandidateFront(existing->candidates, multiaddr);
    existing->dial_index = 0;
    SyncActiveFromIndex(*existing);
  }
  // Fresh dial target must not stay blocked by a prior miss on a stale address (B14).
  if (prev_preferred != multiaddr) {
    ClearBackoff(peer_key);
  }
  return {};
}

Roe<void> DialBook::RegisterEndpoints(const DialKey& peer_key,
                                      const std::vector<std::string>& multiaddrs) {
  EndpointRecord rec;
  rec.peer_id = peer_key;
  std::string prev_preferred;
  if (const auto* existing = Find(peer_key)) {
    prev_preferred = existing->multiaddr;
    if (!existing->peer_id.empty()) {
      rec.peer_id = existing->peer_id;
    }
  }
  for (const std::string& ma : multiaddrs) {
    auto parsed = ParseAdpMultiaddr(ma);
    if (!parsed) {
      continue;
    }
    if (std::find(rec.candidates.begin(), rec.candidates.end(), ma) != rec.candidates.end()) {
      continue;
    }
    if (rec.peer_id == peer_key && !parsed->peer_id.empty()) {
      rec.peer_id = parsed->peer_id;
    }
    rec.candidates.push_back(ma);
    if (rec.candidates.size() >= kMaxCandidatesPerKey) {
      break;
    }
  }
  if (rec.candidates.empty()) {
    return Error("amp dial book: no parseable multiaddrs");
  }
  rec.dial_index = 0;
  SyncActiveFromIndex(rec);
  endpoints_[peer_key] = std::move(rec);
  if (prev_preferred != endpoints_[peer_key].multiaddr) {
    ClearBackoff(peer_key);
  }
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

bool DialBook::AdvanceDialCandidate(const DialKey& peer_key) {
  auto* rec = Find(peer_key);
  if (!rec || rec->candidates.size() < 2) {
    return false;
  }
  if (rec->dial_index + 1 >= rec->candidates.size()) {
    return false;
  }
  ++rec->dial_index;
  SyncActiveFromIndex(*rec);
  return true;
}

void DialBook::ResetDialIndex(const DialKey& peer_key) {
  auto* rec = Find(peer_key);
  if (!rec) {
    return;
  }
  rec->dial_index = 0;
  SyncActiveFromIndex(*rec);
}

void DialBook::PromoteDialWinner(const DialKey& peer_key, const std::string& multiaddr) {
  auto* rec = Find(peer_key);
  if (!rec || multiaddr.empty()) {
    return;
  }
  InsertCandidateFront(rec->candidates, multiaddr);
  rec->dial_index = 0;
  SyncActiveFromIndex(*rec);
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
  // Merge into an existing dial key for this peer_id when present; else under peer_id.
  DialKey target_key = peer_id;
  for (const auto& [key, existing] : endpoints_) {
    if (existing.peer_id == peer_id) {
      target_key = key;
      break;
    }
  }
  std::vector<std::string> merged;
  if (const auto* existing = Find(target_key)) {
    merged = existing->candidates;
  }
  for (const auto& ma : addrs) {
    if (!ParseAdpMultiaddr(ma)) {
      continue;
    }
    if (std::find(merged.begin(), merged.end(), ma) != merged.end()) {
      continue;
    }
    merged.push_back(ma);
    if (merged.size() >= kMaxCandidatesPerKey) {
      break;
    }
  }
  if (merged.empty()) {
    return;
  }
  (void)RegisterEndpoints(target_key, merged);
}

} // namespace pp::amp
