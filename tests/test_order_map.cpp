#include <gtest/gtest.h>

#include <random>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include "nexus/container/order_map.hpp"

using nexus::Order;
using nexus::OrderId;
using nexus::OrderMap;

namespace {

Order* tag(uintptr_t v) { return reinterpret_cast<Order*>(v * 64); }  // distinct non-null keys

// First `n` ids whose home slot is `h`.
std::vector<OrderId> ids_with_home(const OrderMap& m, size_t h, size_t n) {
  std::vector<OrderId> out;
  for (OrderId k = 1; out.size() < n; ++k) {
    if (m.home(k) == h) out.push_back(k);
  }
  return out;
}

}  // namespace

TEST(OrderMap, CapacityIsPowerOfTwoWithLoadAtMostHalf) {
  OrderMap m(1000);
  EXPECT_EQ(m.capacity(), 2048u);
  EXPECT_EQ(m.max_entries(), 1024u);
  OrderMap tiny(0);
  EXPECT_EQ(tiny.capacity(), 2u);
  EXPECT_EQ(tiny.max_entries(), 1u);
}

TEST(OrderMap, RejectsImpossibleSizes) {
  EXPECT_THROW(OrderMap(size_t{1} << 63), std::length_error);
  EXPECT_THROW(OrderMap(OrderMap::kMaxEntries + 1), std::length_error);
}

TEST(OrderMap, InsertFindErase) {
  OrderMap m(16);
  EXPECT_EQ(m.find(1), nullptr);
  EXPECT_TRUE(m.insert(1, tag(1)));
  EXPECT_FALSE(m.insert(1, tag(2)));  // duplicate
  EXPECT_EQ(m.find(1), tag(1));
  EXPECT_EQ(m.size(), 1u);
  EXPECT_TRUE(m.erase(1));
  EXPECT_FALSE(m.erase(1));
  EXPECT_EQ(m.find(1), nullptr);
  EXPECT_EQ(m.size(), 0u);
}

TEST(OrderMap, IdZeroIsAValidKey) {
  OrderMap m(4);
  EXPECT_FALSE(m.erase(0));
  EXPECT_TRUE(m.insert(0, tag(1)));
  EXPECT_EQ(m.find(0), tag(1));
  EXPECT_TRUE(m.erase(0));
  EXPECT_EQ(m.find(0), nullptr);
}

TEST(OrderMap, CollisionsProbeLinearly) {
  OrderMap m(8);
  auto ids = ids_with_home(m, 3, 4);
  for (size_t i = 0; i < ids.size(); ++i) ASSERT_TRUE(m.insert(ids[i], tag(i + 1)));
  for (size_t i = 0; i < ids.size(); ++i) EXPECT_EQ(m.find(ids[i]), tag(i + 1));
}

TEST(OrderMap, DeleteInClusterKeepsOthersReachable) {
  OrderMap m(8);
  auto same = ids_with_home(m, 5, 3);
  auto next = ids_with_home(m, 6, 1);  // displaced by the cluster at 5
  for (size_t i = 0; i < 3; ++i) ASSERT_TRUE(m.insert(same[i], tag(i + 1)));
  ASSERT_TRUE(m.insert(next[0], tag(9)));
  ASSERT_TRUE(m.erase(same[0]));  // hole at the head of the cluster
  EXPECT_EQ(m.find(same[0]), nullptr);
  EXPECT_EQ(m.find(same[1]), tag(2));
  EXPECT_EQ(m.find(same[2]), tag(3));
  EXPECT_EQ(m.find(next[0]), tag(9));
  ASSERT_TRUE(m.erase(same[2]));  // hole in the middle
  EXPECT_EQ(m.find(same[1]), tag(2));
  EXPECT_EQ(m.find(next[0]), tag(9));
}

TEST(OrderMap, ClusterWrapsAroundTableEnd) {
  OrderMap m(8);
  const size_t last = m.capacity() - 1;
  auto wrap = ids_with_home(m, last, 3);
  auto zero = ids_with_home(m, 0, 1);
  for (size_t i = 0; i < 3; ++i) ASSERT_TRUE(m.insert(wrap[i], tag(i + 1)));
  ASSERT_TRUE(m.insert(zero[0], tag(7)));
  ASSERT_TRUE(m.erase(wrap[0]));
  EXPECT_EQ(m.find(wrap[1]), tag(2));
  EXPECT_EQ(m.find(wrap[2]), tag(3));
  EXPECT_EQ(m.find(zero[0]), tag(7));
}

TEST(OrderMap, BackwardShiftSkipsKeyAtItsHomeAcrossWrap) {
  OrderMap m(8);  // 16 slots
  const size_t last = m.capacity() - 1;
  auto a = ids_with_home(m, last - 1, 2);  // homes 14, 14
  auto b = ids_with_home(m, last, 1);      // home 15
  ASSERT_TRUE(m.insert(a[0], tag(1)));     // slot 14
  ASSERT_TRUE(m.insert(b[0], tag(2)));     // slot 15, its home
  ASSERT_TRUE(m.insert(a[1], tag(3)));     // wraps to slot 0
  ASSERT_TRUE(m.erase(a[0]));  // b must stay at 15; a[1] must shift back past it into 14
  EXPECT_EQ(m.find(b[0]), tag(2));
  EXPECT_EQ(m.find(a[1]), tag(3));
  EXPECT_EQ(m.find(a[0]), nullptr);
  ASSERT_TRUE(m.erase(b[0]));
  EXPECT_EQ(m.find(a[1]), tag(3));
}

TEST(OrderMap, Reinsertion) {
  OrderMap m(4);
  for (int round = 0; round < 100; ++round) {
    ASSERT_TRUE(m.insert(42, tag(round + 1)));
    ASSERT_EQ(m.find(42), tag(round + 1));
    ASSERT_TRUE(m.erase(42));
  }
  EXPECT_EQ(m.size(), 0u);
}

TEST(OrderMap, FullTableRejectsUntilErase) {
  OrderMap m(8);
  for (OrderId k = 1; k <= m.max_entries(); ++k) ASSERT_TRUE(m.insert(k, tag(k)));
  EXPECT_FALSE(m.insert(1000, tag(1000)));
  EXPECT_EQ(m.find(1000), nullptr);
  EXPECT_EQ(m.size(), m.max_entries());
  ASSERT_TRUE(m.erase(3));
  EXPECT_TRUE(m.insert(1000, tag(1000)));
  for (OrderId k = 1; k <= m.max_entries(); ++k) {
    if (k != 3) {
      EXPECT_EQ(m.find(k), tag(k));
    }
  }
}

TEST(OrderMap, MatchesStdUnorderedMapUnderRandomOps) {
  OrderMap m(64);
  std::unordered_map<OrderId, Order*> ref;
  std::mt19937_64 rng(7);
  for (int i = 0; i < 200'000; ++i) {
    const OrderId k = rng() % 256;  // dense keys force long clusters and many deletions
    const auto op = rng() % 3;
    if (op == 0) {
      const bool full = ref.size() == m.max_entries();
      const bool expect = !full && !ref.count(k);
      ASSERT_EQ(m.insert(k, tag(k + 1)), expect);
      if (expect) ref[k] = tag(k + 1);
    } else if (op == 1) {
      ASSERT_EQ(m.erase(k), ref.erase(k) == 1);
    } else {
      auto it = ref.find(k);
      ASSERT_EQ(m.find(k), it == ref.end() ? nullptr : it->second);
    }
    ASSERT_EQ(m.size(), ref.size());
  }
}
