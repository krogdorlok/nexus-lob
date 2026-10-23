#pragma once
#include <cstddef>
#include <cstring>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace nexus {

// Fixed-capacity pool with a LIFO free list. All memory is acquired and pre-faulted in the
// constructor, so allocate/deallocate never touch the heap or take a page fault.
template <typename T, size_t Cap>
class ObjectPool {
  static_assert(std::is_trivially_destructible_v<T>);
  static_assert(Cap > 0);

 public:
  ObjectPool()  // free_ is built first so a failed storage allocation leaks nothing
      : free_(std::make_unique<T*[]>(Cap)),
        storage_(static_cast<T*>(::operator new(Cap * sizeof(T), std::align_val_t{alignof(T)}))),
        top_(Cap) {
    for (size_t i = 0; i < Cap; ++i) free_[i] = storage_ + (Cap - 1 - i);
    std::memset(static_cast<void*>(storage_), 0, Cap * sizeof(T));  // pre-fault pages
  }
  ~ObjectPool() { ::operator delete(storage_, std::align_val_t{alignof(T)}); }
  ObjectPool(const ObjectPool&) = delete;
  ObjectPool& operator=(const ObjectPool&) = delete;

  template <typename... A>
  T* allocate(A&&... a) noexcept(noexcept(T{std::forward<A>(a)...})) {
    if (top_ == 0) [[unlikely]]
      return nullptr;
    return ::new (free_[--top_]) T{std::forward<A>(a)...};
  }
  void deallocate(T* p) noexcept { free_[top_++] = p; }
  size_t available() const noexcept { return top_; }
  static constexpr size_t capacity() noexcept { return Cap; }

 private:
  std::unique_ptr<T*[]> free_;
  T* storage_;
  size_t top_;
};

}  // namespace nexus
