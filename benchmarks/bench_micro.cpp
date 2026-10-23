// Per-operation costs for NexusLOB and the naive reference engine. Every timed batch runs on a
// freshly built book (construction and prefill happen with the timer paused), and a batch that
// sees PoolExhausted is reported as an error instead of a number.
#include <benchmark/benchmark.h>

#include <algorithm>
#include <memory>
#include <numeric>
#include <random>
#include <vector>

#include "nexus/core/matching_engine.hpp"
#include "reference/naive_engine.hpp"

using namespace nexus;

namespace {

constexpr Price kMin = 1, kMax = 1 << 16;
constexpr size_t kBatch = 4096;
constexpr size_t kCap = 1 << 16;  // > largest prefill, so the pool can never run dry

using Nexus = MatchingEngine<kCap>;
using Naive = reference::NaiveEngine;

template <class E>
std::unique_ptr<E> make() {
  if constexpr (std::is_same_v<E, Naive>) {
    return std::make_unique<Naive>(kMin, kMax, kCap);
  } else {
    return std::make_unique<E>(kMin, kMax);
  }
}

Command limit(OrderId id, Side s, Price p, Quantity q) {
  return {id, p, q, CommandType::New, s, OrderType::Limit, TimeInForce::GTC, 0};
}

struct Sink {
  uint64_t events = 0, pool_rejects = 0;
  void operator()(const Event& e) {
    ++events;
    pool_rejects += e.reason == RejectReason::PoolExhausted;
  }
};

// Non-crossing passive order i: bids on 64 levels under 30000, asks on 64 levels above it.
Command passive(OrderId i) {
  const bool buy = i % 2;
  const Price off = 1 + (i / 2) % 64;
  return limit(i, buy ? Side::Buy : Side::Sell, buy ? 30000 - off : 30000 + off,
               Quantity(1 + i % 100));
}

bool finish(benchmark::State& state, Sink& sink, uint64_t ops) {
  benchmark::DoNotOptimize(sink.events);
  if (sink.pool_rejects) {
    state.SkipWithError("pool exhausted mid-measurement");
    return false;
  }
  state.SetItemsProcessed(int64_t(ops));
  return true;
}

template <class E>
void BM_AddPassive(benchmark::State& state) {
  std::unique_ptr<E> eng;
  Sink sink;
  uint64_t ops = 0;
  for (auto _ : state) {
    state.PauseTiming();
    eng.reset();
    eng = make<E>();
    state.ResumeTiming();
    for (OrderId i = 1; i <= kBatch; ++i) eng->process(passive(i), sink);
    ops += kBatch;
  }
  finish(state, sink, ops);
}

template <class E>
void BM_Cancel(benchmark::State& state) {
  std::vector<OrderId> order(kBatch);
  std::iota(order.begin(), order.end(), OrderId{1});
  std::shuffle(order.begin(), order.end(), std::mt19937_64(3));  // cancel from mid-queue too
  std::unique_ptr<E> eng;
  Sink sink;
  uint64_t ops = 0;
  for (auto _ : state) {
    state.PauseTiming();
    eng.reset();
    eng = make<E>();
    for (OrderId i = 1; i <= kBatch; ++i) eng->process(passive(i), sink);
    state.ResumeTiming();
    for (OrderId id : order) {
      eng->process(
          {id, 0, 0, CommandType::Cancel, Side::Buy, OrderType::Limit, TimeInForce::GTC, 0}, sink);
    }
    ops += kBatch;
  }
  finish(state, sink, ops);
}

// Modify that moves price (loses priority): unlink, re-match, re-link.
template <class E>
void BM_ModifyPrice(benchmark::State& state) {
  std::unique_ptr<E> eng;
  Sink sink;
  uint64_t ops = 0;
  for (auto _ : state) {
    state.PauseTiming();
    eng.reset();
    eng = make<E>();
    for (OrderId i = 1; i <= kBatch; ++i) eng->process(passive(i), sink);
    state.ResumeTiming();
    for (OrderId i = 1; i <= kBatch; ++i) {
      const Command c = passive(i);
      const Price p = c.side == Side::Buy ? c.price - 64 : c.price + 64;  // still passive
      eng->process(
          {i, p, c.qty, CommandType::Modify, c.side, OrderType::Limit, TimeInForce::GTC, 0}, sink);
    }
    ops += kBatch;
  }
  finish(state, sink, ops);
}

// One aggressive buy that sweeps N price levels (one resting order per level).
template <class E>
void BM_SweepLevels(benchmark::State& state) {
  const auto levels = static_cast<Quantity>(state.range(0));
  constexpr size_t kSweeps = 256;
  std::unique_ptr<E> eng;
  Sink sink;
  uint64_t ops = 0;
  for (auto _ : state) {
    state.PauseTiming();
    eng.reset();
    eng = make<E>();
    OrderId id = 1;
    for (Price p = 10000; p < 10000 + kSweeps * levels; ++p)
      eng->process(limit(id++, Side::Sell, p, 1), sink);
    state.ResumeTiming();
    for (size_t s = 0; s < kSweeps; ++s) {
      const Price top = 10000 + (s + 1) * levels - 1;
      eng->process(limit(id++, Side::Buy, top, levels), sink);
    }
    ops += kSweeps;
  }
  finish(state, sink, ops);
}

}  // namespace

BENCHMARK(BM_AddPassive<Nexus>)->Name("AddPassive/nexus");
BENCHMARK(BM_AddPassive<Naive>)->Name("AddPassive/naive");
BENCHMARK(BM_Cancel<Nexus>)->Name("Cancel/nexus");
BENCHMARK(BM_Cancel<Naive>)->Name("Cancel/naive");
BENCHMARK(BM_ModifyPrice<Nexus>)->Name("ModifyPrice/nexus");
BENCHMARK(BM_ModifyPrice<Naive>)->Name("ModifyPrice/naive");
BENCHMARK(BM_SweepLevels<Nexus>)->Name("Sweep/nexus")->Arg(1)->Arg(8)->Arg(64);
BENCHMARK(BM_SweepLevels<Naive>)->Name("Sweep/naive")->Arg(1)->Arg(8)->Arg(64);

BENCHMARK_MAIN();
