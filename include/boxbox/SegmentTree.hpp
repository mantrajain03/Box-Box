#ifndef BOXBOX_SEGMENT_TREE_HPP
#define BOXBOX_SEGMENT_TREE_HPP

#include <cstddef>
#include <vector>

namespace boxbox {

/// Hand-written segment tree answering range sum and range minimum over a
/// fixed-length array of doubles, with point updates.
///
/// Ranges are HALF-OPEN, [begin, end): rangeSum(b, e) adds values[b] up to
/// values[e-1]. rangeSum(b, b) is an empty range and returns 0.0; rangeMin of
/// an empty range has no answer and throws.
///
/// Layout: iterative and bottom-up, in 2n slots with no power-of-two padding.
/// Leaves sit at n .. 2n-1 and node i combines children 2i and 2i+1. That is
/// valid for any n because sum and min are both associative and commutative.
///
/// Minimum ties go to the smaller index, so a query always returns the same
/// element a left-to-right scan would (NFR-5).
///
/// Where it is used: TireRangeAnalyzer, for range queries over one compound's
/// lap times by tire age, including after what-if point overrides. It does
/// NOT replace PitStrategy's prefix-sum table, which stays the source of
/// static stint costs.
///
/// COSTS, n = number of values (NFR-2):
///   build          O(n) time - each internal node is combined once
///   rangeSum       O(log n)  - at most two nodes per level
///   rangeMin       O(log n)
///   rangeMinIndex  O(log n)
///   update         O(log n)  - one ancestor per level
///   valueAt        O(1)
///   storage        O(n): 2n doubles for sums plus 2n (value, index) pairs
///                  for minimums, about 48 bytes per value on a 64-bit build
class SegmentTree {
public:
    SegmentTree() = default;

    /// \throws std::invalid_argument if any value is not finite.
    explicit SegmentTree(const std::vector<double>& values);

    /// Replace the whole contents. O(n).
    /// \throws std::invalid_argument if any value is not finite; the tree is
    ///         left unchanged.
    void build(const std::vector<double>& values);

    std::size_t size() const noexcept { return n_; }
    bool empty() const noexcept { return n_ == 0U; }

    /// Sum over [begin, end). O(log n).
    /// \throws std::out_of_range if begin > end or end > size().
    double rangeSum(std::size_t begin, std::size_t end) const;

    /// Minimum over [begin, end). O(log n).
    /// \throws std::out_of_range on bad bounds.
    /// \throws std::invalid_argument if the range is empty.
    double rangeMin(std::size_t begin, std::size_t end) const;

    /// Index of the minimum over [begin, end), smallest index on ties.
    /// O(log n). Throws exactly as rangeMin does.
    std::size_t rangeMinIndex(std::size_t begin, std::size_t end) const;

    /// O(1). \throws std::out_of_range if index >= size().
    double valueAt(std::size_t index) const;

    /// Set values[index] = value. O(log n).
    /// \throws std::out_of_range if index >= size().
    /// \throws std::invalid_argument if value is not finite.
    /// A rejected update leaves the tree unchanged.
    void update(std::size_t index, double value);

private:
    struct MinEntry {
        double value;
        std::size_t index;
    };

    static MinEntry better(const MinEntry& a, const MinEntry& b) noexcept;
    void checkRange(std::size_t begin, std::size_t end) const;
    MinEntry queryMin(std::size_t begin, std::size_t end) const;

    std::size_t n_ = 0;
    std::vector<double> sum_;
    std::vector<MinEntry> min_;
};

}  // namespace boxbox

#endif  // BOXBOX_SEGMENT_TREE_HPP
