#include "pipeline.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <thread>

#include "nexus/common/tsc.hpp"
#include "nexus/queue/spsc_ring_buffer.hpp"

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#elif defined(__APPLE__)
#include <pthread.h>
#include <sys/qos.h>
#endif

namespace nexus {

void pin_current_thread(int cpu) {
#if defined(__linux__)
  if (cpu < 0) return;
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0) {
    std::fprintf(stderr, "warning: could not pin thread to cpu %d\n", cpu);
  }
#else
  static std::atomic<bool> warned{false};
  if (cpu >= 0 && !warned.exchange(true)) {
    std::fprintf(stderr, "warning: thread pinning is Linux-only; running unpinned\n");
  }
#if defined(__APPLE__)
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
#endif
}

namespace {

struct Outbound {
  Event ev;
  Timestamp origin_ts;  // producer stamp on a command's first event, 0 on the rest
};

}  // namespace

template <class Engine>
PipelineResult run_pipeline(Engine& engine, const PipelineConfig& cfg) {
  if (cfg.flow.max_live > kPipelineMaxOrders) {
    throw std::invalid_argument("flow.max_live exceeds the engine's order pool");
  }
  if (cfg.commands == 0 || cfg.warmup > UINT64_MAX - cfg.commands) {
    throw std::invalid_argument("need commands > 0 and warmup + commands < 2^64");
  }
  auto in = std::make_unique<SPSCRingBuffer<Command, 1 << 16>>();
  auto out = std::make_unique<SPSCRingBuffer<Outbound, 1 << 16>>();
  auto result = std::make_unique<PipelineResult>();
  FlowGenerator flow(cfg.flow);
  const uint64_t total = cfg.warmup + cfg.commands;
  std::atomic<bool> engine_done{false};
  const double ns_per_tick = calibrate_ns_per_tick();
  uint64_t start_tsc = 0, end_tsc = 0;  // first measured push, last first-event dequeue

  std::thread engine_thread([&] {
    pin_current_thread(cfg.engine_cpu);
    Command c;
    for (uint64_t n = 0; n < total;) {
      if (!in->pop(c)) continue;
      Timestamp origin = c.ts;
      engine.process(c, [&](const Event& e) {
        const Outbound o{e, origin};
        origin = 0;
        while (!out->push(o)) {
        }
      });
      ++n;
    }
    engine_done.store(true, std::memory_order_release);
  });

  std::thread publisher([&] {
    pin_current_thread(-1);
    uint64_t firsts = 0;
    auto handle = [&](const Outbound& o, uint64_t now) {
      ++result->events;
      result->pool_rejects += o.ev.reason == RejectReason::PoolExhausted;
      if (o.origin_ts == 0) return;
      if (firsts++ >= cfg.warmup) result->latency.record(now - o.origin_ts);
      end_tsc = now;
    };
    Outbound o;
    for (;;) {
      if (out->pop(o)) {
        handle(o, o.origin_ts ? tsc_end() : 0);  // stamp first, before any bookkeeping
      } else if (engine_done.load(std::memory_order_acquire)) {
        while (out->pop(o)) handle(o, o.origin_ts ? tsc_end() : 0);
        break;
      }
    }
  });

  // Producer: this thread. Paced mode schedules pushes on a fixed TSC grid (open loop).
  pin_current_thread(-1);
  const double ticks_per_cmd = cfg.rate > 0 ? 1e9 / cfg.rate / ns_per_tick : 0;
  double deadline = double(tsc_begin());
  for (uint64_t i = 0; i < total; ++i) {
    Command c = flow.next();
    if (ticks_per_cmd > 0) {
      deadline += ticks_per_cmd;
      while (double(tsc_begin()) < deadline) {
      }
    }
    c.ts = tsc_begin();  // stamped once; time spent blocked on a full queue counts as latency
    if (i == cfg.warmup) start_tsc = c.ts;
    while (!in->push(c)) {
    }
  }
  engine_thread.join();
  publisher.join();
  result->seconds = double(end_tsc - start_tsc) * ns_per_tick * 1e-9;
  return std::move(*result);
}

template PipelineResult run_pipeline(NexusEngine&, const PipelineConfig&);
template PipelineResult run_pipeline(NaiveEngine&, const PipelineConfig&);

}  // namespace nexus
