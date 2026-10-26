#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>

namespace nexus {

// Apple M-series cores use 128-byte cache lines; everything else we target uses 64.
#if defined(__APPLE__) && defined(__aarch64__)
inline constexpr size_t kCacheLine = 128;
#else
inline constexpr size_t kCacheLine = 64;
#endif

// Lock-free single-producer single-consumer ring. Indices grow monotonically and are masked on
// access, so full/empty are unambiguous and unsigned wraparound is harmless. Each side caches
// the other's index and re-reads it (acquire) only when the cache says full/empty.
// Heap-allocate large instances (std::make_unique); never place on the stack.
template <typename T, size_t Cap>
class SPSCRingBuffer {
  static_assert(Cap && (Cap & (Cap - 1)) == 0, "Cap must be a power of 2");
  static_assert(std::is_trivially_copyable_v<T>);
  // "Lock-free" is a property of the target, not of the C++ standard: refuse to build where
  // the index atomics would fall back to a lock.
  static_assert(std::atomic<size_t>::is_always_lock_free);

 public:
  SPSCRingBuffer() = default;
  // Test hook: start both indices at `start` to exercise index wraparound.
  explicit SPSCRingBuffer(size_t start) noexcept {
    prod_.head.store(start, std::memory_order_relaxed);
    prod_.tail_cache = start;
    cons_.tail.store(start, std::memory_order_relaxed);
    cons_.head_cache = start;
  }

  bool push(const T& v) noexcept {  // producer only
    const size_t h = prod_.head.load(std::memory_order_relaxed);
    if (h - prod_.tail_cache == Cap) {
      prod_.tail_cache = cons_.tail.load(std::memory_order_acquire);
      if (h - prod_.tail_cache == Cap) return false;
    }
    buf_[h & kMask] = v;
    prod_.head.store(h + 1, std::memory_order_release);
    return true;
  }

  bool pop(T& out) noexcept {  // consumer only
    const size_t t = cons_.tail.load(std::memory_order_relaxed);
    if (t == cons_.head_cache) {
      cons_.head_cache = prod_.head.load(std::memory_order_acquire);
      if (t == cons_.head_cache) return false;
    }
    out = buf_[t & kMask];
    cons_.tail.store(t + 1, std::memory_order_release);
    return true;
  }

 private:
  static constexpr size_t kMask = Cap - 1;
  struct alignas(kCacheLine) Prod {
    std::atomic<size_t> head{0};
    size_t tail_cache{0};
  } prod_;
  struct alignas(kCacheLine) Cons {
    std::atomic<size_t> tail{0};
    size_t head_cache{0};
  } cons_;
  alignas(kCacheLine) std::array<T, Cap> buf_{};
};

}  // namespace nexus
