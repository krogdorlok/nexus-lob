#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "nexus/core/matching_engine.hpp"

using namespace nexus;

namespace {

constexpr Price kMin = 100, kMax = 200;

Command limit(OrderId id, Side s, Price p, Quantity q, TimeInForce tif = TimeInForce::GTC) {
  return {id, p, q, CommandType::New, s, OrderType::Limit, tif, 0};
}
Command market(OrderId id, Side s, Quantity q, TimeInForce tif = TimeInForce::IOC) {
  return {id, 0, q, CommandType::New, s, OrderType::Market, tif, 0};
}
Command cancel(OrderId id) {
  return {id, 0, 0, CommandType::Cancel, Side::Buy, OrderType::Limit, TimeInForce::GTC, 0};
}
Command modify(OrderId id, Price p, Quantity q) {
  return {id, p, q, CommandType::Modify, Side::Buy, OrderType::Limit, TimeInForce::GTC, 0};
}

using Events = std::vector<Event>;
constexpr Side B = Side::Buy, S = Side::Sell;

template <size_t Cap = 64>
struct Harness {
  MatchingEngine<Cap> engine{kMin, kMax};
  Events run(const Command& c) {
    Events out;
    engine.process(c, [&](const Event& e) { out.push_back(e); });
    return out;
  }
};

class Matching : public ::testing::Test {
 protected:
  Harness<> h;
  Events run(const Command& c) { return h.run(c); }
  TopOfBook top() const { return h.engine.top(); }
};

}  // namespace

// ---- Rejections: every RejectReason path -------------------------------------------------

TEST_F(Matching, RejectZeroQty) {
  EXPECT_EQ(run(limit(1, B, 150, 0)), Events{Event::rejected(1, RejectReason::ZeroQty)});
  EXPECT_EQ(run(market(2, S, 0)), Events{Event::rejected(2, RejectReason::ZeroQty)});
  // ZeroQty is checked before the band.
  EXPECT_EQ(run(limit(3, B, 999, 0)), Events{Event::rejected(3, RejectReason::ZeroQty)});
  EXPECT_EQ(h.engine.live_orders(), 0u);
}

TEST_F(Matching, RejectPriceOutOfBand) {
  EXPECT_EQ(run(limit(1, B, kMin - 1, 5)),
            Events{Event::rejected(1, RejectReason::PriceOutOfBand)});
  EXPECT_EQ(run(limit(2, S, kMax + 1, 5)),
            Events{Event::rejected(2, RejectReason::PriceOutOfBand)});
  EXPECT_EQ(run(limit(3, B, 0, 5, TimeInForce::IOC)),
            Events{Event::rejected(3, RejectReason::PriceOutOfBand)});
  // Band edges themselves are valid.
  EXPECT_EQ(run(limit(4, B, kMin, 5)), Events{Event::accepted(4, B, kMin, 5)});
  EXPECT_EQ(run(limit(5, S, kMax, 5)), Events{Event::accepted(5, S, kMax, 5)});
}

TEST_F(Matching, RejectDuplicateIdOnlyWhileLive) {
  run(limit(1, B, 150, 5));
  EXPECT_EQ(run(limit(1, S, 160, 5)), Events{Event::rejected(1, RejectReason::DuplicateId)});
  run(cancel(1));
  EXPECT_EQ(run(limit(1, S, 160, 5)), Events{Event::accepted(1, S, 160, 5)});
}

TEST_F(Matching, FullyFilledIdCanBeReused) {
  run(limit(1, S, 150, 5));
  run(limit(2, B, 150, 5));
  EXPECT_EQ(h.engine.live_orders(), 0u);
  EXPECT_EQ(run(limit(1, B, 140, 1)), Events{Event::accepted(1, B, 140, 1)});
}

TEST(MatchingPool, RejectPoolExhaustedOnlyForRestingOrders) {
  Harness<2> h;
  h.run(limit(1, B, 150, 5));
  h.run(limit(2, B, 151, 5));
  EXPECT_EQ(h.run(limit(3, B, 152, 5)), Events{Event::rejected(3, RejectReason::PoolExhausted)});
  // Orders that never rest are still allowed when full.
  EXPECT_EQ(h.run(limit(4, S, 151, 2, TimeInForce::IOC)),
            (Events{Event::accepted(4, S, 151, 2), Event::fill(4, 2, S, 151, 2)}));
  EXPECT_EQ(h.run(market(5, S, 1)),
            (Events{Event::accepted(5, S, 0, 1), Event::fill(5, 2, S, 151, 1)}));
  // A modify reuses its slot and cannot exhaust the pool.
  EXPECT_EQ(h.run(modify(1, 149, 7)), Events{Event::modified(1, B, 149, 7)});
  h.run(cancel(2));
  EXPECT_EQ(h.run(limit(6, B, 152, 5)), Events{Event::accepted(6, B, 152, 5)});
}

