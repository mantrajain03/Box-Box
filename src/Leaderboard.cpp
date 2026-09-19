#include "undercut/Leaderboard.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace undercut {

bool Leaderboard::isAhead(const LeaderboardEntry& a, const LeaderboardEntry& b) noexcept {
    if (a.lapsCompleted != b.lapsCompleted) {
        return a.lapsCompleted > b.lapsCompleted;
    }
    // Two < tests rather than one ==; ties fall through to the car number.
    if (a.cumulativeTime < b.cumulativeTime) {
        return true;
    }
    if (b.cumulativeTime < a.cumulativeTime) {
        return false;
    }
    return a.driverNumber < b.driverNumber;
}

// O(n). Sifting down from the last internal node costs sum over levels of
// (nodes at that level * height below it), which telescopes to O(n) - cheaper
// than n successive O(log n) pushes.
Leaderboard::Leaderboard(std::vector<LeaderboardEntry> entries)
    : heap_(std::move(entries)) {
    if (heap_.size() < 2U) {
        return;
    }
    for (std::size_t node = heap_.size() / 2U; node > 0U; --node) {
        siftDown(node - 1U);  // counts down through 0 without unsigned underflow
    }
}

// O(log n): at most one swap per level on the way to the root.
void Leaderboard::siftUp(std::size_t index) noexcept {
    while (index > 0U) {
        const std::size_t parent = (index - 1U) / 2U;
        if (!isAhead(heap_[index], heap_[parent])) {
            break;
        }
        std::swap(heap_[index], heap_[parent]);
        index = parent;
    }
}

// O(log n): at most one swap per level on the way to a leaf.
void Leaderboard::siftDown(std::size_t index) noexcept {
    const std::size_t count = heap_.size();

    for (;;) {
        const std::size_t left = 2U * index + 1U;
        const std::size_t right = left + 1U;
        std::size_t leading = index;

        if (left < count && isAhead(heap_[left], heap_[leading])) {
            leading = left;
        }
        if (right < count && isAhead(heap_[right], heap_[leading])) {
            leading = right;
        }
        if (leading == index) {
            break;
        }
        std::swap(heap_[index], heap_[leading]);
        index = leading;
    }
}

void Leaderboard::push(LeaderboardEntry entry) {
    heap_.push_back(entry);
    siftUp(heap_.size() - 1U);
}

LeaderboardEntry Leaderboard::pop() {
    if (heap_.empty()) {
        throw std::out_of_range("Leaderboard::pop on an empty leaderboard");
    }

    const LeaderboardEntry leader = heap_.front();
    heap_.front() = heap_.back();
    heap_.pop_back();

    if (!heap_.empty()) {
        siftDown(0U);
    }
    return leader;
}

const LeaderboardEntry& Leaderboard::peek() const {
    if (heap_.empty()) {
        throw std::out_of_range("Leaderboard::peek on an empty leaderboard");
    }
    return heap_.front();
}

// O(n log n) - n pops. A full classification costs the same as a sort.
std::vector<LeaderboardEntry> Leaderboard::drainOrdered() {
    std::vector<LeaderboardEntry> order;
    order.reserve(heap_.size());

    while (!heap_.empty()) {
        order.push_back(pop());
    }
    return order;
}

// O(k log n). This is where the heap beats a sort: the podium does not require
// ordering the whole field.
std::vector<LeaderboardEntry> Leaderboard::takeTop(std::size_t k) {
    const std::size_t wanted = std::min(k, heap_.size());

    std::vector<LeaderboardEntry> order;
    order.reserve(wanted);

    for (std::size_t i = 0; i < wanted; ++i) {
        order.push_back(pop());
    }
    return order;
}

std::vector<LeaderboardEntry> Leaderboard::ordered() const {
    Leaderboard copy(*this);
    return copy.drainOrdered();
}

}  // namespace undercut
