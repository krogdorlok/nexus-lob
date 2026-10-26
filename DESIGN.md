# NexusLOB design notes

Each section names the decision, the alternative that was rejected, and why. Measured
performance numbers live only in `results/`, the README table generated from it, and
`RESUME.md`, which cites its files; this document explains mechanisms, not magnitudes.

## Data flow

```
producer thread           engine thread (pinned on Linux)        publisher thread
FlowGenerator  --SPSC-->  MatchingEngine::process(cmd, sink) --SPSC-->  latency histogram
 stamp tsc_begin()         OrderBook: 2 x PriceLadder                  stamp tsc_end() on a
 just before push                     OrderMap (id -> Order*)          command's first event
                                      ObjectPool<Order>
```

The engine is single-threaded by design: one thread owns the book, so matching needs no locks
and no atomics. Concurrency exists only at the two queue boundaries.

## Semantics first, then speed

The full matching contract (check order for rejects, what each event carries, when Modify
keeps priority) is written once, at the top of `include/nexus/core/events.hpp`. Two engines
implement it: `MatchingEngine` (fast) and `reference::NaiveEngine` (`std::map` +
`std::list`, written independently from the contract alone). `fuzz_differential` drives both
with the same seeded stream and requires byte-identical events, identical top of book and
identical order counts after every command. `Event` has no padding bytes
(`has_unique_object_representations`), so `memcmp` comparison is well defined.

A fuzzer that never fails proves little, so two guards keep it honest:

* **Coverage gate.** The run fails unless every event type and every `RejectReason` actually
  occurred. This gate caught a real gap: the first configuration never hit `PoolExhausted`
  in the default run, so the tiny-pool configuration was shrunk to 16 slots.
* **Mutation check.** `scripts/mutation_check.sh` (run in CI, output in
  `results/mutation.txt`) injects realistic bugs one at a time (modify keeps priority on a
  quantity increase, FOK off by one, missing market remainder cancel, sweep stopping early)
  and requires the fuzzer to fail on each, printing the seed and command index, while the
  unmutated engine must pass.

## Order: 64 bytes, one x86 cache line

`Order` is `alignas(64)` and exactly 64 bytes, enforced by `static_assert`. Everything the
matcher touches for one resting order (id, price, quantities, side, links) fits in one 64-byte
line, so on x86 walking a level costs one line per order. Apple M-series cores use 128-byte
lines; there two orders share a line, which is harmless because only the engine thread
touches orders.

The layout is the one specified for the project. Matching reads `id`, `price`, `qty`,
`filled`, `side` and the links; `ts`, `type` and `tif` are written when an order rests but are
not read by matching (a resting order is always a GTC limit). They are kept for auditability
and to fill the line, which costs nothing because the size is fixed at 64 bytes anyway.

## Price level: intrusive doubly linked FIFO

**Chosen:** `next`/`prev` pointers inside `Order`; `PriceLevel` holds head, tail, total
volume and count.

**Rejected: `std::list<Order>`.** Each node is a separate heap allocation, so every add and
cancel goes to the allocator, and nodes scatter across memory.

**Rejected: `std::vector<Order*>` per level.** Appends are cheap, but cancelling from the
middle is O(n) erase (or tombstones that must be compacted), and growth reallocates on the hot
path.

With intrusive links, cancel by id is O(1): the id map yields the `Order*`, and `unlink` fixes
two neighbours. The level keeps `volume == sum(remaining)` as an invariant, maintained by
`reduce()` on every partial fill or in-place shrink, so top-of-book volume and FOK checks
never walk orders.

## Price ladder: flat array plus a two-level occupancy bitmap

**Chosen:** one `PriceLevel` per tick in a fixed band `[min, max]`, indexed by
`price - min`. Bit `i` of `l0` says level `i` is non-empty; bit `w` of `l1` says `l0` word
`w` is non-zero. The best bid is the highest set bit, found with `std::countl_zero` on at
most a few words (and a scan of at most `band / 4096` `l1` words); the best ask is the lowest,
via `std::countr_zero`. The next level during a sweep is the next set bit beyond the current
one.

**Rejected: `std::map<Price, Level>`.** O(log n) pointer-chasing per lookup, and a heap
allocation each time a new price appears.

