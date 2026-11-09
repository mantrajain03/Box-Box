#include "boxbox/PitAnalytics.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace boxbox {

namespace {

// One driver's running stint bookkeeping. Events for a driver arrive in log
// order, which is timestamp order, so a single forward pass is enough.
//
// Ordering subtlety: at a stop, PitEntry and the LapCompleted of the stint's
// last lap share one timestamp and PitEntry sorts first. So the stint that a
// PitEntry closes is delimited by PitEntry's own `lap`, and by the time that
// lap's LapCompleted arrives the stop is already counted. A lap therefore
// belongs to the stint numbered by how many stops were made BEFORE it (a stop
// at the end of lap L is before lap L + 1 but not before lap L). This also
// holds for a one-lap stint, whose only LapCompleted arrives after the PitEntry
// that closes it.
struct StintTracker {
    DriverPitSummary summary;
    int stintStartLap = 0;   // lap number the current stint began after
    int lastLap = 0;         // highest lap completed so far
};

void consume(StintTracker& t, const RaceEvent& event) {
    switch (event.type) {
        case RaceEventType::LapCompleted: {
            // Stops are logged in lap order, so the stops before this lap are
            // the leading stopLaps that are < lap.
            const auto stint = static_cast<std::size_t>(
                std::lower_bound(t.summary.stopLaps.begin(), t.summary.stopLaps.end(), event.lap) -
                t.summary.stopLaps.begin());
            std::vector<int>& used = t.summary.compoundIndicesUsed;
            if (used.size() <= stint) {
                used.resize(stint + 1U, -1);   // -1: no lap of an earlier stint was logged
            }
            if (used[stint] < 0) {
                used[stint] = event.oldCompoundIndex;   // the compound this lap was run on
            }
            t.lastLap = std::max(t.lastLap, event.lap);
            break;
        }

        case RaceEventType::PitEntry:
            ++t.summary.stopCount;
            t.summary.stopLaps.push_back(event.lap);
            t.summary.stintLengthsLaps.push_back(event.lap - t.stintStartLap);
            t.stintStartLap = event.lap;
            break;

        default:
            break;
    }
}

// The final stint has no closing PitEntry; it runs to the last lap seen.
DriverPitSummary finish(StintTracker& t) {
    if (t.lastLap > t.stintStartLap) {
        t.summary.stintLengthsLaps.push_back(t.lastLap - t.stintStartLap);
    }
    return std::move(t.summary);
}

}  // namespace

DriverPitSummary PitAnalytics::summaryFor(int driverNumber) const {
    StintTracker tracker;
    tracker.summary.driverNumber = driverNumber;
    for (const RaceEvent& event : log_.entries()) {
        if (event.driverNumber == driverNumber) {
            consume(tracker, event);
        }
    }
    return finish(tracker);
}

// One pass over the log. A hash map from driver number to tracker slot gives
// O(1) expected lookup per event, so the whole call is O(n) expected for n
// events, instead of one full pass per driver.
std::vector<DriverPitSummary> PitAnalytics::allSummaries() const {
    std::unordered_map<int, std::size_t> slotOf;  // never iterated: order stays deterministic
    std::vector<StintTracker> trackers;           // first-appearance order

    for (const RaceEvent& event : log_.entries()) {
        auto found = slotOf.find(event.driverNumber);
        if (found == slotOf.end()) {
            found = slotOf.emplace(event.driverNumber, trackers.size()).first;
            trackers.emplace_back();
            trackers.back().summary.driverNumber = event.driverNumber;
        }
        consume(trackers[found->second], event);
    }

    std::vector<DriverPitSummary> summaries;
    summaries.reserve(trackers.size());
    for (StintTracker& tracker : trackers) {
        summaries.push_back(finish(tracker));
    }
    return summaries;
}

double PitAnalytics::totalPitLoss(int driverNumber, double pitLoss) const {
    const DriverPitSummary summary = summaryFor(driverNumber);
    return static_cast<double>(summary.stopCount) * pitLoss;
}

