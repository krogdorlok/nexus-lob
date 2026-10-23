#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "nexus/common/types.hpp"

namespace nexus {

struct FlowConfig {
  uint64_t seed = 1;
  Price min_price = 1;
  Price max_price = 1 << 16;
  double passive = 0.55;              // GTC limits 1..depth ticks behind mid
  double cancel = 0.35;               // cancels of ids this generator issued (some already filled)
  uint32_t depth_ticks = 20;          // the remaining share is marketable (IOC limits and markets)
  Quantity max_qty = 100;             // passive size is uniform in [1, max_qty]
  Quantity max_aggressive_qty = 300;  // larger, so fills drain what cancels leave behind
  size_t max_live = 1 << 16;          // ids tracked for cancels; at the cap a new passive
                                      // order evicts a random tracked id (often already filled)
};

// Synthetic order flow: the mid price follows a random walk, passive orders cluster near it,
// cancels target tracked ids. The realized mix equals the configured one because a full id
// table evicts instead of forcing cancels. All state is preallocated, so next() never
// allocates and the producer thread adds no heap noise to latency measurements.
class FlowGenerator {
 public:
  explicit FlowGenerator(const FlowConfig& cfg);
  Command next();

 private:
  static const FlowConfig& validated(const FlowConfig& cfg);
  uint64_t rand();
  uint64_t below(uint64_t n) { return rand() % n; }
  Command cancel();

  FlowConfig cfg_;
  uint64_t passive_cut_, cancel_cut_;  // thresholds on a 2^32 scale
  uint64_t state_;
  Price mid_;
  OrderId next_id_{1};
  std::vector<OrderId> live_;
  size_t n_live_{0};
};

}  // namespace nexus
