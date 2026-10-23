#!/usr/bin/env python3
"""Builds the README results section from files in results/ and injects it between the
<!-- RESULTS --> and <!-- /RESULTS --> markers. Every number comes from a results file;
anything missing is written as TBD."""
import json
import re
import statistics
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RES = ROOT / "results"
README = ROOT / "README.md"
START, END = "<!-- RESULTS -->", "<!-- /RESULTS -->"
PERF_EVENTS = ["cycles", "instructions", "L1-dcache-loads", "L1-dcache-load-misses",
               "LLC-loads", "LLC-load-misses", "branch-misses"]


def load_json(name):
    p = RES / name
    return json.loads(p.read_text()) if p.exists() else None


def fmt_rate(v):
    return f"{v / 1e6:.2f}M/s"


def ratio(nexus, naive, higher_is_better):
    if not nexus or not naive:
        return "TBD"
    r = nexus / naive if higher_is_better else naive / nexus
    return f"{r:.2f}x"


def ratio_with_spread(a, b, higher_is_better):
    """Median ratio, or no ratio at all when the min-to-max ranges overlap."""
    overlap = a["min"] <= b["max"] and b["min"] <= a["max"]
    return "within noise (ranges overlap)" if overlap else ratio(a["median"], b["median"],
                                                                 higher_is_better)


def e2e_section(out):
    nx, nv = load_json("e2e_nexus.json"), load_json("e2e_naive.json")
    out.append("### End to end (pipeline, `results/e2e_<engine>.json`)\n")
    if not nx or not nv:
        out.append("TBD (not yet measured)\n")
        return
    out.append(f"{nx['reps']} repetitions of {nx['commands']:,} measured commands after "
               f"{nx['warmup']:,} warmup. Median, with min to max across repetitions in "
               "brackets. Throughput comes from flood runs; latency comes from runs paced at "
               f"{nx['paced_rate_cmds_per_s']:,.0f} commands/s. Timer resolution "
               f"{nx['ns_per_tick']:.2f} ns.\n")
    out.append("| Metric | NexusLOB | Naive | NexusLOB advantage (median ratio) |")
    out.append("|---|---|---|---|")
    a, b = nx["throughput_cmds_per_s"]["flood"], nv["throughput_cmds_per_s"]["flood"]
    out.append(f"| Throughput | {fmt_rate(a['median'])} [{fmt_rate(a['min'])} to "
               f"{fmt_rate(a['max'])}] | {fmt_rate(b['median'])} [{fmt_rate(b['min'])} to "
               f"{fmt_rate(b['max'])}] | {ratio_with_spread(a, b, True)} |")
    for key, label in [("p50", "p50"), ("p90", "p90"), ("p99", "p99"), ("p99_9", "p99.9"),
                       ("max", "max")]:
        a, b = nx["latency_ns"][key], nv["latency_ns"][key]
        out.append(f"| Latency {label} (ns) | {a['median']:,.0f} [{a['min']:,.0f} to "
                   f"{a['max']:,.0f}] | {b['median']:,.0f} [{b['min']:,.0f} to "
                   f"{b['max']:,.0f}] | {ratio_with_spread(a, b, False)} |")
    out.append("")


def replay_rates(engine):
    p = RES / f"replay_{engine}.txt"
    if not p.exists():
        return None
    return [float(m) for m in re.findall(r"([\d.]+) commands/s", p.read_text())]


def replay_section(out):
    nx, nv = replay_rates("nexus"), replay_rates("naive")
    out.append("### Engine only (single-threaded replay, `results/replay_<engine>.txt`)\n")
    if not nx or not nv:
        out.append("TBD (not yet measured)\n")
        return
    a, b = statistics.median(nx), statistics.median(nv)
    out.append(f"Same seeded 10M-command stream, no queues. Median of {len(nx)} runs, "
               "min to max in brackets.\n")
    out.append("| Engine | Throughput |")
    out.append("|---|---|")
    out.append(f"| NexusLOB | {fmt_rate(a)} [{fmt_rate(min(nx))} to {fmt_rate(max(nx))}] |")
    out.append(f"| Naive | {fmt_rate(b)} [{fmt_rate(min(nv))} to {fmt_rate(max(nv))}] |")
    spread = {"nexus": {"median": a, "min": min(nx), "max": max(nx)},
              "naive": {"median": b, "min": min(nv), "max": max(nv)}}
    out.append(f"| NexusLOB advantage | {ratio_with_spread(spread['nexus'], spread['naive'], True)} |")
    out.append("")


