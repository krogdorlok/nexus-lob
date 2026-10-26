// Proves the hot path never touches the heap: all 12 replaceable global operator new/delete pairs
// (plain, array, aligned, nothrow, aligned nothrow, sized) are replaced with counting versions,
// and a 1M-command mixed workload must leave the counter at zero.
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <memory>
#include <new>
#include <vector>

#include "nexus/common/histogram.hpp"
#include "nexus/common/tsc.hpp"
#include "nexus/core/matching_engine.hpp"
#include "nexus/queue/spsc_ring_buffer.hpp"

// On glibc, raw malloc/calloc/realloc are interposed and counted too. On macOS a definition in
// the executable does not interpose libc, so only the C++ allocation functions are counted.
// The engine is header-only C++ and never calls malloc directly, so operator new is the path
// that matters. (Not built under sanitizers, which own the allocator.)
#if defined(__GLIBC__)
#define NEXUS_COUNT_MALLOC 1
extern "C" {
void* __libc_malloc(size_t);
void* __libc_calloc(size_t, size_t);
void* __libc_realloc(void*, size_t);
}
#endif

namespace {

std::atomic<bool> g_counting{false};
std::atomic<size_t> g_allocs{0};

void count() {
  if (g_counting.load(std::memory_order_relaxed)) g_allocs.fetch_add(1);
}

void* counted_alloc(size_t n, size_t align) {
  count();
  void* p = nullptr;
  if (align <= alignof(std::max_align_t)) {
#ifdef NEXUS_COUNT_MALLOC
    p = __libc_malloc(n ? n : 1);  // bypass the interposed malloc so one new counts once
#else
    p = std::malloc(n ? n : 1);
#endif
  } else if (posix_memalign(&p, align, n ? n : 1) != 0) {
    p = nullptr;
  }
  return p;
}

}  // namespace

