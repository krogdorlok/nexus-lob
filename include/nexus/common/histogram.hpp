#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace nexus {

// Fixed-bucket log-linear histogram, so record() never allocates. Values below 128 are exact;
// above that each power of two splits into 64 buckets (relative error under 1.6%).
class LatencyHistogram {
 public:
  void record(uint64_t v) noexcept {
    ++counts_[index(v)];
    ++n_;
    max_ = std::max(max_, v);
  }

  uint64_t count() const noexcept { return n_; }
  uint64_t max() const noexcept { return max_; }

  // Smallest bucket upper bound covering a fraction q of samples (nearest rank), capped at max.
  uint64_t percentile(double q) const noexcept {
    if (n_ == 0) return 0;
    const auto rank = std::max<uint64_t>(1, static_cast<uint64_t>(std::ceil(q * double(n_))));
    uint64_t seen = 0;
    for (size_t i = 0; i < kBuckets; ++i) {
      if ((seen += counts_[i]) >= rank) return std::min(upper(i), max_);
    }
    return max_;
  }

  static constexpr size_t index(uint64_t v) noexcept {
    if (v < kLinear) return static_cast<size_t>(v);
    const int shift = std::bit_width(v) - kSubBits;
    return static_cast<size_t>(shift) * kHalf + static_cast<size_t>(v >> shift);
  }
  static constexpr uint64_t upper(size_t i) noexcept {
    if (i < kLinear) return i;
    const size_t shift = i / kHalf - 1;
    return ((uint64_t{i - shift * kHalf} + 1) << shift) - 1;
  }

 private:
  static constexpr int kSubBits = 7;
  static constexpr size_t kLinear = size_t{1} << kSubBits, kHalf = kLinear / 2;
  static constexpr size_t kBuckets = (64 - kSubBits) * kHalf + kLinear;  // index(2^64-1) + 1

  std::array<uint64_t, kBuckets> counts_{};
  uint64_t n_{0}, max_{0};
};

}  // namespace nexus
