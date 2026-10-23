#pragma once
#include <cstddef>

#include "nexus/container/order_map.hpp"
#include "nexus/container/price_ladder.hpp"
#include "nexus/core/events.hpp"
#include "nexus/core/order.hpp"
#include "nexus/memory/object_pool.hpp"

namespace nexus {

// Book state: both ladders, the id index and the order pool. It keeps the ladder bitmaps and
// the index consistent with level contents but knows nothing about matching rules.
template <size_t MaxOrders>
class OrderBook {
 public:
  OrderBook(Price min_price, Price max_price)
      : bids_(min_price, max_price), asks_(min_price, max_price), index_(MaxOrders) {}

  bool in_band(Price p) const noexcept { return bids_.in_band(p); }
  bool full() const noexcept { return pool_.available() == 0; }
  size_t live_orders() const noexcept { return index_.size(); }
  Order* find(OrderId id) const noexcept { return index_.find(id); }

  PriceLadder& ladder(Side s) noexcept { return s == Side::Buy ? bids_ : asks_; }
  const PriceLadder& ladder(Side s) const noexcept { return s == Side::Buy ? bids_ : asks_; }

  // Best price on side s, or kNoPrice.
  Price best(Side s) const noexcept { return s == Side::Buy ? bids_.highest() : asks_.lowest(); }

  // Creates a resting order at the tail of its level. Caller checked !full() and that the id
  // is not live, so neither the pool nor the index can fail here.
  void add(OrderId id, Side side, Price price, Quantity qty, Quantity filled, Timestamp ts) {
    Order* o = pool_.allocate();
    o->id = id;
    o->price = price;
    o->qty = qty;
    o->filled = filled;
    o->ts = ts;
    o->side = side;
    o->type = OrderType::Limit;
    o->tif = TimeInForce::GTC;
    index_.insert(id, o);
    link(o);
  }

  // Appends an order object (new or re-priced) to the tail of its level.
  void link(Order* o) noexcept {
    PriceLadder& l = ladder(o->side);
    l[o->price].append(o);
    l.mark(o->price);
  }

  // Removes an order from its level but keeps it indexed and allocated.
  void unlink(Order* o) noexcept {
    PriceLadder& l = ladder(o->side);
    PriceLevel& level = l[o->price];
    level.unlink(o);
    if (level.empty()) l.clear(o->price);
  }

  // Drops an unlinked order entirely.
  void release(Order* o) noexcept {
    index_.erase(o->id);
    pool_.deallocate(o);
  }

  void erase(Order* o) noexcept {
    unlink(o);
    release(o);
  }

  TopOfBook top() const noexcept {
    TopOfBook t;
    if ((t.bid_price = best(Side::Buy)) != kNoPrice) t.bid_volume = bids_[t.bid_price].volume();
    if ((t.ask_price = best(Side::Sell)) != kNoPrice) t.ask_volume = asks_[t.ask_price].volume();
    return t;
  }

 private:
  PriceLadder bids_, asks_;
  OrderMap index_;
  ObjectPool<Order, MaxOrders> pool_;
};

}  // namespace nexus
