#include "boxbox/SegmentTree.hpp"

#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace boxbox {

namespace {

void requireFinite(double value) {
    if (!std::isfinite(value)) {
        throw std::invalid_argument("SegmentTree: values must be finite numbers");
    }
}

}  // namespace

SegmentTree::SegmentTree(const std::vector<double>& values) {
    build(values);
}

// Two < tests and no ==, as in Leaderboard::isAhead. The comparison is exact
// on purpose: the tree must pick the very element a brute-force scan picks,
// and ties fall through to the smaller index.
SegmentTree::MinEntry SegmentTree::better(const MinEntry& a, const MinEntry& b) noexcept {
    if (a.value < b.value) {
        return a;
    }
    if (b.value < a.value) {
        return b;
    }
    return (a.index <= b.index) ? a : b;
}

// O(n). Every value is validated before anything is stored, so a rejected
// build leaves the previous contents intact.
void SegmentTree::build(const std::vector<double>& values) {
    for (const double value : values) {
        requireFinite(value);
    }

    const std::size_t n = values.size();
    const MinEntry sentinel{std::numeric_limits<double>::infinity(),
                            std::numeric_limits<std::size_t>::max()};
    std::vector<double> sum(2U * n, 0.0);
    std::vector<MinEntry> mins(2U * n, sentinel);

    for (std::size_t i = 0; i < n; ++i) {
        sum[n + i] = values[i];
        mins[n + i] = MinEntry{values[i], i};
    }
    // Internal nodes bottom-up; each is combined once, so O(n) in total.
    for (std::size_t i = n; i-- > 1U;) {
        sum[i] = sum[2U * i] + sum[2U * i + 1U];
        mins[i] = better(mins[2U * i], mins[2U * i + 1U]);
    }

    n_ = n;
    sum_ = std::move(sum);
    min_ = std::move(mins);
}

void SegmentTree::checkRange(std::size_t begin, std::size_t end) const {
    if (begin > end || end > n_) {
        throw std::out_of_range("SegmentTree: range [" + std::to_string(begin) + ", " +
                                std::to_string(end) + ") is outside [0, " +
                                std::to_string(n_) + ")");
    }
}

// O(log n). Both ends climb one level per iteration. A boundary that is a
// right child contributes its own node and steps inward, so at most two nodes
// per level are read.
double SegmentTree::rangeSum(std::size_t begin, std::size_t end) const {
    checkRange(begin, end);
    double total = 0.0;
    for (std::size_t lo = begin + n_, hi = end + n_; lo < hi; lo /= 2U, hi /= 2U) {
        if ((lo & 1U) != 0U) {
            total += sum_[lo++];
        }
        if ((hi & 1U) != 0U) {
            total += sum_[--hi];
        }
    }
    return total;
}

// O(log n), same walk as rangeSum.
SegmentTree::MinEntry SegmentTree::queryMin(std::size_t begin, std::size_t end) const {
    checkRange(begin, end);
    if (begin == end) {
        throw std::invalid_argument("SegmentTree: range minimum of an empty range");
    }
    MinEntry best{std::numeric_limits<double>::infinity(),
                  std::numeric_limits<std::size_t>::max()};
    for (std::size_t lo = begin + n_, hi = end + n_; lo < hi; lo /= 2U, hi /= 2U) {
        if ((lo & 1U) != 0U) {
            best = better(best, min_[lo++]);
        }
        if ((hi & 1U) != 0U) {
            best = better(best, min_[--hi]);
        }
    }
    return best;
}

double SegmentTree::rangeMin(std::size_t begin, std::size_t end) const {
    return queryMin(begin, end).value;
}

std::size_t SegmentTree::rangeMinIndex(std::size_t begin, std::size_t end) const {
    return queryMin(begin, end).index;
}

double SegmentTree::valueAt(std::size_t index) const {
    if (index >= n_) {
        throw std::out_of_range("SegmentTree: index " + std::to_string(index) +
                                " out of range for size " + std::to_string(n_));
    }
    return sum_[n_ + index];
}

// O(log n): the leaf, then one ancestor per level up to the root. Both checks
// run before anything is written.
void SegmentTree::update(std::size_t index, double value) {
    if (index >= n_) {
        throw std::out_of_range("SegmentTree: index " + std::to_string(index) +
                                " out of range for size " + std::to_string(n_));
    }
    requireFinite(value);

    std::size_t pos = index + n_;
    sum_[pos] = value;
    min_[pos] = MinEntry{value, index};
    for (pos /= 2U; pos >= 1U; pos /= 2U) {
        sum_[pos] = sum_[2U * pos] + sum_[2U * pos + 1U];
        min_[pos] = better(min_[2U * pos], min_[2U * pos + 1U]);
    }
}

}  // namespace boxbox
