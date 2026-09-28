#include "amp/L3/MessageReassembly.h"

namespace pp::amp {

MessageReassembly::MessageReassembly(const size_t max_message_bytes, const size_t max_partials)
    : max_message_bytes_(max_message_bytes), max_partials_(max_partials) {}

Roe<std::optional<std::vector<uint8_t>>> MessageReassembly::Push(const ChannelFragBody& frag, const int64_t now_ms) {
  if (frag.frag_count == 0 || frag.frag_index >= frag.frag_count) {
    return Error("amp ch: bad frag indices");
  }
  if (frag.total_len > max_message_bytes_) {
    return Error("amp ch: message too large");
  }
  // Every real fragment carries at least one byte, so a declared frag_count above total_len is
  // never legitimate — without this, a tiny message can still claim up to 65535 fragments and
  // reserve that many (empty) assembly slots.
  if (frag.frag_count > frag.total_len) {
    return Error("amp ch: frag count exceeds message length");
  }

  auto it = partial_.find(frag.msg_id);
  if (it == partial_.end()) {
    // A cap independent of max_message_bytes_: without it, a peer can open one in-flight partial
    // per distinct msg_id without ever completing any of them.
    if (partial_.size() >= max_partials_) {
      return Error("amp ch: too many in-flight partial messages");
    }
    it = partial_.emplace(frag.msg_id, Partial{}).first;
    it->second.frag_count = frag.frag_count;
    it->second.total_len = frag.total_len;
    it->second.started_ms = now_ms;
    it->second.chunks.resize(frag.frag_count);
  }
  auto& partial = it->second;
  if (partial.frag_count != frag.frag_count || partial.total_len != frag.total_len) {
    return Error("amp ch: frag metadata mismatch");
  }
  if (!partial.chunks[frag.frag_index].empty()) {
    return std::optional<std::vector<uint8_t>>{}; // dup
  }
  if (partial.received_bytes + frag.chunk.size() > partial.total_len) {
    return Error("amp ch: frag stream exceeds declared length");
  }
  partial.received_bytes += static_cast<uint32_t>(frag.chunk.size());
  partial.chunks[frag.frag_index] = frag.chunk;

  for (const auto& chunk : partial.chunks) {
    if (chunk.empty()) {
      return std::optional<std::vector<uint8_t>>{};
    }
  }

  std::vector<uint8_t> out;
  out.reserve(partial.total_len);
  for (const auto& chunk : partial.chunks) {
    out.insert(out.end(), chunk.begin(), chunk.end());
  }
  if (out.size() != partial.total_len) {
    return Error("amp ch: assembled length mismatch");
  }
  partial_.erase(frag.msg_id);
  return std::optional<std::vector<uint8_t>>{std::move(out)};
}

void MessageReassembly::SweepExpired(const int64_t now_ms) {
  for (auto it = partial_.begin(); it != partial_.end();) {
    if (now_ms - it->second.started_ms > kDefaultFragAssemblyTimeoutMs) {
      it = partial_.erase(it);
    } else {
      ++it;
    }
  }
}

} // namespace pp::amp
