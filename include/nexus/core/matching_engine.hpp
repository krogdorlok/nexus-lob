#pragma once
#include <algorithm>
#include <cstddef>
#include <limits>

#include "nexus/core/events.hpp"
#include "nexus/core/order_book.hpp"

namespace nexus {

// Price-time priority matching engine. Semantics are specified in events.hpp. Events go to a
// caller-supplied sink, called as emit(const Event&), so the engine never buffers or
// allocates. After construction, process() performs no heap allocation. The sink must not
// throw: a command interrupted halfway leaves its earlier fills applied.
template <size_t MaxOrders = size_t{1} << 20>
class MatchingEngine {
 public:
  MatchingEngine(Price min_price, Price max_price) : book_(min_price, max_price) {}

  template <class Sink>
  void process(const Command& c, Sink&& sink) {
    auto emit = [&sink](const Event& e) { sink(e); };  // sink's return value is ignored
    switch (c.type) {
      case CommandType::New:
        return on_new(c, emit);
      case CommandType::Cancel:
        return on_cancel(c, emit);
      case CommandType::Modify:
        return on_modify(c, emit);
    }
  }

  TopOfBook top() const noexcept { return book_.top(); }
  size_t live_orders() const noexcept { return book_.live_orders(); }

 private:
  // Limit used for a Market aggressor: crosses every resting price.
  static constexpr Price market_limit(Side s) noexcept {
    return s == Side::Buy ? std::numeric_limits<Price>::max() - 1 : 0;
  }
  static constexpr bool crosses(Side aggressor, Price limit, Price resting) noexcept {
    return aggressor == Side::Buy ? resting <= limit : resting >= limit;
  }
  // Next resting level the aggressor would meet strictly beyond p, or kNoPrice.
  Price next_level(Side aggressor, Price p) const noexcept {
    const PriceLadder& l = book_.ladder(opposite(aggressor));
    if (aggressor == Side::Buy) return p == l.max_price() ? kNoPrice : l.at_or_above(p + 1);
    return p == l.min_price() ? kNoPrice : l.at_or_below(p - 1);
  }

  template <class Sink>
  void on_new(const Command& c, Sink& emit) {
    const bool market = c.order_type == OrderType::Market;
    const bool rests = !market && c.tif == TimeInForce::GTC;
    if (c.qty == 0) return emit(Event::rejected(c.id, RejectReason::ZeroQty));
    if (!market && !book_.in_band(c.price)) {
      return emit(Event::rejected(c.id, RejectReason::PriceOutOfBand));
    }
    if (book_.find(c.id)) return emit(Event::rejected(c.id, RejectReason::DuplicateId));
    if (rests && book_.full()) return emit(Event::rejected(c.id, RejectReason::PoolExhausted));
    const Price limit = market ? market_limit(c.side) : c.price;
    if (c.tif == TimeInForce::FOK && !fillable(c.side, limit, c.qty)) {
      return emit(Event::rejected(c.id, RejectReason::FokUnfillable));
    }

    const Price shown = market ? 0 : c.price;
    emit(Event::accepted(c.id, c.side, shown, c.qty));
    const Quantity left = match(c.id, c.side, limit, c.qty, emit);
    if (left == 0) return;
    if (rests) {
      book_.add(c.id, c.side, c.price, c.qty, c.qty - left, c.ts);
    } else {
      emit(Event::cancelled(c.id, c.side, shown, left));
    }
  }

  template <class Sink>
  void on_cancel(const Command& c, Sink& emit) {
    Order* o = book_.find(c.id);
    if (!o) return emit(Event::rejected(c.id, RejectReason::UnknownId));
    emit(Event::cancelled(o->id, o->side, o->price, o->remaining()));
    book_.erase(o);
  }

  template <class Sink>
  void on_modify(const Command& c, Sink& emit) {
    if (c.qty == 0) return emit(Event::rejected(c.id, RejectReason::ZeroQty));
    if (!book_.in_band(c.price)) return emit(Event::rejected(c.id, RejectReason::PriceOutOfBand));
    Order* o = book_.find(c.id);
    if (!o) return emit(Event::rejected(c.id, RejectReason::UnknownId));
    emit(Event::modified(o->id, o->side, c.price, c.qty));

    if (c.price == o->price && c.qty <= o->remaining()) {  // shrink in place, keep priority
      const Quantity cut = o->remaining() - c.qty;
      o->qty -= cut;
      book_.ladder(o->side)[o->price].reduce(cut);
      return;
    }
    // Lose priority: leave the book (so it cannot match itself), then re-enter as aggressor.
    book_.unlink(o);
    const Quantity left = match(o->id, o->side, c.price, c.qty, emit);
    if (left == 0) return book_.release(o);
    o->price = c.price;
    o->qty = c.qty;
    o->filled = c.qty - left;
    o->ts = c.ts;
    book_.link(o);
  }

  // Sweeps the opposite side while it crosses `limit`. Returns the unfilled quantity.
  template <class Sink>
  Quantity match(OrderId aggressor, Side side, Price limit, Quantity qty, Sink& emit) {
    PriceLadder& opp = book_.ladder(opposite(side));
    for (Price p = book_.best(opposite(side)); qty && p != kNoPrice && crosses(side, limit, p);
         p = next_level(side, p)) {
      PriceLevel& level = opp[p];
      while (qty && !level.empty()) {
        Order* r = level.head();
        const OrderId resting = r->id;
        const Quantity q = std::min(qty, r->remaining());
        r->filled += q;
        level.reduce(q);
        qty -= q;
        if (r->remaining() == 0) book_.erase(r);
        emit(Event::fill(aggressor, resting, side, p, q));  // book is consistent before emit
      }
      if (qty == 0) break;
    }
    return qty;
  }

  // True if crossing volume on the opposite side covers qty. Touches nothing.
  bool fillable(Side side, Price limit, Quantity qty) const noexcept {
    const PriceLadder& opp = book_.ladder(opposite(side));
    uint64_t need = qty;
    for (Price p = book_.best(opposite(side)); p != kNoPrice && crosses(side, limit, p);
         p = next_level(side, p)) {
      const uint64_t v = opp[p].volume();
      if (v >= need) return true;
      need -= v;
    }
    return false;
  }

  OrderBook<MaxOrders> book_;
};

}  // namespace nexus
