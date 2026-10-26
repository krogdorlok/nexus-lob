// Differential fuzzer: drives MatchingEngine and the reference NaiveEngine with the same seeded
// command stream and requires byte-identical events, top of book and order count after every
// command. Usage: fuzz_differential [--seeds N] [--commands M] [--first-seed S]
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "nexus/common/cli_args.hpp"
#include "nexus/core/matching_engine.hpp"
#include "reference/naive_engine.hpp"

using namespace nexus;

namespace {

// Stream with heavy crossing, partial fills, id reuse and every reject path.
class Fuzzer {
 public:
  Fuzzer(uint64_t seed, Price lo, Price hi) : rng_(seed), lo_(lo), hi_(hi), mid_((lo + hi) / 2) {}

  Command next() {
    const int r = pick(100);
    mid_ = clamp_to_band(static_cast<int64_t>(mid_) + pick(3) - 1);
    if (r < 25)
      return {id_for_ref(),     0, 0, CommandType::Cancel, Side::Buy, OrderType::Limit,
              TimeInForce::GTC, 0};
    if (r < 45)
      return {id_for_ref(),     price(),          qty(), CommandType::Modify, Side::Buy,
              OrderType::Limit, TimeInForce::GTC, 0};
    Command c{new_id(),
              price(),
              qty(),
              CommandType::New,
              pick(2) ? Side::Buy : Side::Sell,
              r < 52 ? OrderType::Market : OrderType::Limit,
              TimeInForce::GTC,
              0};
    const int t = pick(100);
    c.tif = t < 70 ? TimeInForce::GTC : t < 85 ? TimeInForce::IOC : TimeInForce::FOK;
    return c;
  }

 private:
  int pick(int n) { return static_cast<int>(rng_() % static_cast<uint64_t>(n)); }
  Quantity qty() { return pick(100) == 0 ? 0 : Quantity(1 + pick(pick(4) ? 10 : 200)); }
  Price price() {
    const int r = pick(100);
    if (r == 0) return lo_ > 0 ? lo_ - 1 : hi_ + 1;  // just outside the band
    if (r == 1) return hi_ + 1 + static_cast<Price>(pick(3));
    if (r == 2) return pick(2) ? lo_ : hi_;
    if (r == 3) return kNoPrice;
    const int spread = 1 + pick(8);
    return clamp_to_band(static_cast<int64_t>(mid_) + pick(2 * spread + 1) - spread);
  }
  Price clamp_to_band(int64_t p) const {
    return static_cast<Price>(std::clamp(p, static_cast<int64_t>(lo_), static_cast<int64_t>(hi_)));
  }
  OrderId new_id() {
    if (!issued_.empty() && pick(30) == 0) return id_for_ref();  // duplicate or reuse
    issued_.push_back(next_id_);
    return next_id_++;
  }
  // Mostly recent ids (likely live), sometimes old or never-issued ones.
  OrderId id_for_ref() {
    if (issued_.empty() || pick(10) == 0) return next_id_ + static_cast<OrderId>(pick(5));
    const size_t n = issued_.size();
    const size_t back = pick(4)
                            ? static_cast<size_t>(pick(static_cast<int>(std::min<size_t>(n, 64))))
                            : static_cast<size_t>(rng_() % n);
    return issued_[n - 1 - back];
  }

