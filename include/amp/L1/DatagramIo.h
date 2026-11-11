#pragma once

#include "amp/L1/Types.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace pp::adp {

/**
 * `Error::code` of a SendTo failure that means "no route to this peer right now" (host down / host
 * unreachable / network unreachable / network down) — the association to it cannot work until the
 * path changes, so the link above drops it at once instead of waiting out its liveness window.
 */
inline constexpr int32_t kDatagramSendUnreachable = 1;

inline bool IsUnreachableSendError(const Error& error) {
  return error.code == kDatagramSendUnreachable;
}

/** What an OS socket tells about itself (OsUdpDatagramIo::Stats). */
struct DatagramIoStats {
  /** Buffers the kernel granted, in bytes (what is usable: Linux's doubled figure halved). */
  size_t receive_buffer_bytes = 0;
  size_t send_buffer_bytes = 0;
  /** Datagrams the kernel dropped for want of receive buffer, since the socket opened (Linux). */
  std::optional<uint64_t> receive_drops;
};

class DatagramIo {
public:
  virtual ~DatagramIo() = default;
  /** The socket's buffers and drops, where the platform tells; none for in-memory IO. */
  virtual std::optional<DatagramIoStats> Stats() const { return std::nullopt; }

  virtual Roe<void> SendTo(const IpEndpoint& peer, std::span<const uint8_t> datagram) = 0;

  /** Most datagrams in one SendSegments call (the kernel's UDP GSO limit). */
  static constexpr size_t kMaxSegments = 64;
  /**
   * Several datagrams to one peer: `parts[i]` is datagram i; every one is `segment` bytes but the last, which may
   * be shorter (Endpoint::FlushSends groups them so). OsUdpDatagramIo sends them in one system call on Linux and
   * Android (UDP GSO, UDP_SEGMENT): the stack — routing, the firewall, the checks — handles them once, and the
   * kernel splits them at the end. Elsewhere, or where the kernel refuses it: SendTo each. An error says so for
   * the whole run (the unreachable code as SendTo's).
   */
  virtual Roe<void> SendSegments(const IpEndpoint& peer, std::span<const std::span<const uint8_t>> parts,
                                 const size_t segment) {
    (void)segment;
    for (const auto& part : parts) {
      if (auto sent = SendTo(peer, part); !sent) {
        return sent;
      }
    }
    return {};
  }

  /** Non-blocking: returns nullopt if no datagram ready. */
  virtual Roe<std::optional<std::pair<IpEndpoint, std::vector<uint8_t>>>> RecvFrom() = 0;

  /** A received datagram, in the IO's own buffer: valid only during the call. */
  using DatagramVisitor = std::function<void(const IpEndpoint& from, std::span<const uint8_t> datagram)>;
  /**
   * Non-blocking: hands up to `max` ready datagrams to `visit`, in arrival order, and returns how many. An IO that
   * can read several per system call (OsUdpDatagramIo: recvmmsg on Linux) does, into buffers it reuses — no copy
   * and no allocation per datagram. `visit` may send on this IO. An error after some datagrams were handed over
   * returns the count; the next call reports it. Default: RecvFrom, one by one.
   */
  virtual Roe<size_t> RecvBatch(const size_t max, const DatagramVisitor& visit) {
    size_t n = 0;
    while (n < max) {
      auto got = RecvFrom();
      if (!got) {
        return n > 0 ? Roe<size_t>(n) : Roe<size_t>(got.error());
      }
      if (!*got) {
        break;
      }
      visit((*got)->first, (*got)->second);
      ++n;
    }
    return n;
  }

  virtual IpEndpoint LocalEndpoint() const = 0;

  /**
   * Block until a datagram may be ready to read, `timeout` passes, or WakeWaiter() is called; true when the IO
   * says something is readable (a socket poll), false otherwise. Called only by the thread that reads.
   * OsUdpDatagramIo polls its socket: an idle reader wakes as a datagram arrives, not at the next fixed tick — a
   * blind wait let a busy socket's small buffer overflow before it (pp-browser stress lab, 2026-11-11). Default
   * (no socket): wait out `timeout` or the wake.
   */
  virtual bool WaitReadable(const std::chrono::milliseconds timeout) {
    std::unique_lock lock(wait_mu_);
    wait_cv_.wait_for(lock, timeout, [this] { return woken_; });
    woken_ = false;
    return false;
  }
  /** Any thread: end a WaitReadable now (or the next one, if none is waiting). */
  virtual void WakeWaiter() {
    {
      std::lock_guard lock(wait_mu_);
      woken_ = true;
    }
    wait_cv_.notify_all();
  }

  /**
   * Close the socket and open a new one on the same address and port — for a socket the OS left
   * dead (iOS after a suspend or a path change: sends fail or nothing is received). Same thread as
   * SendTo / RecvFrom. After a failure the IO has no socket until a later Reopen succeeds.
   * Not every IO can (default: refused).
   */
  virtual Roe<void> Reopen() { return Error("datagram io: reopen not supported"); }

private:
  std::mutex wait_mu_;
  std::condition_variable wait_cv_;
  bool woken_ = false;
};

} // namespace pp::adp
