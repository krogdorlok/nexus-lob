#pragma once
#include <cstdint>

#include "nexus/core/order.hpp"

namespace nexus {

// Intrusive FIFO of resting orders at one price. Links live inside Order, so append and
// unlink are O(1) with no allocation. Invariant: volume() == sum of remaining() over linked
// orders, so callers must call reduce(q) whenever they shrink a linked order's remaining.
class PriceLevel {
 public:
  Order* head() const noexcept { return head_; }
  Order* tail() const noexcept { return tail_; }
  bool empty() const noexcept { return head_ == nullptr; }
  uint64_t volume() const noexcept { return volume_; }
  uint32_t count() const noexcept { return count_; }

  void append(Order* o) noexcept {
    o->next = nullptr;
    o->prev = tail_;
    (tail_ ? tail_->next : head_) = o;
    tail_ = o;
    volume_ += o->remaining();
    ++count_;
  }

  void unlink(Order* o) noexcept {
    (o->prev ? o->prev->next : head_) = o->next;
    (o->next ? o->next->prev : tail_) = o->prev;
    o->next = o->prev = nullptr;
    volume_ -= o->remaining();
    --count_;
  }

  void reduce(Quantity q) noexcept { volume_ -= q; }

 private:
  Order* head_{nullptr};
  Order* tail_{nullptr};
  uint64_t volume_{0};
  uint32_t count_{0};
};

}  // namespace nexus