  std::mt19937_64 rng_;
  Price lo_, hi_, mid_;
  OrderId next_id_{1};
  std::vector<OrderId> issued_;
};

// Coverage tallies: a run that never reaches some path fails instead of passing vacuously.
size_t g_types[5], g_reasons[7], g_modify_crossed;

const char* name(EventType t) {
  constexpr const char* kNames[] = {"Accepted", "Fill", "Cancelled", "Modified", "Rejected"};
  return kNames[static_cast<int>(t)];
}

void dump(const char* who, const std::vector<Event>& evs) {
  std::fprintf(stderr, "  %s (%zu events):\n", who, evs.size());
  for (const Event& e : evs) {
    std::fprintf(stderr, "    %-9s id=%llu rest=%llu side=%d px=%llu qty=%u reason=%d\n",
                 name(e.type), (unsigned long long)e.id, (unsigned long long)e.resting_id,
                 (int)e.side, (unsigned long long)e.price, e.qty, (int)e.reason);
  }
}

template <size_t Cap>
bool run_seed(uint64_t seed, uint64_t commands, Price lo, Price hi) {
  auto fast = std::make_unique<MatchingEngine<Cap>>(lo, hi);
  reference::NaiveEngine naive(lo, hi, Cap);
  Fuzzer fuzz(seed, lo, hi);
  std::vector<Event> a, b;
  for (uint64_t i = 0; i < commands; ++i) {
    const Command c = fuzz.next();
    a.clear();
    b.clear();
    fast->process(c, [&](const Event& e) { a.push_back(e); });
    naive.process(c, [&](const Event& e) { b.push_back(e); });
    const bool same_events =
        a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(Event)) == 0;
    const TopOfBook ta = fast->top(), tb = naive.top();
    if (same_events && ta == tb && fast->live_orders() == naive.live_orders()) {
      for (const Event& e : a) {
        ++g_types[static_cast<int>(e.type)];
        ++g_reasons[static_cast<int>(e.reason)];
      }
      g_modify_crossed += c.type == CommandType::Modify && a.size() > 1;
      continue;
    }
    std::fprintf(stderr,
                 "MISMATCH seed=%llu command=%llu cap=%zu band=[%llu,%llu]\n"
                 "  cmd type=%d id=%llu side=%d otype=%d tif=%d px=%llu qty=%u\n",
                 (unsigned long long)seed, (unsigned long long)i, Cap, (unsigned long long)lo,
                 (unsigned long long)hi, (int)c.type, (unsigned long long)c.id, (int)c.side,
                 (int)c.order_type, (int)c.tif, (unsigned long long)c.price, c.qty);
    dump("nexus", a);
    dump("naive", b);
    std::fprintf(stderr, "  top nexus bid=%llu/%llu ask=%llu/%llu live=%zu\n",
                 (unsigned long long)ta.bid_price, (unsigned long long)ta.bid_volume,
                 (unsigned long long)ta.ask_price, (unsigned long long)ta.ask_volume,
                 fast->live_orders());
    std::fprintf(stderr, "  top naive bid=%llu/%llu ask=%llu/%llu live=%zu\n",
                 (unsigned long long)tb.bid_price, (unsigned long long)tb.bid_volume,
                 (unsigned long long)tb.ask_price, (unsigned long long)tb.ask_volume,
                 naive.live_orders());
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  uint64_t seeds = 20, commands = 20'000, first = 1;
  if (argc % 2 == 0) return std::fprintf(stderr, "every flag needs a value\n"), 2;
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string k = argv[i];
    const uint64_t v = parse_flag<uint64_t>(k, argv[i + 1]);
    if (k == "--seeds")
      seeds = v;
    else if (k == "--commands")
      commands = v;
    else if (k == "--first-seed")
      first = v;
    else
      return std::fprintf(stderr, "unknown flag %s\n", k.c_str()), 2;
  }
  for (uint64_t s = first; s < first + seeds; ++s) {
    // Rotate through band widths (one bitmap word, several words, several l1 words) and pool
    // sizes (a tiny pool exercises PoolExhausted constantly).
    bool ok = false;
    switch (s % 4) {
      case 0:
        ok = run_seed<4096>(s, commands, 1000, 1015);
        break;
      case 1:
        ok = run_seed<16>(s, commands, 1, 64);
        break;
      case 2:
        ok = run_seed<4096>(s, commands, 5000, 5300);
        break;
      case 3:
        ok = run_seed<1024>(s, commands, 0, 9000);
        break;
    }
    if (!ok) return 1;
  }
  std::printf("fuzz_differential: %llu seeds x %llu commands, all identical\n",
              (unsigned long long)seeds, (unsigned long long)commands);
  std::printf("  events: accepted=%zu fill=%zu cancelled=%zu modified=%zu rejected=%zu\n",
              g_types[0], g_types[1], g_types[2], g_types[3], g_types[4]);
  std::printf("  rejects: dup=%zu unknown=%zu band=%zu zero=%zu pool=%zu fok=%zu\n", g_reasons[1],
              g_reasons[2], g_reasons[3], g_reasons[4], g_reasons[5], g_reasons[6]);
  std::printf("  modifies that crossed and filled: %zu\n", g_modify_crossed);
  if (g_modify_crossed == 0) return std::fprintf(stderr, "coverage: no crossing modify\n"), 1;
  for (size_t n : g_types) {
    if (n == 0) return std::fprintf(stderr, "coverage: an event type never occurred\n"), 1;
  }
  for (int r = 1; r < 7; ++r) {
    if (g_reasons[r] == 0) return std::fprintf(stderr, "coverage: reject %d never hit\n", r), 1;
  }
  return 0;
}
