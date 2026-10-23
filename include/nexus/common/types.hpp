#pragma once
#include <cstdint>
#include <limits>
#include <type_traits>

namespace nexus {

using OrderId = uint64_t;
using Price = uint64_t;  // fixed-point ticks, e.g. 100.25 -> 10025 with tick=0.01
using Quantity = uint32_t;
using Timestamp = uint64_t;  // TSC ticks

inline constexpr Price kNoPrice = std::numeric_limits<Price>::max();  // "no level" sentinel

enum class Side : uint8_t { Buy, Sell };
enum class OrderType : uint8_t { Limit, Market };
enum class TimeInForce : uint8_t { GTC, IOC, FOK };
enum class RejectReason : uint8_t {
  None,
  DuplicateId,
  UnknownId,
  PriceOutOfBand,
  ZeroQty,
  PoolExhausted,
  FokUnfillable
};
enum class CommandType : uint8_t { New, Cancel, Modify };

constexpr Side opposite(Side s) noexcept { return s == Side::Buy ? Side::Sell : Side::Buy; }

// One inbound instruction. Cancel uses only `id`; Modify uses `id`, `price`, `qty`.
struct Command {
  OrderId id;
  Price price;
  Quantity qty;
  CommandType type;
  Side side;
  OrderType order_type;
  TimeInForce tif;
  Timestamp ts;  // stamped by the producer on push
};
static_assert(sizeof(Command) == 32 && std::is_trivially_copyable_v<Command>);

}  // namespace nexus
