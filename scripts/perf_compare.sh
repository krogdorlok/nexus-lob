#!/usr/bin/env bash
# Hardware counters for both engines on the identical single-threaded replay workload
# (bench_e2e --replay: same seeded 10M-command stream, no queues or spinning threads), plus a
# generation-only baseline (--replay none) that make_results_table.py subtracts, so the table
# reflects engine work alone. Writes results/perf_{nexus,naive,baseline}.txt (perf CSV) only
# after every run succeeds. Linux with perf only.
set -euo pipefail
cd "$(dirname "$0")/.."
BIN=${BIN:-build/bench/bench_e2e}
EVENTS=cycles,instructions,L1-dcache-loads,L1-dcache-load-misses,LLC-loads,LLC-load-misses,branch-misses

rm -f results/perf_nexus.txt results/perf_naive.txt results/perf_baseline.txt
if [[ "$(uname -s)" != Linux ]] || ! command -v perf >/dev/null; then
  echo "perf_compare: needs Linux with perf; hardware counters stay TBD" >&2
  exit 1
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
for run in nexus naive none; do
  perf stat -x, -r 5 -e "$EVENTS" -o "$tmp/$run.txt" -- "$BIN" --replay "$run"
done
mv "$tmp/nexus.txt" results/perf_nexus.txt
mv "$tmp/naive.txt" results/perf_naive.txt
mv "$tmp/none.txt" results/perf_baseline.txt
echo "wrote results/perf_{nexus,naive,baseline}.txt"
