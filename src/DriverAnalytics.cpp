#include "boxbox/DriverAnalytics.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <utility>

#include "boxbox/Common.hpp"
#include "boxbox/MonotonicDeque.hpp"

namespace boxbox {

namespace {

std::string carAndLap(const LapRecord& r) {
    return "car " + std::to_string(r.driverNumber) + ", lap " + std::to_string(r.lap);
}

/// Throws unless `r` can follow `previous`, the same car's last record (null
/// before its first lap). O(1).
void checkRecord(const LapRecord& r, const LapRecord* previous, std::size_t fieldSize) {
    const std::string where = "DriverAnalytics: " + carAndLap(r) + ": ";

    const int expectedLap = (previous == nullptr) ? 1 : previous->lap + 1;
    if (r.lap != expectedLap) {
        throw std::invalid_argument(where + "expected lap " + std::to_string(expectedLap) +
                                    "; each car's laps must run 1, 2, 3, ... in order");
    }
    if (!std::isfinite(r.lapTime)) {
        throw std::invalid_argument(where + "the lap time is not a finite number");
    }
    if (!std::isfinite(r.trafficTime) || r.trafficTime < 0.0) {
        throw std::invalid_argument(where + "traffic time must be finite and not negative");
    }
    // The lap time includes traffic (LapRecord.hpp), so traffic cannot exceed
    // it. The tolerance absorbs the rounding of a difference of running totals.
    if (r.trafficTime > r.lapTime + kTimeEpsilon) {
        throw std::invalid_argument(where + "traffic time exceeds the lap time that includes it");
    }
    if (!std::isfinite(r.pitTime) || r.pitTime < 0.0) {
        throw std::invalid_argument(where + "pit time must be finite and not negative");
    }
    if (!r.pitStop && r.pitTime > 0.0) {
        throw std::invalid_argument(where + "pit time charged on a lap without a stop");
    }
    if (r.tireAge < 0) {
        throw std::invalid_argument(where + "tire age must not be negative");
    }

    const int expectedStint =
        (previous == nullptr) ? 0 : previous->stint + (previous->pitStop ? 1 : 0);
    if (r.stint != expectedStint) {
        throw std::invalid_argument(where + "expected stint " + std::to_string(expectedStint) +
                                    "; a stint starts at 0 and moves on exactly after a stop");
    }
    if (previous != nullptr && r.stint == previous->stint &&
        r.compoundIndex != previous->compoundIndex) {
        throw std::invalid_argument(where + "the compound changed within a stint");
    }
    if (r.position < 1 || static_cast<std::size_t>(r.position) > fieldSize) {
        throw std::invalid_argument(where + "position " + std::to_string(r.position) +
                                    " is outside 1 .. " + std::to_string(fieldSize));
    }
}

/// Fold one checked lap into its car's running summary. O(1) amortised.
void addLap(DriverSummary& s, const LapRecord& r) {
    ++s.laps;

    // The simulator's charging order, the lap and then the stop at its end,
    // so the total reproduces Driver::cumulativeTime() bit for bit. Adding
    // 0.0 on a lap without a stop changes nothing.
    s.totalTime += r.lapTime;
    s.totalTime += r.pitTime;
    s.greenTime += r.lapTime;
    s.pitTime += r.pitTime;
    s.trafficTime += r.trafficTime;
    if (r.pitStop) {
        ++s.stops;
    }

    // Exact comparisons: a later lap replaces the pick only when strictly
    // faster (slower), so ties keep the earliest lap.
    if (s.laps == 1 || r.lapTime < s.bestLap.lapTime) {
        s.bestLap = r;
    }
    if (s.laps == 1 || s.worstLap.lapTime < r.lapTime) {
        s.worstLap = r;
    }

    s.positions.push_back(r.position);
    if (s.laps == 1 || r.position < s.bestPosition) {
        s.bestPosition = r.position;
    }
    if (s.laps == 1 || r.position > s.worstPosition) {
        s.worstPosition = r.position;
    }

    if (s.stints.empty() || s.stints.back().stint != r.stint) {
        StintSummary stint;
        stint.stint = r.stint;
        stint.compoundIndex = r.compoundIndex;
        stint.firstLap = r.lap;
        s.stints.push_back(stint);
    }
    StintSummary& stint = s.stints.back();
    stint.lastLap = r.lap;
    ++stint.laps;
    stint.time += r.lapTime;
}

/// The statistics that need every lap first. O(n + S log S) for n laps and
/// S stints.
void finishSummary(DriverSummary& s, const std::vector<double>& lapTimes) {
    if (s.laps == 0) {
        return;
    }
    const double n = static_cast<double>(s.laps);
    s.averageLap = s.greenTime / n;
    s.lapRange = s.worstLap.lapTime - s.bestLap.lapTime;

    double deviation = 0.0;
    for (const double t : lapTimes) {
        deviation += std::fabs(t - s.averageLap);
    }
    s.meanAbsDeviation = deviation / n;

    // Compound usage: the stints sorted by compound, equal compounds merged.
    std::vector<CompoundUsage> usage;
    usage.reserve(s.stints.size());
    for (const StintSummary& stint : s.stints) {
        usage.push_back(CompoundUsage{stint.compoundIndex, stint.laps});
    }
    std::sort(usage.begin(), usage.end(), [](const CompoundUsage& a, const CompoundUsage& b) {
        return a.compoundIndex < b.compoundIndex;
    });
    for (const CompoundUsage& u : usage) {
        if (!s.compounds.empty() && s.compounds.back().compoundIndex == u.compoundIndex) {
            s.compounds.back().laps += u.laps;
        } else {
            s.compounds.push_back(u);
        }
    }
}

/// A bounded binary heap keeping the k laps that rank first under
/// lapRanksAhead. The root holds the WORST lap kept, so a newcomer only has
/// to beat the root to get in. Hand-written, like Leaderboard, because the
/// structure is the point.
///
/// COSTS: offer O(log k); drainBestFirst O(k log k); storage O(k).
class BoundedLapHeap {
public:
    BoundedLapHeap(std::size_t capacity, LapOrder order) : capacity_(capacity), order_(order) {
        heap_.reserve(capacity_);
    }

