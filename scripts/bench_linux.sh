#!/usr/bin/env bash
# Regenerates everything in results/ from a clean checkout, then the README results table.
# Intended for a quiet Linux x86_64 box with perf (for example an AWS c7i instance); on other
# platforms it runs everything except hardware counters, which stay TBD.
#   REPS=5 scripts/bench_linux.sh
set -euo pipefail
cd "$(dirname "$0")/.."
REPS=${REPS:-5}
BUILD=build/bench
mkdir -p results

cmake --preset release -B "$BUILD" -DNEXUS_NATIVE=ON -DNEXUS_WERROR=OFF
cmake --build "$BUILD"
ctest --test-dir "$BUILD" --output-on-failure

# Taken before results/machine.txt is rewritten; results/ itself is what this script changes.
commit="$(git rev-parse --short HEAD)$(git diff --quiet HEAD -- . ':!results' || echo ' (dirty)')"
{
  echo "date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "commit: $commit"
  echo "os: $(uname -srm)"
  if [[ "$(uname -s)" == Linux ]]; then
    echo "cpu: $(lscpu | sed -n 's/^Model name:[[:space:]]*//p')"
    echo "logical cpus: $(nproc)"
    echo "memory: $(awk '/MemTotal/ {printf "%.1f GiB", $2/1048576}' /proc/meminfo)"
    echo "governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo n/a)"
    echo "perf: $(perf --version 2>/dev/null || echo unavailable)"
  else
    echo "cpu: $(sysctl -n machdep.cpu.brand_string)"
    echo "logical cpus: $(sysctl -n hw.ncpu)"
    echo "memory: $(( $(sysctl -n hw.memsize) / 1073741824 )) GiB"
    echo "cache line: $(sysctl -n hw.cachelinesize) bytes"
    echo "perf: unavailable"
  fi
  echo "compiler: $(sed -n 's/^CMAKE_CXX_COMPILER:[A-Z]*=//p' "$BUILD/CMakeCache.txt") ($("$(sed -n 's/^CMAKE_CXX_COMPILER:[A-Z]*=//p' "$BUILD/CMakeCache.txt")" --version | head -1))"
  echo "flags: release preset, -O3 -march=native"
} > results/machine.txt
cat results/machine.txt

"$BUILD/fuzz_differential" --seeds 200 --commands 100000 | tee results/fuzz.txt
CXX="$(sed -n 's/^CMAKE_CXX_COMPILER:[A-Z]*=//p' "$BUILD/CMakeCache.txt")" \
  scripts/mutation_check.sh | tee results/mutation.txt

"$BUILD/bench_micro" --benchmark_repetitions="$REPS" --benchmark_report_aggregates_only=true \
  --benchmark_out=results/micro.json --benchmark_out_format=json

rm -f results/replay_nexus.txt results/replay_naive.txt
for _ in $(seq "$REPS"); do  # interleaved, so load drift hits both engines alike
  for engine in nexus naive; do
    "$BUILD/bench_e2e" --replay "$engine" | tee -a "results/replay_$engine.txt"
  done
done

"$BUILD/bench_e2e" --reps "$REPS" --out results

BIN="$BUILD/bench_e2e" scripts/perf_compare.sh || true

python3 scripts/make_results_table.py
