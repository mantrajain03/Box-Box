#include "boxbox/RaceLapAnalytics.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <utility>

#include "boxbox/FenwickTree.hpp"

namespace boxbox {

// ---------------------------------------------------------------------------
// LapTimeIndex
// ---------------------------------------------------------------------------

// O(N): a tree of zeros, one leaf per race lap.
LapTimeIndex::LapTimeIndex(int raceLaps) {
    if (raceLaps < 0) {
        throw std::invalid_argument("LapTimeIndex: raceLaps must not be negative");
    }
    tree_.build(std::vector<double>(static_cast<std::size_t>(raceLaps), 0.0));
}

// O(log N): one point update. Both checks run before anything is written.
void LapTimeIndex::recordLap(int lap, double seconds) {
    if (lap != recorded_ + 1 || lap > raceLaps()) {
        throw std::invalid_argument("LapTimeIndex: expected lap " + std::to_string(recorded_ + 1) +
                                    " of " + std::to_string(raceLaps()) + ", got lap " +
                                    std::to_string(lap));
    }
    if (!std::isfinite(seconds)) {
        throw std::invalid_argument("LapTimeIndex: lap time must be a finite number");
    }
    tree_.update(static_cast<std::size_t>(lap - 1), seconds);
    ++recorded_;
}

void LapTimeIndex::checkWindow(int firstLap, int lastLap) const {
    if (firstLap < 1 || firstLap > lastLap || lastLap > recorded_) {
        throw std::out_of_range("LapTimeIndex: laps " + std::to_string(firstLap) + " to " +
                                std::to_string(lastLap) + " are not a window of the " +
                                std::to_string(recorded_) + " recorded laps");
    }
}

double LapTimeIndex::lapTime(int lap) const {
    checkWindow(lap, lap);
    return tree_.valueAt(static_cast<std::size_t>(lap - 1));
}

// O(log N). Inclusive laps become the tree's half-open [first-1, last).
double LapTimeIndex::totalTime(int firstLap, int lastLap) const {
    checkWindow(firstLap, lastLap);
    return tree_.rangeSum(static_cast<std::size_t>(firstLap - 1),
                          static_cast<std::size_t>(lastLap));
}

// O(log N). SegmentTree breaks ties toward the smaller index, which is the
// lower lap number.
LapTimeIndex::Lap LapTimeIndex::fastestLap(int firstLap, int lastLap) const {
    checkWindow(firstLap, lastLap);
    const std::size_t index = tree_.rangeMinIndex(static_cast<std::size_t>(firstLap - 1),
                                                  static_cast<std::size_t>(lastLap));
    return Lap{static_cast<int>(index) + 1, tree_.valueAt(index)};
}

// ---------------------------------------------------------------------------
// RaceLapRecorder
// ---------------------------------------------------------------------------

RaceLapRecorder::RaceLapRecorder(const RaceSimulator& simulator) {
    for (const Driver& driver : simulator.drivers()) {
        if (driver.lapsCompleted() != 0) {
            throw std::invalid_argument(
                "RaceLapRecorder: must be created before any lap is run");
        }
    }
    laps_.reserve(simulator.drivers().size());
    for (std::size_t i = 0; i < simulator.drivers().size(); ++i) {
        laps_.emplace_back(simulator.track().totalLaps());
    }
    orders_.push_back(boxbox::runningOrder(simulator));  // the grid
}

void RaceLapRecorder::observe(const RaceSimulator& simulator) {
    const std::vector<Driver>& drivers = simulator.drivers();
    if (drivers.size() != laps_.size()) {
        throw std::invalid_argument("RaceLapRecorder: the field changed size");
    }
    // Validate every car before recording any, so a rejected call changes nothing.
    for (std::size_t i = 0; i < drivers.size(); ++i) {
        const int done = drivers[i].lapsCompleted();
        const int seen = laps_[i].recordedLaps();
        if (done < seen || done > seen + 1) {
            throw std::invalid_argument("RaceLapRecorder: car " +
                                        std::to_string(drivers[i].number()) + " is on lap " +
                                        std::to_string(done) + " but " + std::to_string(seen) +
                                        " were recorded; call observe() after every lap");
        }
    }
    for (std::size_t i = 0; i < drivers.size(); ++i) {
        if (drivers[i].lapsCompleted() == laps_[i].recordedLaps() + 1) {
            laps_[i].recordLap(drivers[i].lapsCompleted(), drivers[i].lastLapTime());
        }
    }
    orders_.push_back(boxbox::runningOrder(simulator));
}

const LapTimeIndex& RaceLapRecorder::carLaps(std::size_t driverIndex) const {
    if (driverIndex >= laps_.size()) {
        throw std::out_of_range("RaceLapRecorder: no car at index " + std::to_string(driverIndex));
    }
    return laps_[driverIndex];
}

const std::vector<int>& RaceLapRecorder::runningOrder(int lap) const {
    if (lap < 0 || lap > lapsObserved()) {
        throw std::out_of_range("RaceLapRecorder: no running order after lap " +
                                std::to_string(lap) + "; " + std::to_string(lapsObserved()) +
                                " laps observed");
    }
    return orders_[static_cast<std::size_t>(lap)];
}

std::int64_t RaceLapRecorder::swapsBetween(int lapA, int lapB) const {
    return countOrderSwaps(runningOrder(lapA), runningOrder(lapB));
}

// ---------------------------------------------------------------------------
// Free functions
// ---------------------------------------------------------------------------

// O(D log D), the cost of RaceSimulator::standings.
std::vector<int> runningOrder(const RaceSimulator& simulator) {
    std::vector<int> order;
    order.reserve(simulator.drivers().size());
    for (const Standing& row : simulator.standings()) {
        order.push_back(simulator.drivers()[row.driverIndex].number());
    }
    return order;
}

// O(D log D), guaranteed. Number each car by its position in `before`, then
// read those positions in `after` order. A pair swapped places exactly when a
// car is read after a car that started behind it, so the answer is the number
// of inversions in that sequence. Walking it left to right, the tree counts
// how many cars already read started at or ahead of the current one; everyone
// else already read started behind it.
//
// Start positions are found by binary search in a list sorted by car number,
// not a hash map, so no lookup can degrade: sorting is O(D log D) and each of
// the D lookups is O(log D).
std::int64_t countOrderSwaps(const std::vector<int>& before, const std::vector<int>& after) {
    if (before.size() != after.size()) {
        throw std::invalid_argument("countOrderSwaps: the two orders have different lengths");
    }

    // (car number, start position), sorted by car number. O(D log D).
    std::vector<std::pair<int, std::size_t>> startPosition;
    startPosition.reserve(before.size());
    for (std::size_t i = 0; i < before.size(); ++i) {
        startPosition.emplace_back(before[i], i);
    }
    std::sort(startPosition.begin(), startPosition.end());
    for (std::size_t i = 1; i < startPosition.size(); ++i) {
        if (startPosition[i].first == startPosition[i - 1U].first) {
            throw std::invalid_argument("countOrderSwaps: car " +
                                        std::to_string(startPosition[i].first) +
                                        " appears twice in the first order");
        }
    }

    FenwickTree seen(before.size());
    std::vector<bool> used(before.size(), false);
    std::int64_t swaps = 0;
    for (std::size_t k = 0; k < after.size(); ++k) {
        // O(log D): the first entry whose car number is not less than after[k].
        const auto it = std::lower_bound(
            startPosition.begin(), startPosition.end(), after[k],
            [](const std::pair<int, std::size_t>& entry, int car) { return entry.first < car; });
        if (it == startPosition.end() || it->first != after[k]) {
            throw std::invalid_argument("countOrderSwaps: car " + std::to_string(after[k]) +
                                        " is not in the first order");
        }
        const std::size_t pos = it->second;
        if (used[pos]) {
            throw std::invalid_argument("countOrderSwaps: car " + std::to_string(after[k]) +
                                        " appears twice in the second order");
        }
        used[pos] = true;

        // k cars read so far; those at or ahead of pos did not swap with this one.
        // Slot pos itself is still empty here (this car is added just below).
        const auto aheadOrEqual = static_cast<std::int64_t>(seen.prefixSum(pos + 1U));
        swaps += static_cast<std::int64_t>(k) - aheadOrEqual;
        seen.add(pos, 1.0);
    }
    return swaps;
}

}  // namespace boxbox
