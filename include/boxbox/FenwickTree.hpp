#ifndef BOXBOX_FENWICK_TREE_HPP
#define BOXBOX_FENWICK_TREE_HPP

#include <cstddef>
#include <vector>

namespace boxbox {

/// Hand-written Fenwick tree (binary indexed tree): point updates with prefix
/// and range sums over doubles.
///
/// Public indices are 0-based. Internally the tree is 1-based, which is what
/// lets i & -i pick out the block each node covers.
///
/// Ranges follow SegmentTree: HALF-OPEN, [begin, end).
///   prefixSum(k)   = values[0] + ... + values[k-1]; prefixSum(0) is 0.0
///   rangeSum(b, e) = prefixSum(e) - prefixSum(b);    rangeSum(b, b) is 0.0
///
/// Where it is used: countOrderSwaps (RaceLapAnalytics) counts how many pairs
/// of cars swapped places between two laps, which is an inversion count: walk
/// one order, and for each car ask how many cars already seen started ahead
/// of it. That is one point update and one prefix sum per car, O(D log D) in
/// total. A SegmentTree could do the same in the same bound; the Fenwick tree
/// is shorter and needs no minimum, which this job never asks for.
///
/// The operations are the ones the plan (sec. 4.2C) lists: point update,
/// prefix sum, range sum, and a build. Nothing else is offered, so the tree
/// stores only its n + 1 nodes and no copy of the values.
///
/// COSTS, n = number of slots (NFR-2):
///   construct from values  O(n) - linear build, each node pushes once into
///                                 its parent
///   construct n zeros      O(n)
///   add                    O(log n)
///   prefixSum, rangeSum    O(log n) - rangeSum is two prefix sums
///   storage                O(n): n + 1 doubles, about 8 bytes per value
class FenwickTree {
public:
    FenwickTree() = default;

    /// n slots, all 0.0. O(n).
    explicit FenwickTree(std::size_t size);

    /// \throws std::invalid_argument if any value is not finite.
    explicit FenwickTree(const std::vector<double>& values);

    /// Number of slots. O(1).
    std::size_t size() const noexcept { return tree_.empty() ? 0U : tree_.size() - 1U; }
    bool empty() const noexcept { return size() == 0U; }

    /// values[index] += delta. O(log n).
    /// \throws std::out_of_range if index >= size().
    /// \throws std::invalid_argument if delta is not finite.
    /// A rejected call changes nothing.
    void add(std::size_t index, double delta);

    /// Sum of the first `count` values; count may equal size(). O(log n).
    /// \throws std::out_of_range if count > size().
    double prefixSum(std::size_t count) const;

    /// Sum over [begin, end). O(log n).
    /// \throws std::out_of_range if begin > end or end > size().
    double rangeSum(std::size_t begin, std::size_t end) const;

private:
    std::vector<double> tree_;  ///< 1-based; tree_[0] is unused. Empty when default-built.
};

}  // namespace boxbox

#endif  // BOXBOX_FENWICK_TREE_HPP