**Rejected: a cached best price.** Caches go stale when the best level empties through a
cancel, a fill, or a modify; every one of those paths must remember to update it. Deriving
the best price from the bitmap every time makes stale-best bugs impossible by construction,
at the cost of a few bit operations.

The trade-off is a bounded band. Orders outside it are rejected with `PriceOutOfBand`, which
is how real venues behave anyway (price collars), and the band is a constructor argument.

## Id index: open addressing with backward-shift deletion

**Chosen:** `OrderMap`, a preallocated power-of-two table of `{id, Order*}` slots, linear
probing, Fibonacci hashing (top bits of `id * 2^64/phi`), load factor at most 0.5, and
backward-shift deletion.

**Rejected: `std::unordered_map`.** Node-based: one allocation per insert, and a pointer
chase per lookup.

**Rejected: tombstones.** Deletes leave markers that lengthen probe sequences until a rebuild,
and a rebuild is a latency spike. Backward shift moves later cluster members back into the
hole (unless that would move them before their home slot), so the table stays as compact as
if the deleted key had never been inserted. Tests cover clusters that wrap around the table
end, including a key that must be skipped because it sits at its own home.

**Sizing is a latency decision.** The index is sized for the book's maximum resting orders,
not "as large as possible". Sequential ids hash uniformly over the whole table, so a table
much larger than the cache makes every lookup a likely cache miss. That is why the pipeline
sizes the book for a realistic single-instrument working set (2^17 orders) rather than for
millions. (An informal run during development pointed the same way; it is not recorded in
`results/`, so no figure is claimed.)

## Memory: preallocated pool, pre-faulted

`ObjectPool<Order, N>` allocates all storage in its constructor and `memset`s it, so every
page is faulted in before trading starts. The free list is a LIFO stack of pointers: the
order slot freed most recently is reused first, and it is likely still in cache.

**Rejected: `new`/`delete` per order.** The allocator takes locks or thread caches, has
unbounded worst-case latency, and the first touch of a fresh page is a page fault in the
middle of matching.

**Proof, not promise.** `test_no_alloc` replaces all replaceable global `operator new` and
`operator delete` overloads (plain, array, aligned, nothrow, aligned nothrow, sized; on glibc
also `malloc`/`calloc`/`realloc`) with counting versions, runs 1M mixed commands
after warmup, and asserts zero allocations. A second counted run covers the pipeline's whole
per-command path single-threaded: timestamp, inbound SPSC push and pop, `process()`, every
event through the outbound SPSC, and the latency histogram. A deterministic run covering
every reject reason and every market-remainder path is counted the same way. A positive
control proves the counter fires.

## SPSC ring: acquire/release, cached indices, padding

The queue between threads is a classic Lamport ring with monotonically increasing 64-bit
indices masked on access, so full (`head - tail == Cap`) and empty (`head == tail`) are
unambiguous and unsigned wraparound is harmless (tested from just below 2^32 and just below
2^64, and during a 10M-item two-thread run). "Lock-free" is a property of the target rather
than of the C++ standard, so a `static_assert` on `std::atomic<size_t>::is_always_lock_free`
refuses to build where the index atomics would fall back to a lock.

**Memory ordering.** The producer writes the slot, then publishes `head` with
`memory_order_release`; the consumer reads `head` with `acquire` before reading the slot.
That pair is exactly the happens-before edge needed. **Rejected: `seq_cst`.** It adds a full
fence (on x86, the store becomes `xchg`/`mfence`) to enforce a single total order across all
atomics, which a two-party handoff never needs.

**Cached indices.** Each side keeps a private copy of the other side's index and re-reads the
shared atomic only when the copy says full or empty. In steady state each push or pop touches
only its own cache line.

**False sharing.** `head` (written by the producer) and `tail` (written by the consumer) live
in separate cache-line-aligned blocks, and the buffer starts on its own line. Without this,
the two cores would bounce one line on every operation. The padding size is the platform's
line size: 128 bytes on Apple Silicon, 64 elsewhere.

## Timing

**Latency definition: first response.** For one command: `tsc_end()` taken by the publisher
immediately after it dequeues that command's first event, minus the `tsc_begin()` stamp the
producer wrote into the command just before pushing it to the inbound queue. The first event
is the acknowledgement (`Accepted`, which the engine emits after validation and before
matching, or `Rejected`, `Cancelled`, `Modified`), so this is time to acknowledgement over
two queue hops, not time until a trading order's last fill is published. That is the project
specification's definition, and it is the number a trading client waits on first; matching
cost is measured separately by throughput, replay and the micro benchmarks. If the inbound
queue is full, the wait counts, because the stamp is taken once before the first push
attempt.

