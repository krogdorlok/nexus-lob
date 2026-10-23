#include <gtest/gtest.h>

#include <cstdint>
#include <set>

#include "nexus/core/order.hpp"
#include "nexus/memory/object_pool.hpp"

using nexus::ObjectPool;
using nexus::Order;

TEST(ObjectPool, ExhaustionReturnsNull) {
  ObjectPool<Order, 4> pool;
  EXPECT_EQ(pool.available(), 4u);
  for (int i = 0; i < 4; ++i) EXPECT_NE(pool.allocate(), nullptr);
  EXPECT_EQ(pool.available(), 0u);
  EXPECT_EQ(pool.allocate(), nullptr);
  EXPECT_EQ(pool.allocate(), nullptr);  // stays exhausted
}

TEST(ObjectPool, ReuseAfterFree) {
  ObjectPool<Order, 2> pool;
  Order* a = pool.allocate();
  Order* b = pool.allocate();
  ASSERT_EQ(pool.allocate(), nullptr);
  pool.deallocate(a);
  EXPECT_EQ(pool.allocate(), a);
  pool.deallocate(b);
  EXPECT_EQ(pool.allocate(), b);
}

TEST(ObjectPool, LifoOrder) {
  ObjectPool<Order, 8> pool;
  Order* a = pool.allocate();
  Order* b = pool.allocate();
  Order* c = pool.allocate();
  pool.deallocate(a);
  pool.deallocate(c);
  pool.deallocate(b);
  EXPECT_EQ(pool.allocate(), b);
  EXPECT_EQ(pool.allocate(), c);
  EXPECT_EQ(pool.allocate(), a);
}

TEST(ObjectPool, AlignmentAndDistinctSlots) {
  constexpr size_t kCap = 1024;
  ObjectPool<Order, kCap> pool;
  std::set<Order*> seen;
  for (size_t i = 0; i < kCap; ++i) {
    Order* o = pool.allocate();
    ASSERT_NE(o, nullptr);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(o) % 64, 0u);
    EXPECT_TRUE(seen.insert(o).second);
  }
}

TEST(ObjectPool, ForwardsConstructorArguments) {
  ObjectPool<Order, 1> pool;
  Order* o = pool.allocate(nexus::OrderId{7}, nexus::Price{100}, nexus::Quantity{5},
                           nexus::Quantity{0}, nexus::Timestamp{0}, nexus::Side::Sell,
                           nexus::OrderType::Limit, nexus::TimeInForce::GTC);
  ASSERT_NE(o, nullptr);
  EXPECT_EQ(o->id, 7u);
  EXPECT_EQ(o->price, 100u);
  EXPECT_EQ(o->remaining(), 5u);
  EXPECT_EQ(o->side, nexus::Side::Sell);
  EXPECT_EQ(o->next, nullptr);
  EXPECT_EQ(o->prev, nullptr);
}