    void offer(const LapRecord& lap) {
        if (heap_.size() < capacity_) {
            heap_.push_back(lap);
            siftUp(heap_.size() - 1U);
        } else if (capacity_ > 0U && lapRanksAhead(lap, heap_.front(), order_)) {
            heap_.front() = lap;
            siftDown(0U);
        }
    }

    /// Empty the heap into a list, best lap first. Each pop takes the worst
    /// lap left, so the list fills from the back.
    std::vector<LapRecord> drainBestFirst() {
        std::vector<LapRecord> out(heap_.size());
        for (std::size_t i = heap_.size(); i > 0U; --i) {
            out[i - 1U] = heap_.front();
            heap_.front() = heap_.back();
            heap_.pop_back();
            if (!heap_.empty()) {
                siftDown(0U);
            }
        }
        return out;
    }

private:
    /// True when `a` belongs above `b`: it ranks BEHIND b.
    bool above(const LapRecord& a, const LapRecord& b) const noexcept {
        return lapRanksAhead(b, a, order_);
    }

    void siftUp(std::size_t index) noexcept {
        while (index > 0U) {
            const std::size_t parent = (index - 1U) / 2U;
            if (!above(heap_[index], heap_[parent])) {
                return;
            }
            std::swap(heap_[index], heap_[parent]);
            index = parent;
        }
    }

    void siftDown(std::size_t index) noexcept {
        const std::size_t n = heap_.size();
        while (true) {
            const std::size_t left = 2U * index + 1U;
            const std::size_t right = left + 1U;
            std::size_t top = index;
            if (left < n && above(heap_[left], heap_[top])) {
                top = left;
            }
            if (right < n && above(heap_[right], heap_[top])) {
                top = right;
            }
            if (top == index) {
                return;
            }
            std::swap(heap_[index], heap_[top]);
            index = top;
        }
    }

