#pragma once

#include <mutex>

namespace pp::amp {

/**
 * The io lane's lock (MeshRuntime's io lock, PeerLinkManager's strand): a recursive mutex that tells its owner each
 * time a thread lets go of it for the last time (its outermost unlock), still holding it. MeshRuntime learns so that
 * a thread other than the driver changed the lane — posted work, armed a timer, sent a reliable packet, dialed — and
 * wakes an idle driver when that made something due before its planned wake (MeshRuntime::Drive's next_due).
 */
class IoLaneMutex {
public:
  /** Called with the lock held, at a thread's outermost unlock. Must not throw; may not unlock. */
  using ReleaseHook = void (*)(void* context);

  void lock() {
    mu_.lock();
    ++depth_;
  }
  bool try_lock() {
    if (!mu_.try_lock()) {
      return false;
    }
    ++depth_;
    return true;
  }
  void unlock() {
    if (depth_ == 1 && hook_ != nullptr) {
      hook_(context_);
    }
    --depth_;
    mu_.unlock();
  }

  /** Under the lock (or before any other thread can take it). `hook` null: none. */
  void SetReleaseHook(void* context, ReleaseHook hook) {
    context_ = context;
    hook_ = hook;
  }

private:
  std::recursive_mutex mu_;
  /** Recursion depth of the holder (written only by it). */
  int depth_ = 0;
  void* context_ = nullptr;
  ReleaseHook hook_ = nullptr;
};

} // namespace pp::amp