TEST_F(Matching, RejectFokUnfillableWithoutSideEffects) {
  run(limit(1, S, 150, 5));
  run(limit(2, S, 152, 5));
  const TopOfBook before = top();
  EXPECT_EQ(run(limit(3, B, 151, 6, TimeInForce::FOK)),
            Events{Event::rejected(3, RejectReason::FokUnfillable)});
  EXPECT_EQ(run(market(4, B, 11, TimeInForce::FOK)),
            Events{Event::rejected(4, RejectReason::FokUnfillable)});
  EXPECT_EQ(top(), before);
  EXPECT_EQ(h.engine.live_orders(), 2u);
}

TEST_F(Matching, RejectUnknownId) {
  EXPECT_EQ(run(cancel(9)), Events{Event::rejected(9, RejectReason::UnknownId)});
  EXPECT_EQ(run(modify(9, 150, 1)), Events{Event::rejected(9, RejectReason::UnknownId)});
  run(limit(1, B, 150, 5));
  run(cancel(1));
  EXPECT_EQ(run(cancel(1)), Events{Event::rejected(1, RejectReason::UnknownId)});
}

TEST_F(Matching, ModifyRejectsInCheckOrder) {
  run(limit(1, B, 150, 5));
  EXPECT_EQ(run(modify(1, 150, 0)), Events{Event::rejected(1, RejectReason::ZeroQty)});
  EXPECT_EQ(run(modify(1, kMax + 1, 5)), Events{Event::rejected(1, RejectReason::PriceOutOfBand)});
  EXPECT_EQ(run(modify(7, kMax + 1, 0)), Events{Event::rejected(7, RejectReason::ZeroQty)});
  EXPECT_EQ(run(modify(7, kMax + 1, 5)), Events{Event::rejected(7, RejectReason::PriceOutOfBand)});
  EXPECT_EQ(top().bid_volume, 5u);  // untouched
}

// ---- Resting, priority, fills --------------------------------------------------------------

TEST_F(Matching, PassiveOrdersRestAndShowInTopOfBook) {
  EXPECT_EQ(run(limit(1, B, 149, 5)), Events{Event::accepted(1, B, 149, 5)});
  run(limit(2, B, 149, 7));
  run(limit(3, S, 151, 4));
  EXPECT_EQ(top(), (TopOfBook{149, 12, 151, 4}));
  EXPECT_EQ(h.engine.live_orders(), 3u);
}

TEST_F(Matching, EmptyBookTop) { EXPECT_EQ(top(), TopOfBook{}); }

TEST_F(Matching, PartialFillsAtSameLevelInFifoOrder) {
  run(limit(1, S, 150, 5));
  run(limit(2, S, 150, 5));
  EXPECT_EQ(run(limit(3, B, 150, 7)),
            (Events{Event::accepted(3, B, 150, 7), Event::fill(3, 1, B, 150, 5),
                    Event::fill(3, 2, B, 150, 2)}));
  EXPECT_EQ(top(), (TopOfBook{kNoPrice, 0, 150, 3}));
  EXPECT_EQ(run(cancel(2)), Events{Event::cancelled(2, S, 150, 3)});  // remaining after fill
}

TEST_F(Matching, ExactFillEmptiesLevelAndBestMoves) {
  run(limit(1, S, 150, 5));
  run(limit(2, S, 155, 5));
  EXPECT_EQ(run(limit(3, B, 150, 5)),
            (Events{Event::accepted(3, B, 150, 5), Event::fill(3, 1, B, 150, 5)}));
  EXPECT_EQ(top(), (TopOfBook{kNoPrice, 0, 155, 5}));
  EXPECT_EQ(h.engine.live_orders(), 1u);
}

TEST_F(Matching, MultiLevelSweepFillsAtRestingPricesThenRests) {
  run(limit(1, S, 151, 2));
  run(limit(2, S, 152, 3));
  run(limit(3, S, 154, 4));
  EXPECT_EQ(run(limit(4, B, 153, 10)),
            (Events{Event::accepted(4, B, 153, 10), Event::fill(4, 1, B, 151, 2),
                    Event::fill(4, 2, B, 152, 3)}));
  EXPECT_EQ(top(), (TopOfBook{153, 5, 154, 4}));
}

