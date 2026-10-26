# Resume bullets

Every measured number below is copied from the cited file in `results/` (Apple M2, see
`results/machine.txt`; Intel Xeon on AWS, see `results/aws-c7i-flex.large/machine.txt`);
stream and test sizes are parameters of the code. Rerun
`scripts/bench_linux.sh` and update these if the files change.

* **Built a C++20 limit order book whose matching core runs 3.73x faster than a
  `std::map`/`std::list` baseline.** Map-based books allocate a node per order and chase
  pointers on every lookup, so I replaced them with cache-line-sized orders in an intrusive
  FIFO, a flat price ladder whose best price comes from a two-level occupancy bitmap, an
  open-addressing id index with backward-shift deletion, and a pre-faulted object pool with
  zero heap allocation on the hot path (enforced by a counting-allocator test). Single-threaded
  replay of the same 10M-command stream: 44.87M vs 12.03M commands/s; a cancel costs 9.0 vs
  61.0 CPU ns. The lead held on an Intel Xeon under Linux, run on AWS EC2 with results
  archived to S3: 24.08M vs 9.17M commands/s (2.63x), cancel 12.2 vs 81.5 CPU ns.
  *(results/replay_nexus.txt, results/replay_naive.txt, results/micro.json,
  results/aws-c7i-flex.large/)*

* **Engineered a lock-free three-thread pipeline that sustains 15.87M commands/s end to end,
  2.04x the baseline engine in the same pipeline.** Producer, engine (pinned on Linux) and
  publisher threads communicate through SPSC rings with acquire/release ordering, cached peer
  indices and cache-line padding sized for Apple Silicon's 128-byte lines; latency is
  recorded into an allocation-free histogram. Investigating a stalled benchmark showed the
  ARM `cntvct_el0` counter stepping backwards on macOS, so I switched to the OS monotonic
  clock and added tests (cross-thread ping-pong, sleep and calibration) that fail on a broken
  timer. Paced at 1M commands/s, median first-response latency (to the command's
  acknowledgement) was 125 ns across two queue hops.
  *(results/e2e_nexus.json, results/e2e_naive.json)*

* **Proved correctness with an independently written reference engine and a differential
  fuzzer.** Hand-written tests miss interaction bugs between cancels, partial fills and
  priority-changing modifies, so the fuzzer replays seeded streams through both engines and
  requires byte-identical event sequences after every command, and it fails unless every
  reject reason actually occurs. 200 seeds x 100,000 commands produced 7,420,854 fills and
  identical output, in CI on GCC 13 and Clang 17 alongside ASan, UBSan and TSan builds.
  *(results/fuzz.txt)*
