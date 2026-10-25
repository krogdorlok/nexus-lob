#!/usr/bin/env python3
"""Compares two results/ directories metric by metric and prints a markdown table, marking
moves larger than 10%. Used for the reproducibility pass:
    python3 scripts/compare_results.py BASELINE_DIR RERUN_DIR > results/reproducibility.md"""
import json
import re
import statistics
import sys
from pathlib import Path


def e2e(d, engine):
    p = d / f"e2e_{engine}.json"
    if not p.exists():  # e.g. a machine with too few cores for the pipeline
        return
    j = json.loads(p.read_text())
    yield f"e2e {engine} throughput (cmds/s)", j["throughput_cmds_per_s"]["flood"]["median"]
    for k in ("p50", "p90", "p99", "p99_9", "max"):
        yield f"e2e {engine} latency {k} (ns)", j["latency_ns"][k]["median"]


def replay(d, engine):
    p = d / f"replay_{engine}.txt"
    if not p.exists():
        return
    text = p.read_text()
    yield f"replay {engine} (cmds/s)", statistics.median(
        float(x) for x in re.findall(r"([\d.]+) commands/s", text))


def micro(d):
    for b in json.loads((d / "micro.json").read_text())["benchmarks"]:
        if b.get("aggregate_name") == "median":
            yield f"micro {b['run_name']} (CPU ns/op)", 1e9 / b["items_per_second"]


def perf(d, run):
    p = d / f"perf_{run}.txt"
    if not p.exists():
        return
    for line in p.read_text().splitlines():
        parts = line.split(",")
        if len(parts) > 2 and parts[0].replace(".", "").isdigit():
            yield f"perf {run} {parts[2]}", float(parts[0])


def metrics(d):
    out = {}
    for engine in ("nexus", "naive"):
        out.update(e2e(d, engine))
        out.update(replay(d, engine))
    out.update(micro(d))
    for run in ("nexus", "naive", "baseline"):
        out.update(perf(d, run))
    return out


def commit(d):
    m = re.search(r"^commit: (.*)$", (d / "machine.txt").read_text(), re.M)
    return m.group(1) if m else "unknown"


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    a, b = Path(sys.argv[1]), Path(sys.argv[2])
    ma, mb = metrics(a), metrics(b)
    print(f"Baseline run: commit {commit(a)}. Rerun: commit {commit(b)}. Medians; "
          "**bold** rows moved more than 10%.\n")
    print("| Metric | Baseline | Rerun | Change |")
    print("|---|---|---|---|")
    for name, va in ma.items():
        if name not in mb:
            continue
        vb = mb[name]
        change = (vb - va) / va * 100 if va else 0.0
        cells = [name, f"{va:,.1f}", f"{vb:,.1f}", f"{change:+.1f}%"]
        if abs(change) > 10:
            cells = [f"**{c}**" for c in cells]
        print("| " + " | ".join(cells) + " |")


if __name__ == "__main__":
    main()
