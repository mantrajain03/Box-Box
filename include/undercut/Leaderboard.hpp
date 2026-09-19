#ifndef UNDERCUT_LEADERBOARD_HPP
#define UNDERCUT_LEADERBOARD_HPP

#include <cstddef>
#include <vector>

namespace undercut {

/// One car's timing line: enough to order it against the rest of the field,
/// plus an opaque index the caller can use to get back to its own record.
struct LeaderboardEntry {
    std::size_t driverIndex = 0;  ///< caller's index; the Leaderboard never reads it
    int driverNumber = 0;
    int lapsCompleted = 0;
    double cumulativeTime = 0.0;
};

/// A hand-written binary min-heap over race entries (FR-2).
///
/// "Min" is by race position: the root is the car that is winning. The heap is
/// a plain std::vector with the usual 2i+1 / 2i+2 child layout, sifted up and
/// down explicitly rather than through std::push_heap, because the point of
/// the exercise is the data structure.
///
/// Knows nothing about Driver, RaceSimulator, Track or tires, so it can be
/// constructed and tested on its own (NFR-3).
///
/// COSTS - stated as what the code actually does (NFR-2):
///   peek          O(1)
///   push          O(log n)
///   pop           O(log n)
///   heapify ctor  O(n)          - not O(n log n); it sifts down from the middle
///   takeTop(k)    O(n + k log n) including the heapify
///   drainOrdered  O(n log n)    - n pops; a full ordering is NOT cheaper
///                                 than sorting, and this comment will not
///                                 pretend otherwise
///
/// The heap's real advantage over a sort is the top of the order: the leader
/// costs O(n) to heapify plus O(1) to read, and the top k costs
/// O(n + k log n). Producing the WHOLE classification is O(n log n) either way.
class Leaderboard {
public:
    Leaderboard() = default;

    /// Build from an unordered set of entries in O(n) by sifting down from the
    /// last internal node, rather than O(n log n) by repeated push.
    explicit Leaderboard(std::vector<LeaderboardEntry> entries);

    /// O(log n).
    void push(LeaderboardEntry entry);

    /// Remove and return the leading entry. O(log n).
    /// \throws std::out_of_range if the heap is empty.
    LeaderboardEntry pop();

    /// The leading entry, left in place. O(1).
    /// \throws std::out_of_range if the heap is empty.
    const LeaderboardEntry& peek() const;

    bool empty() const noexcept { return heap_.empty(); }
    std::size_t size() const noexcept { return heap_.size(); }
    void clear() noexcept { heap_.clear(); }

    /// Pop everything into finishing order, leader first. O(n log n).
    /// Leaves the heap empty.
    std::vector<LeaderboardEntry> drainOrdered();

    /// The leading k entries in order, or all of them if k exceeds the size.
    /// O(k log n). Leaves the heap without those entries.
    std::vector<LeaderboardEntry> takeTop(std::size_t k);

    /// Non-destructive ordered snapshot. O(n) to copy plus O(n log n) to drain.
    std::vector<LeaderboardEntry> ordered() const;

    /// The ordering rule, exposed so a test can check the heap against a
    /// brute-force sort using exactly the same comparison.
    ///
    /// True when `a` is classified ahead of `b`: more laps completed first,
    /// then less elapsed time, then the lower car number. Built only from <,
    /// never from == on a double, and total rather than merely weak - so the
    /// classification can never differ between runs (NFR-5).
    static bool isAhead(const LeaderboardEntry& a, const LeaderboardEntry& b) noexcept;

private:
    void siftUp(std::size_t index) noexcept;
    void siftDown(std::size_t index) noexcept;

    std::vector<LeaderboardEntry> heap_;
};

}  // namespace undercut

#endif  // UNDERCUT_LEADERBOARD_HPP
