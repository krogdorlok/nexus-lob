#include "flow_generator.hpp"

#include <algorithm>
#include <stdexcept>

namespace nexus {

namespace {
constexpr double kScale = 4294967296.0;  // 2^32
}

const FlowConfig& FlowGenerator::validated(const FlowConfig& c) {
  const bool mix_ok = c.passive >= 0 && c.cancel >= 0 && c.passive + c.cancel <= 1;  // no NaN
  if (!mix_ok || c.depth_ticks == 0 || c.max_qty == 0 || c.max_aggressive_qty == 0 ||
      c.max_live == 0 || c.min_price >= c.max_price ||
      c.max_price - c.min_price <= 4 * uint64_t{c.depth_ticks}) {
    throw std::invalid_argument("FlowConfig: invalid mix, depth, quantity or band");
  }
  return c;
}

FlowGenerator::FlowGenerator(const FlowConfig& cfg)
    : cfg_(validated(cfg)),
      passive_cut_(static_cast<uint64_t>(cfg.passive * kScale)),
      cancel_cut_(static_cast<uint64_t>((cfg.passive + cfg.cancel) * kScale)),
      state_(cfg.seed),
      mid_(cfg.min_price + (cfg.max_price - cfg.min_price) / 2),
      live_(cfg.max_live) {}

uint64_t FlowGenerator::rand() {  // splitmix64: tiny, fast, deterministic on every platform
  uint64_t z = (state_ += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

Command FlowGenerator::cancel() {
  const size_t i = below(n_live_);
  const OrderId id = live_[i];
  live_[i] = live_[--n_live_];
  return {id, 0, 0, CommandType::Cancel, Side::Buy, OrderType::Limit, TimeInForce::GTC, 0};
}

Command FlowGenerator::next() {
  const uint64_t r = rand();
  if ((r & 7) == 0) {  // mid drifts one tick on 1 in 8 commands, staying clear of the band
    const Price lo = cfg_.min_price + 2 * cfg_.depth_ticks;
    const Price hi = cfg_.max_price - 2 * cfg_.depth_ticks;
    mid_ = std::clamp<Price>(r & 8 ? mid_ + 1 : mid_ - 1, lo, hi);
  }
  const uint64_t u = r >> 32;  // independent high bits pick the command kind
  if (u >= passive_cut_ && u < cancel_cut_ && n_live_ > 0) return cancel();

  Command c{next_id_++,
            0,
            Quantity(1 + below(cfg_.max_qty)),
            CommandType::New,
            below(2) ? Side::Buy : Side::Sell,
            OrderType::Limit,
            TimeInForce::GTC,
            0};
  const Price off = 1 + below(cfg_.depth_ticks);
  const bool buy = c.side == Side::Buy;
  if (u < cancel_cut_) {  // passive (also taken when a cancel finds nothing live)
    c.price = buy ? mid_ - off : mid_ + off;
    if (n_live_ < live_.size()) {
      live_[n_live_++] = c.id;
    } else {
      live_[below(n_live_)] = c.id;
    }
    return c;
  }
  c.qty = Quantity(1 + below(cfg_.max_aggressive_qty));
  if (below(2)) {  // marketable limit, crossing up to depth ticks
    c.price = buy ? mid_ + off : mid_ - off;
    c.tif = TimeInForce::IOC;
  } else {
    c.order_type = OrderType::Market;
    c.tif = TimeInForce::IOC;
  }
  return c;
}

}  // namespace nexus