    std::vector<LapRecord> heap_;
    std::size_t capacity_;
    LapOrder order_;
};

bool passesFilter(const LapRecord& r, const LapFilter& filter) noexcept {
    if (filter.driverNumber && r.driverNumber != *filter.driverNumber) {
        return false;
    }
    if (filter.compoundIndex && r.compoundIndex != *filter.compoundIndex) {
        return false;
    }
    return r.lap >= filter.firstLap && r.lap <= filter.lastLap;
}

}  // namespace

// ---------------------------------------------------------------------------
// LapRecorder
// ---------------------------------------------------------------------------

LapRecorder::CarState LapRecorder::stateOf(const Driver& driver) noexcept {
    CarState state;
    state.laps = driver.lapsCompleted();
    state.compoundIndex = driver.currentCompoundIndex();
    state.tireAge = driver.tireAge();
    state.stint = static_cast<int>(driver.stintIndex());
    state.stops = driver.stopsMade();
    state.trafficLoss = driver.trafficLoss();
    return state;
}

// O(D log D): the duplicate check sorts a copy of the car numbers.
LapRecorder::LapRecorder(const RaceSimulator& simulator) {
    const std::vector<Driver>& drivers = simulator.drivers();
    for (const Driver& driver : drivers) {
        if (driver.lapsCompleted() != 0) {
            throw std::invalid_argument(
                "LapRecorder: must be created on the grid, before any lap is run");
        }
    }

    field_.reserve(drivers.size());
    names_.reserve(drivers.size());
    before_.reserve(drivers.size());
    for (const Driver& driver : drivers) {
        field_.push_back(driver.number());
        names_.push_back(driver.name());
        before_.push_back(stateOf(driver));
    }

    std::vector<int> sorted = field_;
    std::sort(sorted.begin(), sorted.end());
    const auto repeat = std::adjacent_find(sorted.begin(), sorted.end());
    if (repeat != sorted.end()) {
        throw std::invalid_argument("LapRecorder: car number " + std::to_string(*repeat) +
                                    " is used twice; records tell cars apart by number");
    }

    records_.reserve(drivers.size() * static_cast<std::size_t>(simulator.track().totalLaps()));
}

// O(D log D): comparing states is O(D), and the positions take one call to
// RaceSimulator::standings, O(D log D).
void LapRecorder::observe(const RaceSimulator& simulator) {
    const std::vector<Driver>& drivers = simulator.drivers();
    if (drivers.size() != field_.size()) {
        throw std::invalid_argument("LapRecorder: the field changed size");
    }

    // Check every car before recording any, so a rejected call changes nothing.
    bool anyLap = false;
    for (std::size_t i = 0; i < drivers.size(); ++i) {
        if (drivers[i].number() != field_[i]) {
            throw std::invalid_argument("LapRecorder: the field changed");
        }
        const int done = drivers[i].lapsCompleted();
        const int seen = before_[i].laps;
        if (done < seen || done > seen + 1) {
            throw std::invalid_argument("LapRecorder: car " + std::to_string(field_[i]) +
                                        " is on lap " + std::to_string(done) + " but " +
                                        std::to_string(seen) +
                                        " were recorded; call observe() after every lap");
        }
        anyLap = anyLap || done == seen + 1;
    }
    if (!anyLap) {
        return;
    }

    std::vector<int> position(drivers.size(), 0);
    for (const Standing& row : simulator.standings()) {
        position[row.driverIndex] = row.position;
    }

    for (std::size_t i = 0; i < drivers.size(); ++i) {
        const Driver& driver = drivers[i];
        const CarState& was = before_[i];
        if (driver.lapsCompleted() == was.laps + 1) {
            LapRecord r;
            r.driverNumber = driver.number();
            r.lap = driver.lapsCompleted();
            r.lapTime = driver.lastLapTime();
            // trafficLoss() never falls, so the rise is never negative, and it
            // is exactly 0 on a lap without traffic.
            r.trafficTime = driver.trafficLoss() - was.trafficLoss;
            r.pitStop = driver.stopsMade() > was.stops;
            r.pitTime = r.pitStop ? simulator.track().pitLoss() : 0.0;
            r.compoundIndex = was.compoundIndex;
            r.tireAge = was.tireAge;
            r.stint = was.stint;
            r.position = position[i];
            records_.push_back(r);
        }
        before_[i] = stateOf(driver);
    }
}

// ---------------------------------------------------------------------------
// DriverAnalytics
// ---------------------------------------------------------------------------

// O(D + R) average; see the header for the worst case.
DriverAnalytics::DriverAnalytics(const std::vector<int>& field, std::vector<LapRecord> records)
    : records_(std::move(records)) {
    const std::size_t cars = field.size();
    summaries_.resize(cars);
    lapTimes_.resize(cars);
    slot_.reserve(cars);
    for (std::size_t i = 0; i < cars; ++i) {
        if (!slot_.emplace(field[i], i).second) {
            throw std::invalid_argument("DriverAnalytics: car " + std::to_string(field[i]) +
                                        " appears twice in the field");
        }
        summaries_[i].driverNumber = field[i];
    }

    // One pass: find the car's slot, check the record against that car's
    // previous one, then fold it in.
    std::vector<const LapRecord*> previous(cars, nullptr);
    for (const LapRecord& r : records_) {
        const auto found = slot_.find(r.driverNumber);
        if (found == slot_.end()) {
            throw std::invalid_argument("DriverAnalytics: " + carAndLap(r) +
                                        " is for a car outside the field");
        }
        const std::size_t s = found->second;
        checkRecord(r, previous[s], cars);
        addLap(summaries_[s], r);
        lapTimes_[s].push_back(r.lapTime);
        previous[s] = &r;
    }

    for (std::size_t s = 0; s < cars; ++s) {
        finishSummary(summaries_[s], lapTimes_[s]);
    }
}

DriverAnalytics::DriverAnalytics(const LapRecorder& recorder)
    : DriverAnalytics(recorder.field(), recorder.records()) {
    for (std::size_t i = 0; i < summaries_.size(); ++i) {
        summaries_[i].name = recorder.names()[i];
    }
}

std::size_t DriverAnalytics::slotOf(int driverNumber) const {
    const auto found = slot_.find(driverNumber);
    if (found == slot_.end()) {
        throw std::out_of_range("DriverAnalytics: no car numbered " +
                                std::to_string(driverNumber) + " in the field");
    }
    return found->second;
}

const DriverSummary& DriverAnalytics::summary(int driverNumber) const {
    return summaries_[slotOf(driverNumber)];
}

const std::vector<double>& DriverAnalytics::lapTimes(int driverNumber) const {
    return lapTimes_[slotOf(driverNumber)];
}

bool DriverAnalytics::contains(int driverNumber) const {
    return slot_.count(driverNumber) != 0U;
}

// ---------------------------------------------------------------------------
// Fastest and slowest laps
// ---------------------------------------------------------------------------

bool lapRanksAhead(const LapRecord& a, const LapRecord& b, LapOrder order) noexcept {
    // Fastest wants the smaller time first, Slowest the larger.
    const double first = (order == LapOrder::Fastest) ? a.lapTime : b.lapTime;
    const double second = (order == LapOrder::Fastest) ? b.lapTime : a.lapTime;
    if (first < second) {
        return true;
    }
    if (second < first) {
        return false;
    }
    if (a.lap != b.lap) {
        return a.lap < b.lap;
    }
    return a.driverNumber < b.driverNumber;
}

// O(R log k). The heap never holds more laps than there are records, so a
// huge k costs no more memory than the records themselves.
std::vector<LapRecord> selectLaps(const std::vector<LapRecord>& records, std::size_t k,
                                  LapOrder order, const LapFilter& filter) {
    BoundedLapHeap heap(std::min(k, records.size()), order);
    for (const LapRecord& r : records) {
        if (!passesFilter(r, filter)) {
            continue;
        }
        if (!std::isfinite(r.lapTime)) {
            throw std::invalid_argument("selectLaps: " + carAndLap(r) +
                                        " has a lap time that is not a finite number");
        }
        heap.offer(r);
    }
    return heap.drainBestFirst();
}

// ---------------------------------------------------------------------------
// Sliding windows
// ---------------------------------------------------------------------------

// O(n): each lap is pushed once into each deque, and each window's sum is one
// subtraction of two prefix sums.
std::vector<LapWindow> trailingWindows(const std::vector<double>& lapTimes, std::size_t window) {
    if (window == 0U) {
        throw std::invalid_argument("trailingWindows: the window must be at least one lap");
    }
    for (const double t : lapTimes) {
        if (!std::isfinite(t)) {
            throw std::invalid_argument("trailingWindows: lap times must be finite numbers");
        }
    }

    const std::size_t n = lapTimes.size();
    std::vector<LapWindow> windows;
    if (n == 0U) {
        return windows;
    }
    windows.reserve(n);

    // prefix[k] = sum of the first k lap times.
    std::vector<double> prefix(n + 1U, 0.0);
    for (std::size_t i = 0; i < n; ++i) {
        prefix[i + 1U] = prefix[i] + lapTimes[i];
    }

    // No window holds more than n laps, so the deques never need more slots
    // than that, however large W is.
    const std::size_t slots = std::min(window, n);
    MonotonicDeque fastest(slots, MonotonicDeque::Kind::Minimum);
    MonotonicDeque slowest(slots, MonotonicDeque::Kind::Maximum);

    for (std::size_t i = 0; i < n; ++i) {
        fastest.push(lapTimes[i]);
        slowest.push(lapTimes[i]);
        const std::size_t first = (i + 1U > window) ? i + 1U - window : 0U;

        LapWindow w;
        w.firstLap = static_cast<int>(first) + 1;
        w.lastLap = static_cast<int>(i) + 1;
        w.laps = w.lastLap - w.firstLap + 1;
        w.fastest = fastest.extreme();
        w.fastestLap = static_cast<int>(fastest.extremeIndex()) + 1;
        w.slowest = slowest.extreme();
        w.slowestLap = static_cast<int>(slowest.extremeIndex()) + 1;
        w.average = (prefix[i + 1U] - prefix[first]) / static_cast<double>(w.laps);
        windows.push_back(w);
    }
    return windows;
}

// O(n) on top of trailingWindows: one scan over the windows.
WindowSummary summarizeWindows(const std::vector<double>& lapTimes, std::size_t window) {
    const std::vector<LapWindow> windows = trailingWindows(lapTimes, window);

    WindowSummary summary;
    summary.window = window;
    if (windows.empty()) {
        return summary;
    }
    summary.complete = lapTimes.size() >= window;

    // The full-length windows end on laps W .. n. Without one, the last
    // window - the whole race - is the only candidate.
    const std::size_t start = summary.complete ? window - 1U : windows.size() - 1U;
    summary.quickest = windows[start];
    summary.slowest = windows[start];
    summary.widest = windows[start];
    for (std::size_t i = start + 1U; i < windows.size(); ++i) {
        const LapWindow& w = windows[i];
        if (definitelyLess(w.average, summary.quickest.average)) {
            summary.quickest = w;
        }
        if (definitelyLess(summary.slowest.average, w.average)) {
            summary.slowest = w;
        }
        if (definitelyLess(summary.widest.spread(), w.spread())) {
            summary.widest = w;
        }
    }
    summary.dropOff = summary.slowest.average - summary.quickest.average;
    return summary;
}

// ---------------------------------------------------------------------------
// Rankings
// ---------------------------------------------------------------------------

double rankValue(const DriverSummary& summary, RankMetric metric) noexcept {
    switch (metric) {
        case RankMetric::AverageLap:
            return summary.averageLap;
        case RankMetric::BestLap:
            return summary.bestLap.lapTime;
        case RankMetric::LapRange:
            return summary.lapRange;
        case RankMetric::MeanAbsDeviation:
            return summary.meanAbsDeviation;
    }
    return summary.averageLap;  // not reached: every metric is handled above
}

// O(D log D).
std::vector<int> rankDrivers(const std::vector<DriverSummary>& summaries, RankMetric metric) {
    std::vector<const DriverSummary*> order;
    order.reserve(summaries.size());
    for (const DriverSummary& s : summaries) {
        order.push_back(&s);
    }

    std::sort(order.begin(), order.end(),
              [metric](const DriverSummary* a, const DriverSummary* b) {
                  const bool aRan = a->laps > 0;
                  const bool bRan = b->laps > 0;
                  if (aRan != bRan) {
                      return aRan;  // a car with laps ranks ahead of one without
                  }
                  if (aRan) {
                      const double va = rankValue(*a, metric);
                      const double vb = rankValue(*b, metric);
                      if (va < vb) {
                          return true;
                      }
                      if (vb < va) {
                          return false;
                      }
                  }
                  return a->driverNumber < b->driverNumber;
              });

    std::vector<int> numbers;
    numbers.reserve(order.size());
    for (const DriverSummary* s : order) {
        numbers.push_back(s->driverNumber);
    }
    return numbers;
}

}  // namespace boxbox