void* operator new(size_t n) {
  if (void* p = counted_alloc(n, 0)) return p;
  throw std::bad_alloc();
}
void* operator new[](size_t n) { return ::operator new(n); }
void* operator new(size_t n, std::align_val_t a) {
  if (void* p = counted_alloc(n, static_cast<size_t>(a))) return p;
  throw std::bad_alloc();
}
void* operator new[](size_t n, std::align_val_t a) { return ::operator new(n, a); }
void* operator new(size_t n, const std::nothrow_t&) noexcept { return counted_alloc(n, 0); }
void* operator new[](size_t n, const std::nothrow_t&) noexcept { return counted_alloc(n, 0); }
void* operator new(size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
  return counted_alloc(n, static_cast<size_t>(a));
}
void* operator new[](size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
  return counted_alloc(n, static_cast<size_t>(a));
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { std::free(p); }

#ifdef NEXUS_COUNT_MALLOC
extern "C" {
void* malloc(size_t n) {
  count();
  return __libc_malloc(n);
}
void* calloc(size_t n, size_t m) {
  count();
  return __libc_calloc(n, m);
}
void* realloc(void* p, size_t n) {
  count();
  return __libc_realloc(p, n);
}
}
#endif

namespace {

using namespace nexus;

// Mixed workload with preallocated state only: passive adds near a drifting mid, cancels and
// modifies of tracked ids (some already filled), IOC/FOK/market aggressors, junk commands.
class Workload {
 public:
  explicit Workload(size_t max_live) : live_(max_live) {}

  Command next() {
    const uint64_t r = rand() % 100;
    mid_ = std::clamp<Price>(mid_ + rand() % 3 - 1, 1100, 1900);
    if (n_live_ == live_.size() || (r < 30 && n_live_ > 0)) return cancel();
    if (r < 40 && n_live_ > 0) return modify();
    Command c{next_id_++,
              0,
              Quantity(1 + rand() % 50),
              CommandType::New,
              rand() % 2 ? Side::Buy : Side::Sell,
              OrderType::Limit,
              TimeInForce::GTC,
              0};
    const Price off = 1 + rand() % 20;
    if (r < 85) {  // passive
      c.price = c.side == Side::Buy ? mid_ - off : mid_ + off;
      live_[n_live_++] = c.id;
    } else if (r < 97) {  // marketable: limit IOC/FOK, market with any tif
      c.price = c.side == Side::Buy ? mid_ + off : mid_ - off;
      c.tif = static_cast<TimeInForce>(1 + rand() % 2);
      if (r >= 93) {
        c.order_type = OrderType::Market;
        c.tif = static_cast<TimeInForce>(rand() % 3);
      }
    } else if (r == 97) {  // out of band
      c.price = 5000;
    } else if (r == 98 || n_live_ == 0) {  // zero qty
      c.qty = 0;
    } else {  // duplicate id when still live; IOC so it can never rest untracked
      c.id = live_[0];
      c.price = mid_;
      c.tif = TimeInForce::IOC;
    }
    return c;
  }

 private:
  uint64_t rand() {  // splitmix64
    uint64_t z = (state_ += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  Command cancel() {
    const size_t i = rand() % n_live_;
    const OrderId id = live_[i];
    live_[i] = live_[--n_live_];
    return {id, 0, 0, CommandType::Cancel, Side::Buy, OrderType::Limit, TimeInForce::GTC, 0};
  }
  Command modify() {
    const OrderId id = live_[rand() % n_live_];
    const Price p = mid_ + rand() % 41 - 20;
    return {id,
            p,
            Quantity(1 + rand() % 50),
            CommandType::Modify,
            Side::Buy,
            OrderType::Limit,
            TimeInForce::GTC,
            0};
  }

  std::vector<OrderId> live_;
  size_t n_live_{0};
  OrderId next_id_{1};
  Price mid_{1500};
  uint64_t state_{42};
};

TEST(NoAlloc, CounterDetectsAllocations) {
  g_allocs = 0;
  g_counting = true;
  void* volatile p = ::operator new(16);
  g_counting = false;
  ::operator delete(p);
  EXPECT_EQ(g_allocs.load(), 1u);
}

#ifdef NEXUS_COUNT_MALLOC
TEST(NoAlloc, CounterDetectsRawMalloc) {
  g_allocs = 0;
  g_counting = true;
  void* volatile p = std::malloc(16);
  g_counting = false;
  std::free(p);
  EXPECT_EQ(g_allocs.load(), 1u);
}
#endif

// Every reject reason and the market remainder paths, deterministically, while counting.
TEST(NoAlloc, RejectAndRemainderPathsDoNotAllocate) {
  auto engine = std::make_unique<MatchingEngine<2>>(100, 200);
  auto sink = [](const Event&) {};
  auto run = [&](OrderId id, CommandType t, Side s, OrderType o, TimeInForce f, Price p,
                 Quantity q) { engine->process({id, p, q, t, s, o, f, 0}, sink); };
  constexpr auto N = CommandType::New, X = CommandType::Cancel, M = CommandType::Modify;
  constexpr auto L = OrderType::Limit, K = OrderType::Market;
  constexpr auto G = TimeInForce::GTC, I = TimeInForce::IOC, F = TimeInForce::FOK;
  run(1, N, Side::Buy, L, G, 150, 5);  // warm the first slots before counting
  run(1, X, Side::Buy, L, G, 0, 0);

  g_allocs = 0;
  g_counting = true;
  run(1, N, Side::Buy, L, G, 150, 5);
  run(2, N, Side::Buy, L, G, 150, 0);    // ZeroQty
  run(3, N, Side::Buy, L, G, 999, 5);    // PriceOutOfBand
  run(1, N, Side::Sell, L, G, 160, 5);   // DuplicateId
  run(4, N, Side::Buy, L, G, 149, 5);    // fills the pool
  run(5, N, Side::Buy, L, G, 148, 5);    // PoolExhausted
  run(6, N, Side::Sell, L, F, 150, 99);  // FokUnfillable
  run(7, X, Side::Buy, L, G, 0, 0);      // UnknownId (cancel)
  run(7, M, Side::Buy, L, G, 150, 1);    // UnknownId (modify)
  run(8, N, Side::Sell, K, I, 0, 7);     // market IOC: sweeps two levels
  run(9, N, Side::Sell, K, G, 0, 99);    // market GTC: sweep, cancel remainder
  run(10, N, Side::Sell, K, F, 0, 1);    // market FOK on empty book
  g_counting = false;

  EXPECT_EQ(g_allocs.load(), 0u);
  EXPECT_EQ(engine->live_orders(), 0u);
}

TEST(NoAlloc, MillionMixedCommandsAfterWarmup) {
  constexpr size_t kMaxLive = 1 << 14;
  auto engine = std::make_unique<MatchingEngine<kMaxLive>>(1000, 2000);
  Workload flow(kMaxLive);
  size_t events = 0, rejects_pool = 0;
  auto sink = [&](const Event& e) {
    ++events;
    rejects_pool += e.reason == RejectReason::PoolExhausted;
  };
  for (int i = 0; i < 100'000; ++i) engine->process(flow.next(), sink);

  g_allocs = 0;
  g_counting = true;
  for (int i = 0; i < 1'000'000; ++i) engine->process(flow.next(), sink);
  g_counting = false;

  EXPECT_EQ(g_allocs.load(), 0u);
  EXPECT_GT(events, 1'000'000u);
  EXPECT_GT(engine->live_orders(), 0u);
  EXPECT_EQ(rejects_pool, 0u);
}

// The pipeline's whole per-command path, single-threaded: stamp, inbound SPSC push and pop,
// process(), every event through the outbound SPSC, and the latency histogram.
TEST(NoAlloc, FullDataPathAfterWarmup) {
  constexpr size_t kMaxLive = 1 << 14;
  auto engine = std::make_unique<MatchingEngine<kMaxLive>>(1000, 2000);
  auto in = std::make_unique<SPSCRingBuffer<Command, 1024>>();
  auto out = std::make_unique<SPSCRingBuffer<Event, 1024>>();
  auto hist = std::make_unique<LatencyHistogram>();
  Workload flow(kMaxLive);
  auto one_command = [&] {
    Command c = flow.next();
    c.ts = tsc_begin();
    ASSERT_TRUE(in->push(c));
    ASSERT_TRUE(in->pop(c));
    engine->process(c, [&](const Event& e) { ASSERT_TRUE(out->push(e)); });
    Event e;
    while (out->pop(e)) {
    }
    hist->record(tsc_end() - c.ts);
  };
  for (int i = 0; i < 100'000; ++i) one_command();

  g_allocs = 0;
  g_counting = true;
  for (int i = 0; i < 1'000'000; ++i) one_command();
  g_counting = false;

  EXPECT_EQ(g_allocs.load(), 0u);
  EXPECT_EQ(hist->count(), 1'100'000u);
}

}  // namespace
