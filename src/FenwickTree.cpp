#include "boxbox/FenwickTree.hpp"

#include <cmath>
#include <stdexcept>
#include <string>

namespace boxbox {

namespace {

/// Lowest set bit of i, e.g. 12 (1100b) -> 4 (100b). Written as i & (~i + 1)
/// rather than i & -i because i is unsigned.
inline std::size_t lowBit(std::size_t i) noexcept {
    return i & (~i + 1U);
}

void requireFinite(double value) {
    if (!std::isfinite(value)) {
        throw std::invalid_argument("FenwickTree: values must be finite numbers");
    }
}

}  // namespace

FenwickTree::FenwickTree(std::size_t size) : tree_(size + 1U, 0.0) {}

// O(n). Node i covers (i - lowBit(i), i]. Visiting i in increasing order, node
// i is complete by the time it is read, and adding it into its direct parent
// i + lowBit(i) is the only push it needs. One pass therefore builds the tree,
// against O(n log n) for n separate add() calls.
FenwickTree::FenwickTree(const std::vector<double>& values) {
    for (const double value : values) {
        requireFinite(value);
    }

    const std::size_t n = values.size();
    tree_.assign(n + 1U, 0.0);
    for (std::size_t i = 1; i <= n; ++i) {
        tree_[i] = values[i - 1U];
    }
    for (std::size_t i = 1; i <= n; ++i) {
        const std::size_t parent = i + lowBit(i);
        if (parent <= n) {
            tree_[parent] += tree_[i];
        }
    }
}

// O(log n): each step moves to the next node whose block covers index. Both
// checks run before anything is written.
void FenwickTree::add(std::size_t index, double delta) {
    if (index >= size()) {
        throw std::out_of_range("FenwickTree: index " + std::to_string(index) +
                                " out of range for size " + std::to_string(size()));
    }
    requireFinite(delta);
    for (std::size_t i = index + 1U; i < tree_.size(); i += lowBit(i)) {
        tree_[i] += delta;
    }
}

// O(log n): each step strips the lowest set bit.
double FenwickTree::prefixSum(std::size_t count) const {
    if (count > size()) {
        throw std::out_of_range("FenwickTree: prefix length " + std::to_string(count) +
                                " exceeds size " + std::to_string(size()));
    }
    double total = 0.0;
    for (std::size_t i = count; i > 0U; i -= lowBit(i)) {
        total += tree_[i];
    }
    return total;
}

double FenwickTree::rangeSum(std::size_t begin, std::size_t end) const {
    if (begin > end || end > size()) {
        throw std::out_of_range("FenwickTree: range [" + std::to_string(begin) + ", " +
                                std::to_string(end) + ") is outside [0, " +
                                std::to_string(size()) + ")");
    }
    return prefixSum(end) - prefixSum(begin);
}

}  // namespace boxbox
