#!/usr/bin/env bash
# Regenerates everything in results/ from a clean checkout, then the README results table.
# Intended for a quiet Linux x86_64 box with perf (for example an AWS c7i instance); on other
# platforms it runs everything except hardware counters, which stay TBD.
#   REPS=5 scripts/bench_linux.sh
# REQUIRE_PERF=1 makes a missing or failing perf an error instead of a TBD.
# QUICK=1 shrinks every step to exercise the plumbing (the CI smoke test); its numbers are not
# results and its machine.txt says so.
set -euo pipefail
cd "$(dirname "$0")/.."
REPS=${REPS:-5}
stage() { echo "== $(date -u +%H:%M:%S) $*"; }  # timestamps in the log show where time goes
if [[ -n ${QUICK:-} ]]; then
  REPS=1
  FUZZ=(--seeds 4 --commands 5000)
  MICRO=(--benchmark_min_time=0.01s)
  SIZE=(--commands 200000)
  E2E=(--commands 200000 --warmup 20000)
else
  FUZZ=(--seeds 200 --commands 100000)
  MICRO=()
  SIZE=()
  E2E=()
fi
BUILD=build/bench
mkdir -p results
# Remove this script's previous outputs, so a skipped step can never leave stale numbers behind.
rm -f results/machine.txt results/fuzz.txt results/mutation.txt results/micro.json \
  results/replay_*.txt results/e2e_*.json results/e2e_skipped.txt results/perf_*.txt

stage build
cmake --preset release -B "$BUILD" -DNEXUS_NATIVE=ON -DNEXUS_WERROR=OFF
cmake --build "$BUILD"
stage tests
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
  [[ -z ${QUICK:-} ]] || echo "mode: QUICK smoke test, these numbers are not results"
} > results/machine.txt
cat results/machine.txt

stage fuzz and mutation check
"$BUILD/fuzz_differential" "${FUZZ[@]}" | tee results/fuzz.txt
CXX="$(sed -n 's/^CMAKE_CXX_COMPILER:[A-Z]*=//p' "$BUILD/CMakeCache.txt")" \
  scripts/mutation_check.sh | tee results/mutation.txt

stage micro benchmarks
"$BUILD/bench_micro" --benchmark_repetitions="$REPS" --benchmark_report_aggregates_only=true \
  --benchmark_out=results/micro.json --benchmark_out_format=json "${MICRO[@]}"

stage replay
for _ in $(seq "$REPS"); do  # interleaved, so load drift hits both engines alike
  for engine in nexus naive; do
    "$BUILD/bench_e2e" --replay "$engine" "${SIZE[@]}" | tee -a "results/replay_$engine.txt"
  done
done

# The pipeline spins three threads, so on fewer than four logical CPUs they would share cores
# and the numbers would measure the scheduler, not the engine.
cpus=$(getconf _NPROCESSORS_ONLN)
stage pipeline
if ((cpus >= 4)); then
  "$BUILD/bench_e2e" --reps "$REPS" --out results "${E2E[@]}"
else
  echo "skipped: this machine has $cpus logical CPUs; the pipeline needs 3 dedicated cores" |
    tee results/e2e_skipped.txt
fi

stage hardware counters
if ! BIN="$BUILD/bench_e2e" scripts/perf_compare.sh; then
  [[ -z ${REQUIRE_PERF:-} ]] || exit 1
fi

stage results table
python3 scripts/make_results_table.py