PitAnalytics::PitWindow PitAnalytics::pitWindow() const {
    PitWindow window;

    for (const RaceEvent& event : log_.entries()) {
        if (event.type != RaceEventType::PitEntry) {
            continue;
        }
        if (!window.hasStops) {
            window.earliestStopLap = event.lap;
            window.latestStopLap = event.lap;
            window.hasStops = true;
        } else {
            window.earliestStopLap = std::min(window.earliestStopLap, event.lap);
            window.latestStopLap = std::max(window.latestStopLap, event.lap);
        }
    }

    return window;
}

// ---------------------------------------------------------------------------
// Plan sec. 4.5: stint statistics, chronology, windows, coverage
// ---------------------------------------------------------------------------

std::vector<StintInterval> PitAnalytics::stintIntervals(const DriverPitSummary& summary) {
    std::vector<StintInterval> out;
    out.reserve(summary.stintLengthsLaps.size());
    int next = 1;
    for (const int length : summary.stintLengthsLaps) {
        StintInterval interval;
        interval.firstLap = next;
        interval.lastLap = next + length - 1;
        out.push_back(interval);
        next += length;
    }
    return out;
}

namespace {

StintStats statsOf(const std::vector<int>& lengths) {
    StintStats s;
    if (lengths.empty()) {
        return s;
    }
    s.count = static_cast<int>(lengths.size());
    s.shortestLaps = *std::min_element(lengths.begin(), lengths.end());
    s.longestLaps = *std::max_element(lengths.begin(), lengths.end());
    long long total = 0;
    for (const int length : lengths) {
        total += length;
    }
    s.meanLaps = static_cast<double>(total) / static_cast<double>(lengths.size());
    return s;
}

std::vector<int> allStintLengths(const std::vector<DriverPitSummary>& summaries) {
    std::vector<int> lengths;
    for (const DriverPitSummary& s : summaries) {
        lengths.insert(lengths.end(), s.stintLengthsLaps.begin(), s.stintLengthsLaps.end());
    }
    return lengths;
}

}  // namespace

StintStats PitAnalytics::stintStats(const DriverPitSummary& summary) {
    return statsOf(summary.stintLengthsLaps);
}

StintStats PitAnalytics::fieldStintStats() const {
    return statsOf(allStintLengths(allSummaries()));
}

std::vector<StintHistogramBin> PitAnalytics::stintHistogram(int binWidth) const {
    if (binWidth < 1) {
        throw std::invalid_argument("stintHistogram: binWidth must be at least 1 lap");
    }
    const std::vector<int> lengths = allStintLengths(allSummaries());
    std::vector<StintHistogramBin> bins;
    if (lengths.empty()) {
        return bins;
    }
    const int lowest = *std::min_element(lengths.begin(), lengths.end());
    const int highest = *std::max_element(lengths.begin(), lengths.end());
    const auto binCount = static_cast<std::size_t>((highest - lowest) / binWidth) + 1U;
    bins.resize(binCount);
    for (std::size_t b = 0; b < binCount; ++b) {
        // long long: a huge binWidth must not overflow int arithmetic.
        const long long first = static_cast<long long>(lowest) + static_cast<long long>(b) * binWidth;
        const long long last = first + binWidth - 1;
        bins[b].firstLaps = static_cast<int>(first);   // first <= highest, so it fits
        bins[b].lastLaps = static_cast<int>(std::min<long long>(last, std::numeric_limits<int>::max()));
    }
    for (const int length : lengths) {
        ++bins[static_cast<std::size_t>((length - lowest) / binWidth)].count;
    }
    return bins;
}

double PitAnalytics::fieldPitLoss(double pitLoss) const {
    std::size_t stops = 0;
    for (const RaceEvent& event : log_.entries()) {
        if (event.type == RaceEventType::PitEntry) {
            ++stops;
        }
    }
    return static_cast<double>(stops) * pitLoss;
}

