#include "amp/L3/MessageReassembly.h"

namespace pp::amp {

MessageReassembly::MessageReassembly(const size_t max_message_bytes) : max_message_bytes_(max_message_bytes) {}

Roe<std::optional<std::vector<uint8_t>>> MessageReassembly::Push(const ChannelFragBody& frag, const int64_t now_ms) {
  if (frag.frag_count == 0 || frag.frag_index >= frag.frag_count) {
    return Error("amp ch: bad frag indices");
  }
  if (frag.total_len > max_message_bytes_) {
    return Error("amp ch: message too large");
  }

  auto& partial = partial_[frag.msg_id];
  if (partial.frag_count == 0) {
    partial.frag_count = frag.frag_count;
    partial.total_len = frag.total_len;
    partial.started_ms = now_ms;
    partial.chunks.resize(frag.frag_count);
  }
  if (partial.frag_count != frag.frag_count || partial.total_len != frag.total_len) {
    return Error("amp ch: frag metadata mismatch");
  }
  if (!partial.chunks[frag.frag_index].empty()) {
    return std::optional<std::vector<uint8_t>>{}; // dup
  }
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
