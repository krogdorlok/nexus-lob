#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "nexus/core/price_level.hpp"

using nexus::Order;
using nexus::PriceLevel;

namespace {

Order make(nexus::OrderId id, nexus::Quantity qty) {
  Order o{};
  o.id = id;
  o.price = 100;
  o.qty = qty;
  return o;
}

std::vector<nexus::OrderId> ids(const PriceLevel& l) {
  std::vector<nexus::OrderId> out;
  for (Order* o = l.head(); o; o = o->next) out.push_back(o->id);
  std::vector<nexus::OrderId> back;  // walk prev links too, they must agree
  for (Order* o = l.tail(); o; o = o->prev) back.insert(back.begin(), o->id);
  EXPECT_EQ(out, back);
  return out;
}

}  // namespace

TEST(PriceLevel, EmptyByDefault) {
  PriceLevel l;
  EXPECT_TRUE(l.empty());
  EXPECT_EQ(l.head(), nullptr);
  EXPECT_EQ(l.tail(), nullptr);
  EXPECT_EQ(l.volume(), 0u);
  EXPECT_EQ(l.count(), 0u);
}

TEST(PriceLevel, AppendKeepsFifoAndTotals) {
  Order a = make(1, 10), b = make(2, 20), c = make(3, 30);
  PriceLevel l;
  l.append(&a);
  l.append(&b);
  l.append(&c);
  EXPECT_EQ(ids(l), (std::vector<nexus::OrderId>{1, 2, 3}));
  EXPECT_EQ(l.volume(), 60u);
  EXPECT_EQ(l.count(), 3u);
}

TEST(PriceLevel, UnlinkHeadMiddleTail) {
  Order a = make(1, 1), b = make(2, 2), c = make(3, 3), d = make(4, 4);
  PriceLevel l;
  for (Order* o : {&a, &b, &c, &d}) l.append(o);
  l.unlink(&b);  // middle
  EXPECT_EQ(ids(l), (std::vector<nexus::OrderId>{1, 3, 4}));
  l.unlink(&a);  // head
  EXPECT_EQ(ids(l), (std::vector<nexus::OrderId>{3, 4}));
  l.unlink(&d);  // tail
  EXPECT_EQ(ids(l), (std::vector<nexus::OrderId>{3}));
  EXPECT_EQ(l.volume(), 3u);
  EXPECT_EQ(l.count(), 1u);
  EXPECT_EQ(b.next, nullptr);
  EXPECT_EQ(b.prev, nullptr);
}

TEST(PriceLevel, UnlinkOnlyElement) {
  Order a = make(1, 5);
  PriceLevel l;
  l.append(&a);
  l.unlink(&a);
  EXPECT_TRUE(l.empty());
  EXPECT_EQ(l.tail(), nullptr);
  EXPECT_EQ(l.volume(), 0u);
  EXPECT_EQ(l.count(), 0u);
  l.append(&a);  // reusable after emptying
  EXPECT_EQ(ids(l), (std::vector<nexus::OrderId>{1}));
}

TEST(PriceLevel, VolumeExceeds32BitsAndFullFillUnlinks) {
  constexpr nexus::Quantity kMax = UINT32_MAX;
  Order a = make(1, kMax), b = make(2, kMax);
  PriceLevel l;
  l.append(&a);
  l.append(&b);
  EXPECT_EQ(l.volume(), 2ull * kMax);
  a.filled = kMax;  // fully filled, still linked
  l.reduce(kMax);
  EXPECT_EQ(l.volume(), uint64_t{kMax});
  l.unlink(&a);  // remaining is 0, volume unchanged
  EXPECT_EQ(l.volume(), uint64_t{kMax});
  EXPECT_EQ(l.count(), 1u);
}

TEST(PriceLevel, ReduceTracksPartialFills) {
  Order a = make(1, 10), b = make(2, 10);
  PriceLevel l;
  l.append(&a);
  l.append(&b);
  a.filled += 4;
  l.reduce(4);
  EXPECT_EQ(l.volume(), 16u);
  EXPECT_EQ(l.count(), 2u);
  l.unlink(&a);  // subtracts only a's remaining 6
  EXPECT_EQ(l.volume(), 10u);
}
