#pragma once
#include <bit>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "nexus/core/price_level.hpp"

namespace nexus {

// One side of the book: a flat array of PriceLevel indexed by (price - min_price), plus a
// two-level occupancy bitmap. Bit i of l0_ is set iff level i is non-empty; bit w of l1_ is
// set iff l0_ word w is non-zero. The best price is always derived from the bitmap, never
// cached, so it cannot go stale. A search costs a few countl_zero/countr_zero calls plus a
// scan over at most band/4096 l1_ words.
class PriceLadder {
 public:
  static constexpr size_t kMaxLevels = size_t{1} << 24;

  PriceLadder(Price min_price, Price max_price) : min_(min_price), max_(max_price) {
    if (min_price > max_price || max_price == kNoPrice || max_price - min_price >= kMaxLevels) {
      throw std::invalid_argument("PriceLadder: invalid price band");
    }
    const size_t n = max_price - min_price + 1;
    levels_.resize(n);
    l0_.resize((n + 63) / 64);
    l1_.resize((l0_.size() + 63) / 64);
  }

  bool in_band(Price p) const noexcept { return p >= min_ && p <= max_; }
  Price min_price() const noexcept { return min_; }
  Price max_price() const noexcept { return max_; }
  PriceLevel& operator[](Price p) noexcept { return levels_[p - min_]; }
  const PriceLevel& operator[](Price p) const noexcept { return levels_[p - min_]; }

  // Keep the bitmap in sync with level emptiness: mark after the first append, clear after
  // the last unlink.
  void mark(Price p) noexcept {
    const size_t i = p - min_;
    l0_[i >> 6] |= bit(i & 63);
    l1_[i >> 12] |= bit((i >> 6) & 63);
  }
  void clear(Price p) noexcept {
    const size_t i = p - min_;
    if ((l0_[i >> 6] &= ~bit(i & 63)) == 0) l1_[i >> 12] &= ~bit((i >> 6) & 63);
  }

  Price highest() const noexcept { return at_or_below(max_); }
  Price lowest() const noexcept { return at_or_above(min_); }

  // Nearest non-empty level at or below p (p in band), or kNoPrice.
  Price at_or_below(Price p) const noexcept {
    const size_t i = p - min_, w = i >> 6;
    if (const uint64_t m = l0_[w] & (~uint64_t{0} >> (63 - (i & 63)))) return price(w, top(m));
    const size_t w1 = w >> 6;
    if (const uint64_t m = l1_[w1] & (bit(w & 63) - 1)) return highest_in(w1 * 64 + top(m));
    for (size_t k = w1; k-- > 0;) {
      if (l1_[k]) return highest_in(k * 64 + top(l1_[k]));
    }
    return kNoPrice;
  }

  // Nearest non-empty level at or above p (p in band), or kNoPrice.
  Price at_or_above(Price p) const noexcept {
    const size_t i = p - min_, w = i >> 6;
    if (const uint64_t m = l0_[w] & (~uint64_t{0} << (i & 63))) return price(w, bottom(m));
    const size_t w1 = w >> 6;
    if (const uint64_t m = l1_[w1] & ~(bit(w & 63) | (bit(w & 63) - 1))) {
      return lowest_in(w1 * 64 + bottom(m));
    }
    for (size_t k = w1 + 1; k < l1_.size(); ++k) {
      if (l1_[k]) return lowest_in(k * 64 + bottom(l1_[k]));
    }
    return kNoPrice;
  }

 private:
  static constexpr uint64_t bit(size_t b) noexcept { return uint64_t{1} << b; }
  static constexpr size_t top(uint64_t m) noexcept { return 63 - std::countl_zero(m); }
  static constexpr size_t bottom(uint64_t m) noexcept { return std::countr_zero(m); }
  Price price(size_t word, size_t b) const noexcept { return min_ + word * 64 + b; }
  Price highest_in(size_t word) const noexcept { return price(word, top(l0_[word])); }
  Price lowest_in(size_t word) const noexcept { return price(word, bottom(l0_[word])); }

  Price min_, max_;
  std::vector<PriceLevel> levels_;
  std::vector<uint64_t> l0_, l1_;
};

}  // namespace nexus
