#ifndef BOXBOX_RACE_LAP_ANALYTICS_HPP
#define BOXBOX_RACE_LAP_ANALYTICS_HPP

/// Range queries over simulated race laps, and position-swap counting
/// (tire module, plan sec. 4.2; agreed with Mantra 2026-11-06).
///
/// Everything here reads the simulator from outside, through its public
/// getters, and never changes it. RaceSimulator knows nothing about this file.
///
/// Laps are numbered 1 .. N, as everywhere user-facing (PRD 3.1).

#include <cstddef>
#include <cstdint>
#include <vector>

#include "boxbox/RaceSimulator.hpp"
#include "boxbox/SegmentTree.hpp"

namespace boxbox {

/// One car's lap times by race lap, recorded as the race runs, with range
/// queries over any window of laps recorded so far. Backed by a SegmentTree.
///
/// What a recorded lap time is: Driver::lastLapTime(), i.e. the car's own lap
/// plus any time lost to traffic on that lap (FR-9). Pit loss is NOT in it;
/// the simulator charges that to cumulative time separately.
///
/// Why a segment tree: unlike tire-curve lap times, simulated laps go up and
/// down with pit stops and traffic, so "fastest lap between laps 20 and 35"
/// is a real question, and range minimum needs a tree. Laps arrive one at a
/// time, so each is written with a point update. Range sums come from the
/// same tree; on append-only data they could also be had in O(1) from a
/// running total, so the tree is here for the minimum.
///
/// Ties: the lower lap number wins, compared with plain < and no tolerance,
/// so a query returns exactly the lap a left-to-right scan would.
///
/// COSTS, N = race laps: construct O(N), recordLap O(log N), totalTime and
/// fastestLap O(log N), lapTime O(1); storage O(N).
class LapTimeIndex {
public:
    struct Lap {
        int lap = 0;          ///< race lap number, 1-based
        double seconds = 0.0;
    };

    /// \throws std::invalid_argument if raceLaps < 0.
    explicit LapTimeIndex(int raceLaps);

    int raceLaps() const noexcept { return static_cast<int>(tree_.size()); }
    int recordedLaps() const noexcept { return recorded_; }

    /// Record lap `lap`. Laps must arrive in order, as they do in a race.
    /// \throws std::invalid_argument unless lap == recordedLaps() + 1 and
    ///         lap <= raceLaps(), or if seconds is not finite. A rejected
    ///         call records nothing.
    void recordLap(int lap, double seconds);

    /// \throws std::out_of_range unless 1 <= lap <= recordedLaps().
    double lapTime(int lap) const;

    /// Sum of lap times over laps firstLap .. lastLap, both included.
    /// \throws std::out_of_range unless 1 <= firstLap <= lastLap <= recordedLaps().
    double totalTime(int firstLap, int lastLap) const;

    /// Fastest lap over laps firstLap .. lastLap, lowest lap number on ties.
    /// Throws as totalTime does.
    Lap fastestLap(int firstLap, int lastLap) const;

private:
    void checkWindow(int firstLap, int lastLap) const;

    SegmentTree tree_;
    int recorded_ = 0;
};

/// Records every car's laps, and the running order after every lap, by
/// watching a RaceSimulator from outside. Use it as
///
///     RaceLapRecorder recorder(simulator);
///     while (simulator.advanceLap()) {
///         recorder.observe(simulator);
///     }
///
/// COSTS, D = cars, N = race laps: construct O(D·N + D log D);
/// observe O(D log N + D log D) per lap, because it takes the standings,
/// which RaceSimulator::standings documents as O(D log D) per call.
/// Over a race that is O(N·D log D). Storage O(D·N).
class RaceLapRecorder {
public:
    /// \throws std::invalid_argument if any car has already run a lap: the
    ///         recorder must see the race from the grid.
    explicit RaceLapRecorder(const RaceSimulator& simulator);

    /// Record each car that completed a lap since the last call, and the
    /// running order now.
    /// \throws std::invalid_argument if the field changed size, or a car
    ///         completed more than one lap since the last call (a missed
    ///         observe), or went backwards.
    void observe(const RaceSimulator& simulator);

    std::size_t carCount() const noexcept { return laps_.size(); }

    /// Laps of the car at `driverIndex` (index into RaceSimulator::drivers()).
    /// \throws std::out_of_range for a bad index.
    const LapTimeIndex& carLaps(std::size_t driverIndex) const;

    /// Car numbers in running order, leader first, after lap `lap` of the
    /// race; lap 0 is the grid. \throws std::out_of_range unless
    /// 0 <= lap <= laps observed.
    const std::vector<int>& runningOrder(int lap) const;

    int lapsObserved() const noexcept { return static_cast<int>(orders_.size()) - 1; }

    /// How many pairs of cars swapped places between the running orders
    /// after lapA and after lapB. O(D log D). Throws as runningOrder does.
    std::int64_t swapsBetween(int lapA, int lapB) const;

private:
    std::vector<LapTimeIndex> laps_;
    std::vector<std::vector<int>> orders_;  ///< orders_[k]: after lap k
};

/// Car numbers in running order, leader first, from RaceSimulator::standings.
/// O(D log D).
std::vector<int> runningOrder(const RaceSimulator& simulator);

/// Number of pairs of cars whose relative order differs between two running
/// orders of the same cars: the number of inversions, counted with a
/// FenwickTree. Each car's position in `before` is found by binary search in a
/// list sorted by car number, then `after` is walked front to back, counting
/// for each car how many cars already seen started behind it. No hash map is
/// used, so no lookup can degrade and the bound below has no worst-case caveat.
///
/// Takes plain car numbers, like Leaderboard, so it works on any two
/// snapshots of a field. Identical orders give 0; a fully reversed field of
/// D cars gives D(D-1)/2.
///
/// The tree holds counts as doubles. Every count is a whole number far below
/// 2^53, so the arithmetic is exact.
///
/// COST: O(D log D) time in every case - O(D log D) to sort the start
/// positions, then D lookups and D tree operations at O(log D) each - and O(D)
/// space, against O(D^2) for comparing every pair.
/// \throws std::invalid_argument if the two orders differ in length, contain
///         a duplicate car, or are not the same set of cars.
std::int64_t countOrderSwaps(const std::vector<int>& before, const std::vector<int>& after);

}  // namespace boxbox

#endif  // BOXBOX_RACE_LAP_ANALYTICS_HPP
