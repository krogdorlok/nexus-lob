#!/usr/bin/env bash
# Runs the benchmark suite on this Linux host and copies the fresh results plus a machine
# manifest to TARGET: an s3:// URI (on EC2, where userdata.sh writes the DONE marker after the
# log) or a local directory (the CI smoke test, which gets DONE here). Meant for a disposable
# checkout: it empties results/ first, so only files from this run are copied.
#   scripts/aws/remote_bench.sh TARGET
set -euo pipefail
cd "$(dirname "$0")/../.."
target=${1:?usage: remote_bench.sh s3://bucket/prefix | /local/dir}

if [[ $EUID == 0 ]]; then
  sysctl -w kernel.perf_event_paranoid=1  # let perf read hardware counters
fi
rm -rf results && mkdir results  # nothing committed or stale can ride along with this run
rc=0
scripts/bench_linux.sh || rc=$?  # on failure, still upload what exists for debugging; only the
                                 # DONE marker (written after success) marks a run complete

{
  echo "== lscpu"
  lscpu
  echo "== uname"
  uname -a
  echo "== cpu flags"
  grep -m1 '^flags' /proc/cpuinfo
  echo "== perf hardware events"
  perf list hw cache 2>&1 || echo "perf unavailable"
} > results/manifest.txt

if [[ $target == s3://* ]]; then
  aws s3 cp --recursive results "$target/"
else
  mkdir -p "$target"
  cp -R results/. "$target/"
  ((rc != 0)) || touch "$target/DONE"
fi
echo "results copied to $target"
exit "$rc"