**Rejected: stamping the last event.** It would fold the fill count of each order into the
latency, so a sweep across many levels and a passive add would no longer be comparable, and
the engine would have to buffer each command's events to know which one is last.

**Two runs, two questions.** Flood runs (the producer pushes as fast as the queues accept)
measure throughput; their latency is dominated by queueing and is not reported. Paced runs
(pushes on a fixed open-loop schedule) measure latency at a sustainable rate.

**TSC.** On x86 the stamp is `lfence; rdtsc; lfence` at the start and `rdtscp; lfence` at the
end, which keeps out-of-order execution from moving work across the stamp. Ticks are
converted with a ratio calibrated against `steady_clock` over about 200 ms at startup; the
nominal CPU frequency is never assumed, because the TSC frequency and the core frequency
differ. `bench_e2e` refuses to run on x86 Linux if `/proc/cpuinfo` lacks `constant_tsc` and
`nonstop_tsc`. Those flags establish an invariant TSC; they do not prove the cores are
synchronized (the kernel checks that at boot and marks the TSC unstable otherwise), and
multi-socket machines need extra care, so run on a single socket.

**Apple Silicon.** The obvious ARM choice, a user-space `cntvct_el0` read, was observed going
backwards across a short sleep on macOS (the thread can resume on another core), so it is not
usable there. macOS uses `mach_absolute_time()`, the OS-maintained monotonic counter. Its
tick is coarse (the calibrated value is recorded as `ns_per_tick` in
`results/e2e_<engine>.json`), so each latency sample on this platform is a whole number of
ticks (the histogram itself is exact below 128 ticks and within 1.6% above).
`test_pipeline` asserts that the timer advances across a sleep, calibrates to a plausible
frequency, and never steps backwards in a 100k-round ping-pong between two threads, so a
broken timer fails a test instead of silently producing numbers.

## Pinning and scheduling

On Linux the engine thread is pinned with `pthread_setaffinity_np` (CPU 2 by default; the
pipeline warns and continues unpinned if the call fails), so it never migrates and keeps its
L1/L2 warm; the producer and publisher are left to the scheduler. For lowest jitter,
also isolate that core from the scheduler (`isolcpus`/`nohz_full`) and pin interrupts
elsewhere. macOS has no affinity API, so the pipeline warns once and requests
`QOS_CLASS_USER_INTERACTIVE`, which steers threads toward performance cores. All three threads
spin rather than block: a blocked thread needs the scheduler to wake it, which adds a delay
the program does not control.

## Measurement infrastructure: EC2 and S3 on the free tier

The laptop cannot give Linux x86 numbers or hardware counters, so `scripts/aws/` runs the
benchmark suite on an EC2 instance and archives every run in S3.

**EC2 as a disposable, self-destructing benchmark host.** Each run launches a fresh Ubuntu
instance from a pushed commit, so nothing on the host carries over between runs. It is
launched with shutdown behaviour "terminate", a hard shutdown deadline is scheduled at boot,
and the boot script's exit trap uploads the log and shuts down on success or failure. The
launcher terminates the instance on any local error or interrupt, and a client token makes a
retried launch unable to start a second instance. **Rejected: GitHub Actions runners.**
Shared virtual machines with no hardware counters and unknown neighbours. **Rejected: Spot.**
It can reclaim the machine mid-run, which breaks comparability. **Rejected: a long-lived
instance.** State and installed packages drift between runs, and it bills while idle.

**S3 as the run history.** Runs land under `runs/<instance type>/<commit>/<time>/`, and the
launcher compares each new run with the previous one of the same instance type, so a
performance regression shows up as a flagged row. Versioning means a run can never silently
overwrite another, and every run records its OS image and kernel so a cross-image comparison
is flagged rather than mistaken for a code change. **Rejected: committing every run to git.**
The repository should hold the curated results the README cites, not every experiment.

**Least privilege.** The operator uses `aws login` (short-lived credentials, no stored keys).
The instance has no SSH key, its security group allows no inbound traffic and only TCP 80
and 443 outbound, metadata access requires IMDSv2 tokens, and its role can do exactly one
thing: write objects under `runs/` in the results bucket. Every script is a dry run unless
given `--apply`.

