#pragma once
#include <cstdint>

#include "flow_generator.hpp"
#include "nexus/common/histogram.hpp"
#include "nexus/core/matching_engine.hpp"
#include "reference/naive_engine.hpp"

namespace nexus {

inline constexpr size_t kPipelineMaxOrders = size_t{1} << 17;
using NexusEngine = MatchingEngine<kPipelineMaxOrders>;
using NaiveEngine = reference::NaiveEngine;

struct PipelineConfig {
  FlowConfig flow;
  uint64_t warmup = 1'000'000;
  uint64_t commands = 10'000'000;  // measured after warmup
  double rate = 0;                 // commands per second; 0 = as fast as the engine drains
  int engine_cpu = 2;              // core for the engine thread; -1 = do not pin
};

struct PipelineResult {
  LatencyHistogram latency;   // TSC ticks, one sample per measured command
  double seconds = 0;         // first measured push to the last command's first event dequeued
  uint64_t events = 0;        // events dequeued by the publisher, warmup included
  uint64_t pool_rejects = 0;  // PoolExhausted events; a valid benchmark run has none
};

// producer --SPSC--> engine (pinned) --SPSC--> publisher
// Throws std::invalid_argument if cfg.flow.max_live could exceed kPipelineMaxOrders, if
// commands is 0, or if warmup + commands overflows.
// Latency of a command = tsc_end() when the publisher dequeues the command's first event
// minus the tsc_begin() stamp the producer wrote just before pushing the command.
template <class Engine>
PipelineResult run_pipeline(Engine& engine, const PipelineConfig& cfg);

// Pins the calling thread to `cpu` on Linux. Elsewhere it warns once and, on macOS, asks the
// scheduler for the highest QoS class so the thread prefers performance cores.
void pin_current_thread(int cpu);

}  // namespace nexus