std::vector<PitStopRecord> PitAnalytics::chronologicalPitStops() const {
    std::vector<PitStopRecord> stops;
    std::unordered_map<int, std::size_t> open;  // car -> its latest stop awaiting a compound
    for (const RaceEvent& event : log_.entries()) {
        if (event.type == RaceEventType::PitEntry) {
            PitStopRecord stop;
            stop.driverNumber = event.driverNumber;
            stop.lap = event.lap;
            stop.time = event.timestamp;
            open[event.driverNumber] = stops.size();
            stops.push_back(stop);
        } else if (event.type == RaceEventType::CompoundChange) {
            const auto found = open.find(event.driverNumber);
            if (found != open.end()) {
                stops[found->second].fromCompound = event.oldCompoundIndex;
                stops[found->second].toCompound = event.newCompoundIndex;
                open.erase(found);   // one compound change per stop: a stray one cannot overwrite it
            }
        }
    }
    // Exact comparison on purpose: timestamps come straight from the simulator, and a
    // tolerance is not a valid sort key (it is not transitive).
    std::stable_sort(stops.begin(), stops.end(), [](const PitStopRecord& a, const PitStopRecord& b) {
        if (a.time < b.time) return true;
        if (b.time < a.time) return false;
        if (a.driverNumber != b.driverNumber) return a.driverNumber < b.driverNumber;
        return a.lap < b.lap;
    });
    return stops;
}

int StopLapIndex::cumulativeStops(int lap) const {
    if (lap < 0 || lap > raceLaps_) {
        throw std::out_of_range("StopLapIndex: lap " + std::to_string(lap) + " is outside 0.." +
                                std::to_string(raceLaps_));
    }
    return prefix_.empty() ? 0 : prefix_[static_cast<std::size_t>(lap)];
}

int StopLapIndex::stopsInLaps(int fromLap, int toLap) const {
    if (fromLap < 1 || fromLap > toLap || toLap > raceLaps_) {
        throw std::out_of_range("StopLapIndex: laps " + std::to_string(fromLap) + " to " +
                                std::to_string(toLap) + " are not inside 1.." +
                                std::to_string(raceLaps_));
    }
    return prefix_[static_cast<std::size_t>(toLap)] - prefix_[static_cast<std::size_t>(fromLap - 1)];
}

StopLapIndex PitAnalytics::buildStopIndex(int raceLaps) const {
    if (raceLaps < 0) {
        throw std::invalid_argument("buildStopIndex: raceLaps must not be negative");
    }
    std::vector<int> perLap(static_cast<std::size_t>(raceLaps) + 1U, 0);
    for (const RaceEvent& event : log_.entries()) {
        if (event.type != RaceEventType::PitEntry) {
            continue;
        }
        if (event.lap < 1 || event.lap > raceLaps) {
            throw std::invalid_argument("buildStopIndex: a stop on lap " + std::to_string(event.lap) +
                                        " is outside the " + std::to_string(raceLaps) + "-lap race");
        }
        ++perLap[static_cast<std::size_t>(event.lap)];
    }
    StopLapIndex index;
    index.raceLaps_ = raceLaps;
    index.prefix_.assign(perLap.size(), 0);
    for (std::size_t lap = 1; lap < perLap.size(); ++lap) {
        index.prefix_[lap] = index.prefix_[lap - 1U] + perLap[lap];
    }
    return index;
}

PitWindowSummary PitAnalytics::windowSummary(int fromLap, int toLap, double pitLoss) const {
    if (fromLap < 1 || fromLap > toLap) {
        throw std::invalid_argument("windowSummary: need 1 <= fromLap <= toLap");
    }
    PitWindowSummary w;
    w.fromLap = fromLap;
    w.toLap = toLap;
    std::vector<int> cars;
    for (const RaceEvent& event : log_.entries()) {
        if (event.type != RaceEventType::PitEntry || event.lap < fromLap || event.lap > toLap) {
            continue;
        }
        if (w.stops == 0) {
            w.earliestLap = event.lap;
            w.latestLap = event.lap;
        } else {
            w.earliestLap = std::min(w.earliestLap, event.lap);
            w.latestLap = std::max(w.latestLap, event.lap);
        }
        ++w.stops;
        cars.push_back(event.driverNumber);
    }
    std::sort(cars.begin(), cars.end());
    w.drivers = static_cast<int>(std::unique(cars.begin(), cars.end()) - cars.begin());
    w.pitLossTotal = static_cast<double>(w.stops) * pitLoss;
    return w;
}

