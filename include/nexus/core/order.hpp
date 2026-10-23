#pragma once
#include "nexus/common/types.hpp"

namespace nexus {

struct alignas(64) Order {
  OrderId id;            // 8
  Price price;           // 8
  Quantity qty;          // 4
  Quantity filled;       // 4
  Timestamp ts;          // 8
  Side side;             // 1
  OrderType type;        // 1
  TimeInForce tif;       // 1
  uint8_t pad[13]{};     // 13
  Order* next{nullptr};  // 8
  Order* prev{nullptr};  // 8  => 64
  constexpr Quantity remaining() const noexcept { return qty - filled; }
};
static_assert(sizeof(Order) == 64 && alignof(Order) == 64);

}  // namespace nexus
