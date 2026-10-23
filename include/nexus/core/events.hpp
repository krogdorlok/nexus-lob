#pragma once
#include <type_traits>

#include "nexus/common/types.hpp"

// Semantics contract shared by MatchingEngine and reference::NaiveEngine. The differential
// fuzzer checks that both emit byte-identical event sequences for the same commands.
//
// New(id, side, order_type, tif, price, qty), checks in this order:
//   1. qty == 0                                  -> Rejected(ZeroQty)
//   2. Limit and price outside [min, max]        -> Rejected(PriceOutOfBand)  (Market skips this)
//   3. id is a live resting order                -> Rejected(DuplicateId)
//   4. Limit GTC and the book holds max_orders   -> Rejected(PoolExhausted)
//   5. FOK and opposite crossing volume < qty    -> Rejected(FokUnfillable)  (no side effects)
//   Then Accepted(id, side, price, qty), with price = 0 for Market orders, followed by Fills
//   against the opposite side in price-time priority. Buy crosses an ask at p when p <= price;
//   Sell crosses a bid at p when p >= price; Market crosses everything. Each Fill is
//   (aggressor id, resting id, aggressor side, resting price, qty). Afterwards:
//   Limit GTC rests its remainder at the tail of its level; Limit IOC, Limit FOK and every
//   Market order (any tif) emits Cancelled(id, side, price, remainder) if remainder > 0, where
//   price is the limit price, or 0 for Market.
//
// Cancel(id): unknown id -> Rejected(UnknownId); else Cancelled(id, side, price, remaining).
//
// Modify(id, price, qty), qty is the new open quantity. Checks in order: ZeroQty,
// PriceOutOfBand, UnknownId. If price is unchanged and qty <= remaining, the order keeps its
// queue position and its remaining shrinks to qty. Otherwise it is removed from the book and
// re-entered as a Limit GTC aggressor with the same id: it may match (Fills follow) and any
// remainder rests at the tail of the new level. Modify never needs a new slot, so it cannot
// fail with PoolExhausted. Both paths emit Modified(id, side, new price, new qty) first.
//
// Rejected events carry only id and reason. Self-trades are allowed (no STP).
namespace nexus {

enum class EventType : uint8_t { Accepted, Fill, Cancelled, Modified, Rejected };

struct Event {
  OrderId id;          // order id; the aggressor for Fill
  OrderId resting_id;  // Fill only
  Price price;
  Quantity qty;
  EventType type;
  RejectReason reason;
  Side side;
  uint8_t pad;

  static constexpr Event accepted(OrderId id, Side s, Price p, Quantity q) noexcept {
    return {id, 0, p, q, EventType::Accepted, RejectReason::None, s, 0};
  }
  static constexpr Event fill(OrderId aggressor, OrderId resting, Side s, Price p,
                              Quantity q) noexcept {
    return {aggressor, resting, p, q, EventType::Fill, RejectReason::None, s, 0};
  }
  static constexpr Event cancelled(OrderId id, Side s, Price p, Quantity q) noexcept {
    return {id, 0, p, q, EventType::Cancelled, RejectReason::None, s, 0};
  }
  static constexpr Event modified(OrderId id, Side s, Price p, Quantity q) noexcept {
    return {id, 0, p, q, EventType::Modified, RejectReason::None, s, 0};
  }
  static constexpr Event rejected(OrderId id, RejectReason r) noexcept {
    return {id, 0, 0, 0, EventType::Rejected, r, Side::Buy, 0};
  }
  bool operator==(const Event&) const = default;
};
// No padding bytes, so memcmp equality is well defined.
static_assert(sizeof(Event) == 32 && std::has_unique_object_representations_v<Event>);
static_assert(std::is_trivially_copyable_v<Event>);

struct TopOfBook {
  Price bid_price{kNoPrice};
  uint64_t bid_volume{0};
  Price ask_price{kNoPrice};
  uint64_t ask_volume{0};
  bool operator==(const TopOfBook&) const = default;
};

}  // namespace nexus