TEST_F(Matching, SellSweepsBidsDownward) {
  run(limit(1, B, 149, 2));
  run(limit(2, B, 147, 3));
  run(limit(3, B, 148, 1));
  EXPECT_EQ(run(limit(4, S, 147, 6)),
            (Events{Event::accepted(4, S, 147, 6), Event::fill(4, 1, S, 149, 2),
                    Event::fill(4, 3, S, 148, 1), Event::fill(4, 2, S, 147, 3)}));
  EXPECT_EQ(top(), TopOfBook{});
}

TEST_F(Matching, BetterPriceBeatsEarlierTime) {
  run(limit(1, S, 152, 1));
  run(limit(2, S, 151, 1));
  EXPECT_EQ(run(limit(3, B, 152, 1)),
            (Events{Event::accepted(3, B, 152, 1), Event::fill(3, 2, B, 151, 1)}));
}

TEST_F(Matching, MarketSweepsAndNeverRests) {
  run(limit(1, S, 150, 2));
  run(limit(2, S, 200, 2));
  EXPECT_EQ(run(market(3, B, 5, TimeInForce::GTC)),
            (Events{Event::accepted(3, B, 0, 5), Event::fill(3, 1, B, 150, 2),
                    Event::fill(3, 2, B, 200, 2), Event::cancelled(3, B, 0, 1)}));
  EXPECT_EQ(top(), TopOfBook{});
  EXPECT_EQ(run(market(4, S, 3)),
            (Events{Event::accepted(4, S, 0, 3), Event::cancelled(4, S, 0, 3)}));  // empty book
  EXPECT_EQ(h.engine.live_orders(), 0u);
}

TEST_F(Matching, IocRemainderIsCancelled) {
  run(limit(1, B, 150, 2));
  EXPECT_EQ(run(limit(2, S, 149, 5, TimeInForce::IOC)),
            (Events{Event::accepted(2, S, 149, 5), Event::fill(2, 1, S, 150, 2),
                    Event::cancelled(2, S, 149, 3)}));
  EXPECT_EQ(top(), TopOfBook{});
  EXPECT_EQ(run(limit(3, S, 160, 5, TimeInForce::IOC)),
            (Events{Event::accepted(3, S, 160, 5), Event::cancelled(3, S, 160, 5)}));
}

TEST_F(Matching, FokFillsExactlyAcrossLevels) {
  run(limit(1, S, 150, 2));
  run(limit(2, S, 151, 3));
  run(limit(3, S, 152, 9));
  EXPECT_EQ(run(limit(4, B, 151, 5, TimeInForce::FOK)),
            (Events{Event::accepted(4, B, 151, 5), Event::fill(4, 1, B, 150, 2),
                    Event::fill(4, 2, B, 151, 3)}));
  EXPECT_EQ(top(), (TopOfBook{kNoPrice, 0, 152, 9}));
}

TEST_F(Matching, CancelReportsRemainingAndUpdatesBest) {
  run(limit(1, B, 150, 5));
  run(limit(2, B, 148, 5));
  EXPECT_EQ(run(cancel(1)), Events{Event::cancelled(1, B, 150, 5)});
  EXPECT_EQ(top(), (TopOfBook{148, 5, kNoPrice, 0}));
}

// ---- Modify ------------------------------------------------------------------------------

TEST_F(Matching, ModifyQtyDownKeepsPriority) {
  run(limit(1, S, 150, 5));
  run(limit(2, S, 150, 5));
  EXPECT_EQ(run(modify(1, 150, 3)), Events{Event::modified(1, S, 150, 3)});
  EXPECT_EQ(top().ask_volume, 8u);
  EXPECT_EQ(run(limit(3, B, 150, 3)),
            (Events{Event::accepted(3, B, 150, 3), Event::fill(3, 1, B, 150, 3)}));
}

TEST_F(Matching, ModifySameQtyKeepsPriority) {
  run(limit(1, S, 150, 5));
  run(limit(2, S, 150, 5));
  run(modify(1, 150, 5));
  EXPECT_EQ(run(limit(3, B, 150, 1))[1], Event::fill(3, 1, B, 150, 1));
}

TEST_F(Matching, ModifyAfterPartialFillShrinksRemaining) {
  run(limit(1, S, 150, 10));
  run(limit(2, B, 150, 4));  // 1 has 6 remaining
  EXPECT_EQ(run(modify(1, 150, 6)), Events{Event::modified(1, S, 150, 6)});
  EXPECT_EQ(top().ask_volume, 6u);
  run(modify(1, 150, 2));
  EXPECT_EQ(run(cancel(1)), Events{Event::cancelled(1, S, 150, 2)});
}