**Counters are recorded or explained, never invented.** On `c7i-flex.large`, `perf` reports
that the instance exposes no hardware performance counters at all, which matches AWS's
documentation for small instance sizes (`results/aws-c7i-flex.large/perf_unavailable.txt`).
The first attempt treated that as a failure and lost its other measurements; the scripts now
treat it as an outcome: perf's message is written to `perf_unavailable.txt` instead of
failing the run or printing a number. Counters need a whole-socket or metal instance, which
is outside the free tier.

## Limitations

* One instrument per engine; no self-trade prevention; no iceberg, stop or pegged orders.
* The price band is fixed at construction.
* Results in `results/` come from an Apple M2 laptop running a desktop session, without
  pinning or core isolation; those in `results/aws-c7i-flex.large/` from a free-tier EC2 VM
  with one physical core and two hyper-threads. Hardware counters need Linux on a machine that
  exposes them (bare metal or a whole-socket instance); the free-tier EC2 instance does not.
* Not a hard real-time system: no `mlockall`, huge pages, real-time scheduling, NUMA policy
  or interrupt routing, and a full outbound queue makes the engine spin until it drains.
* The flow generator is synthetic, not a replay of real market data.

## Ten hard interview questions

1. **Why is cancel O(1) here, and what would make it O(n)?** The id map yields the `Order*`
   and the level is a doubly linked intrusive list, so unlink touches two neighbours. With a
   `std::vector` per level, cancel would need a search and an erase that shifts the tail.

2. **How do you find the best price after the best level empties, without a stale cache?**
   The best price is derived from the occupancy bitmap every time: mask the current `l0` word,
   `countl_zero`/`countr_zero`, then fall back to the `l1` summary. Nothing is cached, so no
   code path can forget to update it.

3. **Why acquire/release and not relaxed or seq_cst in the SPSC queue?** Relaxed would let the
   consumer see the new `head` before the slot write, and read garbage. `seq_cst` would add a
   full fence for a global order that a two-thread handoff never needs. Release on publish plus
   acquire on observe is the minimum that orders the slot write before the read.

4. **What is false sharing and where does this code prevent it?** Two cores writing different
   variables on one cache line invalidate each other's copy on every write. The SPSC
   producer index, consumer index and buffer each start on their own line, with 128-byte
   alignment on Apple Silicon where lines are 128 bytes.

5. **How do you prove the hot path never allocates?** Replace every global allocation
   function with a counting version, run a million mixed commands after warmup (plus a
   deterministic pass over every reject and remainder path), and assert zero. A positive
   control shows the counter works. Under sanitizers the test is skipped, because their
   runtimes own the allocator.

6. **Why calibrate the TSC instead of using the CPU frequency?** The TSC ticks at a fixed
   reference rate that differs from the current core frequency under turbo or power saving.
   Calibrating against `steady_clock` measures the real ratio. Cross-core use also requires
   an invariant TSC (`constant_tsc`, `nonstop_tsc`, which the benchmark checks) that the
   kernel has found synchronized across cores.

7. **Your latency histogram never allocates; how accurate are its percentiles?** It is
   log-linear: values below 128 ticks are exact; above that each power of two has 64 buckets,
   so a reported percentile is at most about 1.6% above the true value, and never above the
   true maximum.

8. **How do you know the fast engine is correct?** An independently written naive engine
   implements the same contract, and a differential fuzzer compares every event byte for byte
   over 200 seeds of 100k commands (run in CI). It must also hit every reject reason, and a
   scripted mutation check shows it catches injected bugs.

9. **What happens when the pool is exhausted?** A Limit GTC order that might rest is rejected
   with `PoolExhausted` before it touches the book. Orders that never rest (IOC, FOK, market)
   are still accepted, and a modify reuses its own slot, so it cannot fail for lack of space.

10. **Why does a modify that only shrinks quantity keep priority, while a price change loses
    it?** Shrinking cannot disadvantage anyone behind you in the queue, so exchanges allow it
    in place. A price change or a size increase is a new economic commitment, so it is a
    cancel plus a new order: the order leaves the book first, so it can never match itself,
    then re-enters as an aggressor and rests at the tail.