StopTimingComparison PitAnalytics::compareStopTiming(int driverA, int driverB) const {
    const DriverPitSummary a = summaryFor(driverA);
    const DriverPitSummary b = summaryFor(driverB);
    StopTimingComparison c;
    c.driverA = driverA;
    c.driverB = driverB;
    const std::size_t shared = std::min(a.stopLaps.size(), b.stopLaps.size());
    for (std::size_t i = 0; i < shared; ++i) {
        StopTimingComparison::Row row;
        row.stopIndex = static_cast<int>(i) + 1;
        row.lapA = a.stopLaps[i];
        row.lapB = b.stopLaps[i];
        row.lapDifference = row.lapA - row.lapB;
        c.rows.push_back(row);
    }
    c.extraStopsA = static_cast<int>(a.stopLaps.size() - shared);
    c.extraStopsB = static_cast<int>(b.stopLaps.size() - shared);
    return c;
}

CoverageReport PitAnalytics::checkStintCoverage(std::vector<StintInterval> stints, int raceLaps) {
    CoverageReport report;
    std::sort(stints.begin(), stints.end(), [](const StintInterval& a, const StintInterval& b) {
        if (a.firstLap != b.firstLap) return a.firstLap < b.firstLap;
        return a.lastLap < b.lastLap;
    });
    int next = 1;  // the lap the next stint must start on
    for (const StintInterval& s : stints) {
        if (s.firstLap < 1 || s.lastLap < s.firstLap) {
            report.message = "stint " + std::to_string(s.firstLap) + "-" + std::to_string(s.lastLap) +
                             " is not a valid lap interval";
            return report;
        }
        if (s.firstLap > next) {
            report.message = "laps " + std::to_string(next) + "-" + std::to_string(s.firstLap - 1) +
                             " are not covered by any stint";
            return report;
        }
        if (s.firstLap < next) {
            report.message = "stint " + std::to_string(s.firstLap) + "-" + std::to_string(s.lastLap) +
                             " overlaps the previous stint, which ends on lap " +
                             std::to_string(next - 1);
            return report;
        }
        next = s.lastLap + 1;
    }
    if (next - 1 < raceLaps) {
        report.message = "laps " + std::to_string(next) + "-" + std::to_string(raceLaps) +
                         " are not covered by any stint";
        return report;
    }
    if (next - 1 > raceLaps) {
        report.message = "the stints run to lap " + std::to_string(next - 1) +
                         ", past the end of the " + std::to_string(raceLaps) + "-lap race";
        return report;
    }
    report.tiles = true;
    return report;
}

std::vector<int> PitAnalytics::minimumStopLaps(std::vector<StopWindow> windows) {
    for (const StopWindow& w : windows) {
        if (w.earliestLap < 1 || w.earliestLap > w.latestLap) {
            throw std::invalid_argument("minimumStopLaps: car " + std::to_string(w.driverNumber) +
                                        " has the invalid window " + std::to_string(w.earliestLap) +
                                        "-" + std::to_string(w.latestLap));
        }
    }
    std::sort(windows.begin(), windows.end(), [](const StopWindow& a, const StopWindow& b) {
        if (a.latestLap != b.latestLap) return a.latestLap < b.latestLap;
        if (a.earliestLap != b.earliestLap) return a.earliestLap < b.earliestLap;
        return a.driverNumber < b.driverNumber;
    });
    std::vector<int> chosen;
    for (const StopWindow& w : windows) {
        // Windows are visited by latest lap, so the last chosen lap can only be too early for
        // this window, never too late: if it is before the window opens, take this one's end.
        if (chosen.empty() || chosen.back() < w.earliestLap) {
            chosen.push_back(w.latestLap);
        }
    }
    return chosen;
}