TEST_F(Matching, ModifyQtyUpLosesPriority) {
  run(limit(1, S, 150, 5));
  run(limit(2, S, 150, 5));
  run(modify(1, 150, 6));
  EXPECT_EQ(top().ask_volume, 11u);
  EXPECT_EQ(run(limit(3, B, 150, 1))[1], Event::fill(3, 2, B, 150, 1));
}

TEST_F(Matching, ModifyPriceLosesPriority) {
  run(limit(1, S, 151, 5));
  run(limit(2, S, 150, 5));
  run(modify(1, 150, 5));
  EXPECT_EQ(top(), (TopOfBook{kNoPrice, 0, 150, 10}));
  EXPECT_EQ(run(limit(3, B, 150, 1))[1], Event::fill(3, 2, B, 150, 1));
}

TEST_F(Matching, ModifyIntoCrossMatchesThenRests) {
  run(limit(1, S, 152, 3));
  run(limit(2, B, 148, 5));
  EXPECT_EQ(run(modify(2, 152, 5)),
            (Events{Event::modified(2, B, 152, 5), Event::fill(2, 1, B, 152, 3)}));
  EXPECT_EQ(top(), (TopOfBook{152, 2, kNoPrice, 0}));
  EXPECT_EQ(run(cancel(2)), Events{Event::cancelled(2, B, 152, 2)});
}

TEST_F(Matching, ModifyFullyFilledLeavesNothingResting) {
  run(limit(1, S, 152, 5));
  run(limit(2, B, 148, 5));
  EXPECT_EQ(run(modify(2, 153, 5)),
            (Events{Event::modified(2, B, 153, 5), Event::fill(2, 1, B, 152, 5)}));
  EXPECT_EQ(h.engine.live_orders(), 0u);
  EXPECT_EQ(top(), TopOfBook{});
  EXPECT_EQ(run(cancel(2)), Events{Event::rejected(2, RejectReason::UnknownId)});
}

TEST_F(Matching, ModifyNeverMatchesItself) {
  run(limit(1, B, 150, 5));
  EXPECT_EQ(run(modify(1, 151, 5)), Events{Event::modified(1, B, 151, 5)});
  EXPECT_EQ(top(), (TopOfBook{151, 5, kNoPrice, 0}));
}

// ---- Band edges ----------------------------------------------------------------------------

TEST_F(Matching, CrossingAtTopBandEdge) {
  run(limit(1, S, kMax - 1, 1));
  run(limit(2, S, kMax, 1));
  EXPECT_EQ(run(limit(3, B, kMax, 5)),
            (Events{Event::accepted(3, B, kMax, 5), Event::fill(3, 1, B, kMax - 1, 1),
                    Event::fill(3, 2, B, kMax, 1)}));
  EXPECT_EQ(top(), (TopOfBook{kMax, 3, kNoPrice, 0}));
}

TEST_F(Matching, CrossingAtBottomBandEdge) {
  run(limit(1, B, kMin + 1, 1));
  run(limit(2, B, kMin, 1));
  EXPECT_EQ(run(limit(3, S, kMin, 5)),
            (Events{Event::accepted(3, S, kMin, 5), Event::fill(3, 1, S, kMin + 1, 1),
                    Event::fill(3, 2, S, kMin, 1)}));
  EXPECT_EQ(top(), (TopOfBook{kNoPrice, 0, kMin, 3}));
}

TEST_F(Matching, SellAtBottomEdgeTakesBidAtTopEdge) {
  run(limit(1, B, kMax, 2));
  EXPECT_EQ(run(limit(2, S, kMin, 2)),
            (Events{Event::accepted(2, S, kMin, 2), Event::fill(2, 1, S, kMax, 2)}));
  EXPECT_EQ(top(), TopOfBook{});
}

TEST_F(Matching, FokChecksAcrossFullBand) {
  run(limit(1, S, kMin, 1));
  run(limit(2, S, kMax, 1));
  EXPECT_EQ(run(limit(3, B, kMax, 2, TimeInForce::FOK)),
            (Events{Event::accepted(3, B, kMax, 2), Event::fill(3, 1, B, kMin, 1),
                    Event::fill(3, 2, B, kMax, 1)}));
  run(limit(4, B, kMin, 1));
  run(limit(5, B, kMax, 1));
  EXPECT_EQ(run(market(6, S, 2, TimeInForce::FOK)),
            (Events{Event::accepted(6, S, 0, 2), Event::fill(6, 5, S, kMax, 1),
                    Event::fill(6, 4, S, kMin, 1)}));
  EXPECT_EQ(top(), TopOfBook{});
}