def micro_section(out):
    data = load_json("micro.json")
    out.append("### Micro benchmarks (Google Benchmark, `results/micro.json`)\n")
    if not data:
        out.append("TBD (not yet measured)\n")
        return
    rows = {}
    for b in data["benchmarks"]:
        if b.get("aggregate_name") != "median":
            continue
        op, engine, *arg = b["run_name"].split("/")
        name = op + (f" ({arg[0]} level{'' if arg[0] == '1' else 's'})" if arg else "")
        rows.setdefault(name, {})[engine] = 1e9 / b["items_per_second"]
    reps = data["benchmarks"][0].get("repetitions", "?")
    out.append(f"Median of {reps} repetitions, CPU ns per operation (one sweep = one operation). "
               "Each batch runs on a fresh book.\n")
    out.append("| Operation | NexusLOB (CPU ns/op) | Naive (CPU ns/op) | NexusLOB advantage |")
    out.append("|---|---|---|---|")
    for name, r in rows.items():
        a, b = r.get("nexus"), r.get("naive")
        out.append(f"| {name} | {a:,.1f} | {b:,.1f} | {ratio(a, b, False)} |")
    out.append("")


def parse_perf(engine):
    p = RES / f"perf_{engine}.txt"
    if not p.exists():
        return None
    vals = {}
    for line in p.read_text().splitlines():
        parts = line.split(",")
        if len(parts) > 2 and parts[2] in PERF_EVENTS:
            try:
                vals[parts[2]] = float(parts[0])
            except ValueError:
                vals[parts[2]] = None  # <not supported> / <not counted>
    return vals


def perf_section(out):
    nx, nv, base = parse_perf("nexus"), parse_perf("naive"), parse_perf("baseline")
    if nx and nv and base:  # remove the cost of generating the command stream
        for d in (nx, nv):
            for e in PERF_EVENTS:
                d[e] = None if d.get(e) is None or base.get(e) is None else d[e] - base[e]
    out.append("### Hardware counters (`perf stat`, `results/perf_<engine>.txt`)\n")
    if not nx or not nv or not base:
        out.append("perf counters require Linux x86_64; run `scripts/perf_compare.sh` there.\n")
        out.append("| Event | NexusLOB | Naive | Reduction |")
        out.append("|---|---|---|---|")
        for e in PERF_EVENTS:
            out.append(f"| {e} | TBD (requires Linux x86_64) | TBD | TBD |")
        out.append("")
        return
    out.append("Single-threaded replay of the same 10M-command stream, mean of 5 perf runs each, "
               "minus a generation-only baseline run (`results/perf_baseline.txt`), so the "
               "counts are engine work only.\n")
    out.append("| Event | NexusLOB | Naive | Reduction |")
    out.append("|---|---|---|---|")
    for e in PERF_EVENTS:
        a, b = nx.get(e), nv.get(e)
        if a is None or b is None:
            out.append(f"| {e} | not supported | not supported | TBD |")
        else:
            out.append(f"| {e} | {a:,.0f} | {b:,.0f} | {ratio(a, b, False)} |")
    if all(nx.get(k) and nv.get(k) for k in ("cycles", "instructions")):
        out.append(f"| IPC | {nx['instructions'] / nx['cycles']:.2f} | "
                   f"{nv['instructions'] / nv['cycles']:.2f} | |")
    out.append("")


def main():
    out = []
    machine = RES / "machine.txt"
    out.append("Machine (`results/machine.txt`):\n")
    out.append("```\n" + (machine.read_text().strip() if machine.exists() else "TBD") + "\n```\n")
    e2e_section(out)
    replay_section(out)
    micro_section(out)
    perf_section(out)
    table = "\n".join(out).rstrip() + "\n"
    text = README.read_text()
    if START not in text or END not in text:
        raise SystemExit(f"README.md is missing {START} / {END} markers")
    head, rest = text.split(START, 1)
    _, tail = rest.split(END, 1)
    README.write_text(f"{head}{START}\n{table}{END}{tail}")
    print("README results table updated")


if __name__ == "__main__":
    main()