std::vector<TrafficAfterStop> PitAnalytics::trafficAfterStops(const std::vector<LapRecord>& records,
                                                              int lapsAfter) {
    if (lapsAfter < 1) {
        throw std::invalid_argument("trafficAfterStops: lapsAfter must be at least 1");
    }
    // Group by car (hash map, never iterated), each car's laps in lap order.
    std::unordered_map<int, std::vector<const LapRecord*>> byCar;
    std::vector<int> cars;
    for (const LapRecord& r : records) {
        auto found = byCar.find(r.driverNumber);
        if (found == byCar.end()) {
            found = byCar.emplace(r.driverNumber, std::vector<const LapRecord*>()).first;
            cars.push_back(r.driverNumber);
        }
        found->second.push_back(&r);
    }
    std::sort(cars.begin(), cars.end());

    std::vector<TrafficAfterStop> out;
    for (const int car : cars) {
        std::vector<const LapRecord*>& laps = byCar[car];
        std::sort(laps.begin(), laps.end(),
                  [](const LapRecord* a, const LapRecord* b) { return a->lap < b->lap; });
        std::vector<double> prefix(laps.size() + 1U, 0.0);  // prefix[k] = traffic of laps[0..k-1]
        for (std::size_t i = 0; i < laps.size(); ++i) {
            prefix[i + 1U] = prefix[i] + laps[i]->trafficTime;
        }
        for (std::size_t i = 0; i < laps.size(); ++i) {
            if (!laps[i]->pitStop) {
                continue;
            }
            // long long: lap + lapsAfter must not overflow for a huge lapsAfter.
            const long long lastLap = static_cast<long long>(laps[i]->lap) + lapsAfter;
            const auto end = std::upper_bound(
                laps.begin(), laps.end(), lastLap,
                [](long long limit, const LapRecord* r) { return limit < r->lap; });
            const auto endIndex = static_cast<std::size_t>(end - laps.begin());
            TrafficAfterStop t;
            t.driverNumber = car;
            t.stopLap = laps[i]->lap;
            t.lapsConsidered = static_cast<int>(endIndex - (i + 1U));
            t.trafficSeconds = prefix[endIndex] - prefix[i + 1U];
            out.push_back(t);
        }
    }
    return out;
}

UndercutOvercutResult PitAnalytics::compareUndercutOvercut(
    const PitStrategy& strategy, const std::vector<PlannedStint>& earlierPlan,
    const std::vector<PlannedStint>& laterPlan, int earlierLap, int laterLap) {
    if (earlierLap >= laterLap) {
        throw std::invalid_argument(
            "compareUndercutOvercut: earlierLap must be before laterLap");
    }
    // Each plan must actually stop where it claims to: its first stint ends
    // at that lap. Otherwise the reported laps would not describe the plans.
    if (earlierPlan.empty() || laterPlan.empty() ||
        earlierPlan.front().laps != earlierLap || laterPlan.front().laps != laterLap) {
        throw std::invalid_argument(
            "compareUndercutOvercut: each plan's first stint must end on its stated stop lap");
    }

    const StrategyResult earlierResult = strategy.evaluatePlan(earlierPlan);
    const StrategyResult laterResult = strategy.evaluatePlan(laterPlan);

    UndercutOvercutResult result;
    result.earlierStopLap = earlierLap;
    result.laterStopLap = laterLap;
    result.earlierPlanTotalTime =
        earlierResult.feasible ? earlierResult.totalTime : kUnreachable;
    result.laterPlanTotalTime =
        laterResult.feasible ? laterResult.totalTime : kUnreachable;

    // Positive favours the earlier stop. If either plan is infeasible the
    // delta is meaningless, so it is left as kUnreachable rather than
    // computed from an infinity that would silently propagate.
    if (earlierResult.feasible && laterResult.feasible) {
        result.timeDeltaSeconds =
            result.laterPlanTotalTime - result.earlierPlanTotalTime;
    } else {
        result.timeDeltaSeconds = kUnreachable;
    }

    return result;
}

}  // namespace boxbox
