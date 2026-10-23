#pragma once
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#if defined(__x86_64__)
#include <x86intrin.h>
#elif defined(__APPLE__)
#include <mach/mach_time.h>
#endif

namespace nexus {

// Serialized cycle-counter reads. x86: TSC with lfence/rdtscp fencing. macOS: user-space
// cntvct_el0 reads are not consistent across cores there (measured going backwards across
// a 10 ms sleep), so use mach_absolute_time(), the OS-maintained monotonic counter; it ticks at
// 24 MHz on Apple M-series, so one tick is about 41.7 ns. Other AArch64: the generic timer
// (cntvct_el0) behind an isb. Elsewhere: steady_clock.
inline uint64_t tsc_begin() noexcept {
#if defined(__x86_64__)
  _mm_lfence();
  uint64_t t = __rdtsc();
  _mm_lfence();
  return t;
#elif defined(__APPLE__)
  return mach_absolute_time();
#elif defined(__aarch64__)
  uint64_t t;
  asm volatile("isb; mrs %0, cntvct_el0" : "=r"(t));
  return t;
#else
  return std::chrono::steady_clock::now().time_since_epoch().count();
#endif
}

inline uint64_t tsc_end() noexcept {
#if defined(__x86_64__)
  unsigned aux;
  uint64_t t = __rdtscp(&aux);
  _mm_lfence();
  return t;
#else
  return tsc_begin();
#endif
}

// Measures ticks->ns against steady_clock over ~200 ms. Never assumes a nominal frequency.
inline double calibrate_ns_per_tick() {
  using Clock = std::chrono::steady_clock;
  const auto t0 = Clock::now();
  const uint64_t c0 = tsc_begin();
  while (Clock::now() - t0 < std::chrono::milliseconds(200)) {
  }
  const uint64_t c1 = tsc_end();
  const auto t1 = Clock::now();
  return std::chrono::duration<double, std::nano>(t1 - t0).count() / double(c1 - c0);
}

// On x86 Linux the TSC is only a valid clock with constant_tsc and nonstop_tsc. Warns and
// returns false if either flag is missing; returns true elsewhere.
inline bool check_tsc_flags() {
#if defined(__x86_64__) && defined(__linux__)
  std::ifstream in("/proc/cpuinfo");
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("flags", 0) != 0) continue;
    const bool ok = line.find(" constant_tsc") != std::string::npos &&
                    line.find(" nonstop_tsc") != std::string::npos;
    if (!ok) std::fprintf(stderr, "warning: CPU lacks constant_tsc/nonstop_tsc\n");
    return ok;
  }
  std::fprintf(stderr, "warning: could not read TSC flags from /proc/cpuinfo\n");
  return false;
#else
  return true;
#endif
}

}  // namespace nexus
