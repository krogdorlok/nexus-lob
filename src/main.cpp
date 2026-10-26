// Runs the pipeline once and prints throughput and latency.
// Usage: nexus_lob [--engine nexus|naive] [--commands N] [--warmup N] [--rate R] [--cpu C]
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <string>

#include "nexus/common/cli_args.hpp"
#include "nexus/common/tsc.hpp"
#include "pipeline.hpp"

using namespace nexus;

namespace {

int run(int argc, char** argv) {
  PipelineConfig cfg;
  std::string engine = "nexus";
  if (argc % 2 == 0) return std::fprintf(stderr, "every flag needs a value\n"), 2;
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string k = argv[i], v = argv[i + 1];
    if (k == "--engine")
      engine = v;
    else if (k == "--commands")
      cfg.commands = parse_flag<uint64_t>(k, v);
    else if (k == "--warmup")
      cfg.warmup = parse_flag<uint64_t>(k, v);
    else if (k == "--rate")
      cfg.rate = parse_flag<double>(k, v);
    else if (k == "--cpu")
      cfg.engine_cpu = parse_flag<int>(k, v);
    else
      return std::fprintf(stderr, "unknown flag %s\n", k.c_str()), 2;
  }
  if (engine != "nexus" && engine != "naive") {
    return std::fprintf(stderr, "--engine must be nexus or naive\n"), 2;
  }
  if (!check_tsc_flags()) {
    return std::fprintf(stderr, "refusing to measure cross-core latency with this TSC\n"), 1;
  }
  const double ns = calibrate_ns_per_tick();

  PipelineResult r;
  if (engine == "nexus") {
    auto e = std::make_unique<NexusEngine>(cfg.flow.min_price, cfg.flow.max_price);
    r = run_pipeline(*e, cfg);
  } else {
    NaiveEngine e(cfg.flow.min_price, cfg.flow.max_price, kPipelineMaxOrders);
    r = run_pipeline(e, cfg);
  }
  const auto& h = r.latency;
  std::printf("engine=%s commands=%llu rate=%s\n", engine.c_str(), (unsigned long long)cfg.commands,
              cfg.rate > 0 ? std::to_string(cfg.rate).c_str() : "flood");
  std::printf("throughput: %.0f commands/s\n", double(cfg.commands) / r.seconds);
  std::printf("latency ns: p50=%.0f p90=%.0f p99=%.0f p99.9=%.0f max=%.0f (tick=%.2f ns)\n",
              h.percentile(0.50) * ns, h.percentile(0.90) * ns, h.percentile(0.99) * ns,
              h.percentile(0.999) * ns, double(h.max()) * ns, ns);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception& e) {  // e.g. a pipeline configuration the engine rejects
    std::fprintf(stderr, "error: %s\n", e.what());
    return 2;
  }
}
