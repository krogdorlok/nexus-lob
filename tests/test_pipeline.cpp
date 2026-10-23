#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>

#include "flow_generator.hpp"
#include "nexus/common/tsc.hpp"
#include "pipeline.hpp"

using namespace nexus;

TEST(Timer, AdvancesAcrossSleepAndCalibrates) {
  const uint64_t a = tsc_begin();
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  const uint64_t b = tsc_end();
  ASSERT_GT(b, a);
  const double ns = calibrate_ns_per_tick();
  EXPECT_GT(ns, 0.01);  // faster than 100 GHz is not a real clock
  EXPECT_LT(ns, 1000.0);
  EXPECT_NEAR(double(b - a) * ns, 10e6, 9e6);  // sleep may overshoot; must not be wildly off
}

// The pipeline subtracts stamps taken on different threads (and cores), so the timer must
// never step backwards across threads: ping-pong a stamp and require it to only grow.
TEST(Timer, MonotonicAcrossThreads) {
  constexpr int kRounds = 100'000;
  std::atomic<uint64_t> last{tsc_begin()};
  std::atomic<int> turn{0};
  std::atomic<int> backwards{0};
  auto player = [&](int me) {
    for (int i = 0; i < kRounds; ++i) {
      while (turn.load(std::memory_order_acquire) != me) {
      }
      const uint64_t now = tsc_begin();
      if (now < last.load(std::memory_order_relaxed)) backwards.fetch_add(1);
      last.store(now, std::memory_order_relaxed);
      turn.store(1 - me, std::memory_order_release);
    }
  };
  std::thread other(player, 1);
  player(0);
  other.join();
  EXPECT_EQ(backwards.load(), 0);
}

TEST(FlowGenerator, DeterministicAndRoughlyMatchesMix) {
  FlowConfig cfg;
  FlowGenerator a(cfg), b(cfg);
  size_t passive = 0, cancels = 0, marketable = 0;
  constexpr int kN = 200'000;
  for (int i = 0; i < kN; ++i) {
    const Command x = a.next(), y = b.next();
    ASSERT_EQ(std::memcmp(&x, &y, sizeof x), 0);
    if (x.type == CommandType::Cancel) {
      ++cancels;
    } else if (x.tif == TimeInForce::GTC) {
      ++passive;
      ASSERT_TRUE(x.price >= cfg.min_price && x.price <= cfg.max_price);
    } else {
      ++marketable;
    }
  }
  EXPECT_NEAR(double(passive) / kN, 0.55, 0.02);
  EXPECT_NEAR(double(cancels) / kN, 0.35, 0.02);
  EXPECT_NEAR(double(marketable) / kN, 0.10, 0.02);
}

TEST(FlowGenerator, RejectsBadConfig) {
  auto bad = [](auto mutate) {
    FlowConfig cfg;
    mutate(cfg);
    EXPECT_THROW(FlowGenerator{cfg}, std::invalid_argument);
  };
  bad([](FlowConfig& c) { c.passive = 0.8, c.cancel = 0.3; });
  bad([](FlowConfig& c) { c.passive = -0.1; });
  bad([](FlowConfig& c) { c.cancel = std::numeric_limits<double>::quiet_NaN(); });
  bad([](FlowConfig& c) { c.depth_ticks = 0; });
  bad([](FlowConfig& c) { c.max_qty = 0; });
  bad([](FlowConfig& c) { c.max_live = 0; });
  bad([](FlowConfig& c) { c.min_price = 10, c.max_price = 5; });
  bad([](FlowConfig& c) { c.min_price = 1, c.max_price = 40; });  // too narrow for depth 20
}

TEST(FlowGenerator, MixHoldsBeyondIdTableSaturation) {
  FlowConfig cfg;
  cfg.max_live = 64;  // saturates within a few hundred commands
  FlowGenerator g(cfg);
  size_t passive = 0, cancels = 0;
  constexpr int kN = 500'000;
  for (int i = 0; i < kN; ++i) {
    const Command c = g.next();
    cancels += c.type == CommandType::Cancel;
    passive += c.type == CommandType::New && c.tif == TimeInForce::GTC;
  }
  EXPECT_NEAR(double(passive) / kN, 0.55, 0.01);
  EXPECT_NEAR(double(cancels) / kN, 0.35, 0.01);
}

TEST(Pipeline, RejectsInvalidRunShape) {
  auto engine = std::make_unique<NexusEngine>(FlowConfig{}.min_price, FlowConfig{}.max_price);
  PipelineConfig cfg;
  cfg.engine_cpu = -1;
  cfg.commands = 0;
  EXPECT_THROW(run_pipeline(*engine, cfg), std::invalid_argument);
  cfg.commands = 1;
  cfg.warmup = UINT64_MAX;
  EXPECT_THROW(run_pipeline(*engine, cfg), std::invalid_argument);
  cfg.warmup = 0;
  cfg.flow.max_live = kPipelineMaxOrders + 1;
  EXPECT_THROW(run_pipeline(*engine, cfg), std::invalid_argument);
}

// Runs the three-thread pipeline end to end (also the TSan target for the pipeline).
template <class Engine>
void smoke(Engine& engine) {
  PipelineConfig cfg;
  cfg.warmup = 20'000;
  cfg.commands = 100'000;
  cfg.engine_cpu = -1;
  const PipelineResult r = run_pipeline(engine, cfg);
  EXPECT_EQ(r.latency.count(), cfg.commands);
  EXPECT_GT(r.events, cfg.warmup + cfg.commands);
  EXPECT_GT(r.seconds, 0.0);
  EXPECT_GT(r.latency.max(), 0u);
  EXPECT_EQ(r.pool_rejects, 0u);
}

TEST(Pipeline, NexusEngineEndToEnd) {
  auto engine = std::make_unique<NexusEngine>(FlowConfig{}.min_price, FlowConfig{}.max_price);
  smoke(*engine);
  EXPECT_GT(engine->live_orders(), 0u);
}

TEST(Pipeline, NaiveEngineEndToEnd) {
  NaiveEngine engine(FlowConfig{}.min_price, FlowConfig{}.max_price, kPipelineMaxOrders);
  smoke(engine);
}

TEST(Pipeline, PacedModeRecordsEveryCommand) {
  auto engine = std::make_unique<NexusEngine>(FlowConfig{}.min_price, FlowConfig{}.max_price);
  PipelineConfig cfg;
  cfg.warmup = 1'000;
  cfg.commands = 20'000;
  cfg.rate = 200'000;  // 0.1 s of paced flow
  cfg.engine_cpu = -1;
  const PipelineResult r = run_pipeline(*engine, cfg);
  EXPECT_EQ(r.latency.count(), cfg.commands);
  EXPECT_GT(r.seconds, 0.08);  // pacing actually throttled the producer
}
