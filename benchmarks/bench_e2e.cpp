// End-to-end pipeline benchmark. Per repetition and engine it runs the pipeline twice:
//   flood: the producer pushes as fast as the queues accept -> throughput (commands/s)
//   paced: the producer pushes at --rate commands/s           -> latency percentiles (ns)
// and writes the median, min and max over repetitions to <out>/e2e_<engine>.json.
// Usage: bench_e2e [--engine nexus|naive|both] [--reps 5] [--commands 10000000]
//                  [--warmup 1000000] [--rate 1000000] [--cpu 2] [--out results]
//        bench_e2e --replay nexus|naive|none [--commands N]
// --replay runs one engine single-threaded over a pregenerated command stream (no queues, no
// spinning threads), the workload scripts/perf_compare.sh counts with perf stat. `none` only
// generates the stream; perf_compare subtracts it so counters reflect the engine alone.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "nexus/common/tsc.hpp"
#include "pipeline.hpp"

using namespace nexus;

namespace {

struct Stat {
  std::vector<double> runs;
  double median() const {
    std::vector<double> v = runs;
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
  }
};

void write_stat(FILE* f, const char* name, const Stat& s, bool last) {
  std::fprintf(f, "    \"%s\": {\"median\": %.1f, \"min\": %.1f, \"max\": %.1f, \"runs\": [", name,
               s.median(), *std::min_element(s.runs.begin(), s.runs.end()),
               *std::max_element(s.runs.begin(), s.runs.end()));
  for (size_t i = 0; i < s.runs.size(); ++i) {
    std::fprintf(f, "%s%.1f", i ? ", " : "", s.runs[i]);
  }
  std::fprintf(f, "]}%s\n", last ? "" : ",");
}

struct Series {
  const char* name;
  Stat tput, p50, p90, p99, p999, pmax;
};

// One repetition for one engine: a flood run for throughput, then a paced run for latency.
template <class Make>
bool run_rep(Series& s, Make make, const PipelineConfig& base, double ns, int rep) {
  PipelineConfig flood = base;
  flood.rate = 0;
  auto e1 = make();
  const PipelineResult f = run_pipeline(*e1, flood);
  e1.reset();
  auto e2 = make();
  const PipelineResult p = run_pipeline(*e2, base);
  if (f.pool_rejects || p.pool_rejects) {
    return std::fprintf(stderr, "%s: pool exhausted, run invalid\n", s.name), false;
  }
  s.tput.runs.push_back(double(flood.commands) / f.seconds);
  const auto& h = p.latency;
  s.p50.runs.push_back(double(h.percentile(0.50)) * ns);
  s.p90.runs.push_back(double(h.percentile(0.90)) * ns);
  s.p99.runs.push_back(double(h.percentile(0.99)) * ns);
  s.p999.runs.push_back(double(h.percentile(0.999)) * ns);
  s.pmax.runs.push_back(double(h.max()) * ns);
  std::fprintf(stderr, "%s rep %d: %.0f cmds/s, paced p50=%.0f p99=%.0f p99.9=%.0f ns\n", s.name,
               rep + 1, s.tput.runs.back(), s.p50.runs.back(), s.p99.runs.back(),
               s.p999.runs.back());
  return true;
}

bool write_json(const Series& s, const PipelineConfig& base, double ns,
                const std::string& out_dir) {
  const std::string path = out_dir + "/e2e_" + s.name + ".json";
  FILE* f = std::fopen(path.c_str(), "w");
  if (!f) return std::fprintf(stderr, "cannot write %s\n", path.c_str()), false;
  std::fprintf(f, "{\n  \"engine\": \"%s\",\n  \"reps\": %zu,\n  \"commands\": %llu,\n", s.name,
               s.tput.runs.size(), (unsigned long long)base.commands);
  std::fprintf(f, "  \"warmup\": %llu,\n  \"paced_rate_cmds_per_s\": %.0f,\n",
               (unsigned long long)base.warmup, base.rate);
  std::fprintf(f, "  \"ns_per_tick\": %.4f,\n", ns);
  const FlowConfig& fc = base.flow;
  std::fprintf(f,
               "  \"flow\": {\"passive\": %.2f, \"cancel\": %.2f, \"marketable\": %.2f, "
               "\"depth_ticks\": %u, \"max_live\": %zu, \"seed\": %llu},\n",
               fc.passive, fc.cancel, 1 - fc.passive - fc.cancel, fc.depth_ticks, fc.max_live,
               (unsigned long long)fc.seed);
  std::fprintf(f,
               "  \"latency_definition\": \"producer tsc_begin() stamp just before pushing a "
               "command to the inbound SPSC queue, to publisher tsc_end() when it dequeues that "
               "command's first event; paced run\",\n");
  std::fprintf(f, "  \"throughput_cmds_per_s\": {\n");
  write_stat(f, "flood", s.tput, true);
  std::fprintf(f, "  },\n  \"latency_ns\": {\n");
  write_stat(f, "p50", s.p50, false);
  write_stat(f, "p90", s.p90, false);
  write_stat(f, "p99", s.p99, false);
  write_stat(f, "p99_9", s.p999, false);
  write_stat(f, "max", s.pmax, true);
  std::fprintf(f, "  }\n}\n");
  std::fclose(f);
  std::printf("wrote %s\n", path.c_str());
  return true;
}

template <class Engine>
int replay(Engine& engine, uint64_t n, const FlowConfig& flow_cfg) {
  FlowGenerator flow(flow_cfg);
  std::vector<Command> cmds(n);
  for (Command& c : cmds) c = flow.next();
  uint64_t events = 0;
  const auto t0 = std::chrono::steady_clock::now();
  for (const Command& c : cmds) engine.process(c, [&](const Event&) { ++events; });
  const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("replay: %llu commands, %llu events, %.0f commands/s\n", (unsigned long long)n,
              (unsigned long long)events, double(n) / s);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  PipelineConfig cfg;
  cfg.rate = 1e6;
  std::string engine = "both", out = "results", replay_engine;
  int reps = 5;
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string k = argv[i], v = argv[i + 1];
    if (k == "--engine") {
      engine = v;
    } else if (k == "--reps") {
      reps = std::atoi(v.c_str());
    } else if (k == "--commands") {
      cfg.commands = std::strtoull(v.c_str(), nullptr, 10);
    } else if (k == "--warmup") {
      cfg.warmup = std::strtoull(v.c_str(), nullptr, 10);
    } else if (k == "--rate") {
      cfg.rate = std::strtod(v.c_str(), nullptr);
    } else if (k == "--cpu") {
      cfg.engine_cpu = std::atoi(v.c_str());
    } else if (k == "--out") {
      out = v;
    } else if (k == "--replay") {
      replay_engine = v;
    } else {
      return std::fprintf(stderr, "unknown flag %s\n", k.c_str()), 2;
    }
  }
  if (reps < 1 || cfg.rate <= 0 || cfg.commands == 0 ||
      (engine != "nexus" && engine != "naive" && engine != "both")) {
    return std::fprintf(stderr, "bad arguments\n"), 2;
  }
  const Price lo = cfg.flow.min_price, hi = cfg.flow.max_price;
  if (replay_engine == "nexus")
    return replay(*std::make_unique<NexusEngine>(lo, hi), cfg.commands, cfg.flow);
  if (replay_engine == "naive") {
    NaiveEngine e(lo, hi, kPipelineMaxOrders);
    return replay(e, cfg.commands, cfg.flow);
  }
  if (replay_engine == "none") {
    FlowGenerator flow(cfg.flow);
    std::vector<Command> cmds(cfg.commands);
    for (Command& c : cmds) c = flow.next();
    std::printf("replay: generated %zu commands, no engine\n", cmds.size());
    return 0;
  }
  if (!replay_engine.empty()) {
    return std::fprintf(stderr, "--replay must be nexus, naive or none\n"), 2;
  }
  if (!check_tsc_flags()) {
    return std::fprintf(stderr, "refusing to measure cross-core latency with this TSC\n"), 1;
  }
  // Engines alternate within every repetition (and swap order each time), so slow drifts in
  // background load hit both engines alike instead of biasing the comparison.
  const double ns = calibrate_ns_per_tick();
  Series nexus{"nexus", {}, {}, {}, {}, {}, {}}, naive{"naive", {}, {}, {}, {}, {}, {}};
  auto make_nexus = [&] { return std::make_unique<NexusEngine>(lo, hi); };
  auto make_naive = [&] { return std::make_unique<NaiveEngine>(lo, hi, kPipelineMaxOrders); };
  const bool do_nexus = engine != "naive", do_naive = engine != "nexus";
  for (int rep = 0; rep < reps; ++rep) {
    const bool nexus_first = rep % 2 == 0;
    for (int k = 0; k < 2; ++k) {
      const bool is_nexus = (k == 0) == nexus_first;
      const bool ok = is_nexus ? !do_nexus || run_rep(nexus, make_nexus, cfg, ns, rep)
                               : !do_naive || run_rep(naive, make_naive, cfg, ns, rep);
      if (!ok) return 1;
    }
  }
  if (do_nexus && !write_json(nexus, cfg, ns, out)) return 1;
  if (do_naive && !write_json(naive, cfg, ns, out)) return 1;
  return 0;
}
