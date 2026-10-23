#pragma once
#include <bit>
#include <cstddef>
#include <stdexcept>
#include <vector>

#include "nexus/core/order.hpp"

namespace nexus {

// OrderId -> Order* open-addressing table. Preallocated, linear probing, backward-shift
// deletion (no tombstones). Capacity is a power of two at least 2 * max_entries, so the load
// factor never exceeds 0.5 and every probe sequence ends at an empty slot.
class OrderMap {
 public:
  static constexpr size_t kMaxEntries = size_t{1} << 40;

  explicit OrderMap(size_t max_entries)
      : capacity_(std::bit_ceil(checked(max_entries) < 1 ? size_t{2} : 2 * max_entries)),
        mask_(capacity_ - 1),
        shift_(64 - std::countr_zero(capacity_)),
        max_entries_(capacity_ / 2),
        slots_(capacity_) {}

  // False if the id is already present or the table is at max load. `v` must be non-null.
  bool insert(OrderId k, Order* v) noexcept {
    if (size_ == max_entries_) return false;
    size_t i = home(k);
    for (; slots_[i].val; i = (i + 1) & mask_) {
      if (slots_[i].key == k) return false;
    }
    slots_[i] = {k, v};
    ++size_;
    return true;
  }

  Order* find(OrderId k) const noexcept {
    for (size_t i = home(k);; i = (i + 1) & mask_) {
      const Slot& s = slots_[i];
      if (!s.val || s.key == k) return s.val;
    }
  }

  bool erase(OrderId k) noexcept {
    size_t i = home(k);
    for (; slots_[i].key != k || !slots_[i].val; i = (i + 1) & mask_) {
      if (!slots_[i].val) return false;
    }
    // Backward shift: pull later cluster members into the hole unless that would move them
    // before their home slot.
    for (size_t j = (i + 1) & mask_; slots_[j].val; j = (j + 1) & mask_) {
      if (((j - home(slots_[j].key)) & mask_) >= ((j - i) & mask_)) {
        slots_[i] = slots_[j];
        i = j;
      }
    }
    slots_[i] = {};
    --size_;
    return true;
  }

  // Fibonacci hashing: the top bits of id * 2^64/phi spread sequential ids evenly.
  size_t home(OrderId k) const noexcept {
    return static_cast<size_t>((k * 0x9E3779B97F4A7C15ull) >> shift_);
  }
  size_t size() const noexcept { return size_; }
  size_t capacity() const noexcept { return capacity_; }
  size_t max_entries() const noexcept { return max_entries_; }

 private:
  static size_t checked(size_t n) {
    if (n > kMaxEntries) throw std::length_error("OrderMap: too many entries");
    return n;
  }

  struct Slot {
    OrderId key{0};
    Order* val{nullptr};  // nullptr marks an empty slot
  };
  size_t capacity_, mask_;
  int shift_;
  size_t max_entries_;
  size_t size_{0};
  std::vector<Slot> slots_;
};

}  // namespace nexus
