#pragma once

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <list>
#include <map>
#include <unordered_map>

#include "nexus/core/events.hpp"

namespace nexus::reference {

class NaiveEngine {
 public:
  NaiveEngine(Price min_price, Price max_price, std::size_t max_orders)
      : min_price_(min_price), max_price_(max_price), max_orders_(max_orders) {}

  template <class Sink>
  void process(const Command& c, Sink&& emit) {
    switch (c.type) {
      case CommandType::New:
        ProcessNew(c, emit);
        break;
      case CommandType::Cancel:
        ProcessCancel(c, emit);
        break;
      case CommandType::Modify:
        ProcessModify(c, emit);
        break;
    }
  }

  TopOfBook top() const {
    TopOfBook result;
    if (!bids_.empty()) {
      const auto& [price, orders] = *std::prev(bids_.end());
      result.bid_price = price;
      for (const Order& order : orders) result.bid_volume += order.remaining;
    }
    if (!asks_.empty()) {
      const auto& [price, orders] = *asks_.begin();
      result.ask_price = price;
      for (const Order& order : orders) result.ask_volume += order.remaining;
    }
    return result;
  }

  std::size_t live_orders() const { return index_.size(); }

 private:
  struct Order {
    OrderId id;
    Quantity remaining;
  };

  using Levels = std::map<Price, std::list<Order>>;

  struct Location {
    Side side;
    Price price;
    std::list<Order>::iterator order;
  };

  using Index = std::unordered_map<OrderId, Location>;

  static bool Crosses(Side side, OrderType type, Price limit, Price resting) {
    if (type == OrderType::Market) return true;
    return side == Side::Buy ? resting <= limit : resting >= limit;
  }

  Levels& LevelsFor(Side side) { return side == Side::Buy ? bids_ : asks_; }
  const Levels& LevelsFor(Side side) const { return side == Side::Buy ? bids_ : asks_; }

  bool CanFill(Side side, OrderType type, Price price, Quantity needed) const {
    for (const auto& [resting_price, orders] : LevelsFor(opposite(side))) {
      if (!Crosses(side, type, price, resting_price)) continue;
      for (const Order& order : orders) {
        if (order.remaining >= needed) return true;
        needed -= order.remaining;
      }
    }
    return false;
  }

  void Rest(OrderId id, Side side, Price price, Quantity remaining) {
    auto& orders = LevelsFor(side)[price];
    orders.push_back({id, remaining});
    index_.emplace(id, Location{side, price, std::prev(orders.end())});
  }

  void Remove(Index::iterator found) {
    const Location location = found->second;
    Levels& levels = LevelsFor(location.side);
    auto level = levels.find(location.price);
    level->second.erase(location.order);
    if (level->second.empty()) levels.erase(level);
    index_.erase(found);
  }

  template <class Sink>
  void Match(OrderId id, Side side, OrderType type, Price price, Quantity& remaining, Sink& emit) {
    Levels& other = LevelsFor(opposite(side));
    while (remaining > 0 && !other.empty()) {
      auto level = side == Side::Buy ? other.begin() : std::prev(other.end());
      const Price resting_price = level->first;
      if (!Crosses(side, type, price, resting_price)) break;

      Order& resting = level->second.front();
      const OrderId resting_id = resting.id;
      const Quantity filled = std::min(remaining, resting.remaining);
      remaining -= filled;
      resting.remaining -= filled;
      if (resting.remaining == 0) {
        index_.erase(resting_id);
        level->second.pop_front();
        if (level->second.empty()) other.erase(level);
      }
      emit(Event::fill(id, resting_id, side, resting_price, filled));
    }
  }

  template <class Sink>
  void ProcessNew(const Command& c, Sink& emit) {
    if (c.qty == 0) {
      emit(Event::rejected(c.id, RejectReason::ZeroQty));
      return;
    }
    if (c.order_type == OrderType::Limit && (c.price < min_price_ || c.price > max_price_)) {
      emit(Event::rejected(c.id, RejectReason::PriceOutOfBand));
      return;
    }
    if (index_.contains(c.id)) {
      emit(Event::rejected(c.id, RejectReason::DuplicateId));
      return;
    }
    if (c.order_type == OrderType::Limit && c.tif == TimeInForce::GTC &&
        live_orders() == max_orders_) {
      emit(Event::rejected(c.id, RejectReason::PoolExhausted));
      return;
    }
    if (c.tif == TimeInForce::FOK && !CanFill(c.side, c.order_type, c.price, c.qty)) {
      emit(Event::rejected(c.id, RejectReason::FokUnfillable));
      return;
    }

    // A Market order has no order price, including when its remainder is cancelled.
    const Price event_price = c.order_type == OrderType::Market ? 0 : c.price;
    emit(Event::accepted(c.id, c.side, event_price, c.qty));
    Quantity remaining = c.qty;
    Match(c.id, c.side, c.order_type, c.price, remaining, emit);
    if (remaining == 0) return;
    if (c.order_type == OrderType::Limit && c.tif == TimeInForce::GTC) {
      Rest(c.id, c.side, c.price, remaining);
    } else {
      emit(Event::cancelled(c.id, c.side, event_price, remaining));
    }
  }

  template <class Sink>
  void ProcessCancel(const Command& c, Sink& emit) {
    auto found = index_.find(c.id);
    if (found == index_.end()) {
      emit(Event::rejected(c.id, RejectReason::UnknownId));
      return;
    }
    const Location location = found->second;
    const Quantity remaining = location.order->remaining;
    Remove(found);
    emit(Event::cancelled(c.id, location.side, location.price, remaining));
  }

  template <class Sink>
  void ProcessModify(const Command& c, Sink& emit) {
    if (c.qty == 0) {
      emit(Event::rejected(c.id, RejectReason::ZeroQty));
      return;
    }
    if (c.price < min_price_ || c.price > max_price_) {
      emit(Event::rejected(c.id, RejectReason::PriceOutOfBand));
      return;
    }
    auto found = index_.find(c.id);
    if (found == index_.end()) {
      emit(Event::rejected(c.id, RejectReason::UnknownId));
      return;
    }

    const Location location = found->second;
    if (c.price == location.price && c.qty <= location.order->remaining) {
      location.order->remaining = c.qty;
      emit(Event::modified(c.id, location.side, c.price, c.qty));
      return;
    }

    Remove(found);
    emit(Event::modified(c.id, location.side, c.price, c.qty));
    Quantity remaining = c.qty;
    Match(c.id, location.side, OrderType::Limit, c.price, remaining, emit);
    if (remaining > 0) Rest(c.id, location.side, c.price, remaining);
  }

  Price min_price_;
  Price max_price_;
  std::size_t max_orders_;
  Levels bids_;
  Levels asks_;
  Index index_;
};

}  // namespace nexus::reference
