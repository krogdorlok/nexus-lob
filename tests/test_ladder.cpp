#include <gtest/gtest.h>

#include <iterator>
#include <random>
#include <set>
#include <stdexcept>

#include "nexus/container/price_ladder.hpp"

using nexus::kNoPrice;
using nexus::Price;
using nexus::PriceLadder;

TEST(PriceLadder, RejectsInvalidBands) {
  EXPECT_THROW(PriceLadder(10, 9), std::invalid_argument);
  EXPECT_THROW(PriceLadder(0, kNoPrice), std::invalid_argument);
  EXPECT_THROW(PriceLadder(0, PriceLadder::kMaxLevels), std::invalid_argument);
  EXPECT_NO_THROW(PriceLadder(5, 5));
}

TEST(PriceLadder, InBand) {
  PriceLadder l(100, 200);
  EXPECT_FALSE(l.in_band(99));
  EXPECT_TRUE(l.in_band(100));
  EXPECT_TRUE(l.in_band(200));
  EXPECT_FALSE(l.in_band(201));
}

TEST(PriceLadder, EmptyBook) {
  PriceLadder l(100, 200);
  EXPECT_EQ(l.highest(), kNoPrice);
  EXPECT_EQ(l.lowest(), kNoPrice);
  EXPECT_EQ(l.at_or_below(150), kNoPrice);
  EXPECT_EQ(l.at_or_above(150), kNoPrice);
}

TEST(PriceLadder, SingleLevel) {
  PriceLadder l(100, 200);
  l.mark(150);
  EXPECT_EQ(l.highest(), 150u);
  EXPECT_EQ(l.lowest(), 150u);
  EXPECT_EQ(l.at_or_below(149), kNoPrice);
  EXPECT_EQ(l.at_or_above(151), kNoPrice);
  EXPECT_EQ(l.at_or_below(150), 150u);
  EXPECT_EQ(l.at_or_above(150), 150u);
  l.clear(150);
  EXPECT_EQ(l.highest(), kNoPrice);
  EXPECT_EQ(l.lowest(), kNoPrice);
}

TEST(PriceLadder, BandEdges) {
  PriceLadder l(1000, 1000 + 70'000);  // spans several l1 words
  l.mark(1000);
  l.mark(71'000);
  EXPECT_EQ(l.lowest(), 1000u);
  EXPECT_EQ(l.highest(), 71'000u);
  EXPECT_EQ(l.at_or_above(1001), 71'000u);
  EXPECT_EQ(l.at_or_below(70'999), 1000u);
  l.clear(1000);
  EXPECT_EQ(l.lowest(), 71'000u);
  l.clear(71'000);
  EXPECT_EQ(l.highest(), kNoPrice);
}

TEST(PriceLadder, BandJustBelowSentinel) {
  const Price lo = kNoPrice - 65, hi = kNoPrice - 1;
  PriceLadder l(lo, hi);
  for (Price off : {0, 63, 64}) l.mark(lo + off);
  EXPECT_EQ(l.highest(), hi);
  EXPECT_EQ(l.lowest(), lo);
  EXPECT_EQ(l.at_or_below(lo + 62), lo);
  EXPECT_EQ(l.at_or_above(lo + 1), lo + 63);
  l.clear(hi);
  EXPECT_EQ(l.highest(), lo + 63);
  l.clear(lo + 63);
  l.clear(lo);
  EXPECT_EQ(l.at_or_above(lo), kNoPrice);
}

TEST(PriceLadder, BestUpdatesAfterLevelsEmpty) {
  PriceLadder l(0, 10'000);
  for (Price p : {10, 63, 64, 4095, 4096, 9000}) l.mark(p);
  EXPECT_EQ(l.highest(), 9000u);
  l.clear(9000);
  EXPECT_EQ(l.highest(), 4096u);  // crosses an l1 word boundary
  l.clear(4096);
  EXPECT_EQ(l.highest(), 4095u);
  l.clear(4095);
  EXPECT_EQ(l.highest(), 64u);
  l.clear(64);
  EXPECT_EQ(l.highest(), 63u);  // crosses an l0 word boundary
  EXPECT_EQ(l.lowest(), 10u);
  l.clear(10);
  EXPECT_EQ(l.lowest(), 63u);
}

TEST(PriceLadder, ClearKeepsSiblingBitsInSameWord) {
  PriceLadder l(0, 127);
  l.mark(3);
  l.mark(5);
  l.clear(5);
  EXPECT_EQ(l.highest(), 3u);  // l1 bit must survive while the word is non-zero
  l.clear(3);
  EXPECT_EQ(l.highest(), kNoPrice);
}

TEST(PriceLadder, LevelsAreIndexedByPrice) {
  PriceLadder l(100, 200);
  nexus::Order o{};
  o.qty = 7;
  l[150].append(&o);
  EXPECT_EQ(l[150].volume(), 7u);
  EXPECT_TRUE(l[151].empty());
}

TEST(PriceLadder, MatchesStdSetUnderRandomOps) {
  const Price lo = 500, hi = 500 + 20'000;
  PriceLadder l(lo, hi);
  std::set<Price> ref;
  std::mt19937_64 rng(11);
  for (int i = 0; i < 200'000; ++i) {
    // Cluster around a few hot spots so words fill and empty repeatedly.
    const Price p = lo + (rng() % 4) * 6000 + rng() % 300;
    if (rng() % 2) {
      l.mark(p);
      ref.insert(p);
    } else {
      l.clear(p);
      ref.erase(p);
    }
    const Price q = lo + rng() % (hi - lo + 1);
    auto ge = ref.lower_bound(q);
    auto gt = ref.upper_bound(q);
    ASSERT_EQ(l.at_or_above(q), ge == ref.end() ? kNoPrice : *ge);
    ASSERT_EQ(l.at_or_below(q), gt == ref.begin() ? kNoPrice : *std::prev(gt));
    ASSERT_EQ(l.highest(), ref.empty() ? kNoPrice : *ref.rbegin());
    ASSERT_EQ(l.lowest(), ref.empty() ? kNoPrice : *ref.begin());
  }
}
