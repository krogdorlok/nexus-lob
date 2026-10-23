#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <memory>
#include <thread>

#include "nexus/common/histogram.hpp"
#include "nexus/queue/spsc_ring_buffer.hpp"

using nexus::SPSCRingBuffer;

TEST(SPSC, EmptyPopFails) {
  auto q = std::make_unique<SPSCRingBuffer<int, 4>>();
  int v = 0;
  EXPECT_FALSE(q->pop(v));
}

TEST(SPSC, FullPushFailsUntilPop) {
  auto q = std::make_unique<SPSCRingBuffer<int, 4>>();
  for (int i = 0; i < 4; ++i) EXPECT_TRUE(q->push(i));
  EXPECT_FALSE(q->push(99));
  int v = -1;
  ASSERT_TRUE(q->pop(v));
  EXPECT_EQ(v, 0);
  EXPECT_TRUE(q->push(4));
  EXPECT_FALSE(q->push(5));
  for (int want = 1; want <= 4; ++want) {
    ASSERT_TRUE(q->pop(v));
    EXPECT_EQ(v, want);
  }
  EXPECT_FALSE(q->pop(v));
}

// Starts the indices just below a boundary and cycles across it many times.
void cross_boundary(size_t start) {
  auto q = std::make_unique<SPSCRingBuffer<uint64_t, 8>>(start);
  uint64_t next_in = 0, next_out = 0, v = 0;
  for (int round = 0; round < 100; ++round) {
    const int burst = 1 + round % 8;
    for (int i = 0; i < burst; ++i) ASSERT_TRUE(q->push(next_in++));
    if (burst == 8) {
      ASSERT_FALSE(q->push(0));  // full detection across the wrap
    }
    for (int i = 0; i < burst; ++i) {
      ASSERT_TRUE(q->pop(v));
      ASSERT_EQ(v, next_out++);
    }
    ASSERT_FALSE(q->pop(v));  // empty detection across the wrap
  }
}

TEST(SPSC, WrapsPast32BitIndices) { cross_boundary((uint64_t{1} << 32) - 20); }

TEST(SPSC, WrapsPastSizeMax) { cross_boundary(std::numeric_limits<size_t>::max() - 20); }

TEST(SPSC, TenMillionItemsFifoAcrossThreads) {
  constexpr uint64_t kItems = 10'000'000;
  // Start near SIZE_MAX so the indices also wrap while both threads run.
  auto q = std::make_unique<SPSCRingBuffer<uint64_t, 1024>>(std::numeric_limits<size_t>::max() -
                                                            kItems / 2);
  std::thread producer([&] {
    for (uint64_t i = 0; i < kItems; ++i) {
      while (!q->push(i)) {
      }
    }
  });
  uint64_t expected = 0, v = 0;
  bool in_order = true;
  while (expected < kItems) {
    if (!q->pop(v)) continue;
    in_order &= v == expected++;
  }
  producer.join();
  EXPECT_TRUE(in_order);
  EXPECT_FALSE(q->pop(v));
}

// ---- LatencyHistogram (the publisher's recorder) ------------------------------------------

using nexus::LatencyHistogram;

TEST(LatencyHistogram, ExactBelow128AndBoundedErrorAbove) {
  for (uint64_t v : {0ull, 1ull, 127ull})
    EXPECT_EQ(LatencyHistogram::upper(LatencyHistogram::index(v)), v);
  for (uint64_t v = 128; v < 1'000'000; v = v * 3 / 2 + 1) {
    const uint64_t up = LatencyHistogram::upper(LatencyHistogram::index(v));
    EXPECT_GE(up, v);
    EXPECT_LE(double(up - v), double(v) / 64.0);
  }
  const uint64_t top = std::numeric_limits<uint64_t>::max();
  EXPECT_EQ(LatencyHistogram::upper(LatencyHistogram::index(top)), top);
}

TEST(LatencyHistogram, BucketsAreContiguousAndMonotonic) {
  for (size_t i = 1; i < 2000; ++i) {
    EXPECT_EQ(LatencyHistogram::index(LatencyHistogram::upper(i - 1) + 1), i);
  }
}

TEST(LatencyHistogram, Percentiles) {
  LatencyHistogram h;
  EXPECT_EQ(h.percentile(0.5), 0u);
  for (uint64_t v = 1; v <= 100; ++v) h.record(v);
  EXPECT_EQ(h.count(), 100u);
  EXPECT_EQ(h.percentile(0.5), 50u);
  EXPECT_EQ(h.percentile(0.99), 99u);
  EXPECT_EQ(h.percentile(1.0), 100u);
  h.record(1'000'000);
  EXPECT_EQ(h.max(), 1'000'000u);
  EXPECT_EQ(h.percentile(1.0), 1'000'000u);  // capped at the true max, not the bucket bound
}
