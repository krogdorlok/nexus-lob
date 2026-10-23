Baseline run: commit 3cf5099 (dirty). Rerun: commit 1a1e548. Medians; **bold** rows moved more than 10%.

| Metric | Baseline | Rerun | Change |
|---|---|---|---|
| e2e nexus throughput (cmds/s) | 16,004,232.5 | 15,865,258.4 | -0.9% |
| **e2e nexus latency p50 (ns)** | **166.7** | **125.0** | **-25.0%** |
| **e2e nexus latency p90 (ns)** | **208.3** | **250.0** | **+20.0%** |
| **e2e nexus latency p99 (ns)** | **311,958.4** | **215,958.4** | **-30.8%** |
| **e2e nexus latency p99_9 (ns)** | **8,447,960.1** | **12,970,627.7** | **+53.5%** |
| **e2e nexus latency max (ns)** | **33,921,090.3** | **23,359,546.6** | **-31.1%** |
| replay nexus (cmds/s) | 44,588,209.0 | 44,870,431.0 | +0.6% |
| e2e naive throughput (cmds/s) | 8,359,101.1 | 7,763,967.5 | -7.1% |
| **e2e naive latency p50 (ns)** | **166.7** | **125.0** | **-25.0%** |
| **e2e naive latency p90 (ns)** | **208.3** | **250.0** | **+20.0%** |
| **e2e naive latency p99 (ns)** | **17,125.0** | **97,291.7** | **+468.1%** |
| **e2e naive latency p99_9 (ns)** | **4,095,959.2** | **12,287,960.9** | **+200.0%** |
| **e2e naive latency max (ns)** | **15,297,086.5** | **23,308,713.2** | **+52.4%** |
| replay naive (cmds/s) | 12,545,220.0 | 12,034,485.0 | -4.1% |
| micro AddPassive/nexus (CPU ns/op) | 10.7 | 11.0 | +2.1% |
| micro AddPassive/naive (CPU ns/op) | 54.8 | 54.0 | -1.4% |
| micro Cancel/nexus (CPU ns/op) | 8.9 | 9.0 | +0.2% |
| micro Cancel/naive (CPU ns/op) | 62.0 | 61.0 | -1.6% |
| micro ModifyPrice/nexus (CPU ns/op) | 12.7 | 12.8 | +0.9% |
| micro ModifyPrice/naive (CPU ns/op) | 87.8 | 89.4 | +1.8% |
| micro Sweep/nexus/1 (CPU ns/op) | 13.6 | 13.2 | -2.9% |
| micro Sweep/nexus/8 (CPU ns/op) | 89.5 | 89.3 | -0.2% |
| micro Sweep/nexus/64 (CPU ns/op) | 701.8 | 707.5 | +0.8% |
| micro Sweep/naive/1 (CPU ns/op) | 56.7 | 57.4 | +1.3% |
| micro Sweep/naive/8 (CPU ns/op) | 391.2 | 395.9 | +1.2% |
| micro Sweep/naive/64 (CPU ns/op) | 3,115.0 | 3,108.8 | -0.2% |
