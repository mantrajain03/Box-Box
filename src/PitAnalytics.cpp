#include "boxbox/PitAnalytics.hpp"

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace boxbox {

namespace {

// One driver's running stint bookkeeping. Events for a driver arrive in log
// order, which is timestamp order, so a single forward pass is enough.
//
// Ordering subtlety: at a stop, PitEntry and the LapCompleted of the stint's
// last lap share one timestamp and PitEntry sorts first. So the stint that a
// PitEntry closes is delimited by PitEntry's own `lap`, and a LapCompleted
// only opens a new stint when its lap is past the current stint's start.
struct StintTracker {
    DriverPitSummary summary;
    int stintStartLap = 0;   // lap number the current stint began after
    bool stintOpen = false;  // has a lap of the current stint been seen?
    int lastLap = 0;         // highest lap completed so far
};

void consume(StintTracker& t, const RaceEvent& event) {
    switch (event.type) {
        case RaceEventType::LapCompleted:
            if (!t.stintOpen && event.lap > t.stintStartLap) {
                // oldCompoundIndex is the compound this lap was run on.
                t.summary.compoundIndicesUsed.push_back(event.oldCompoundIndex);
                t.stintOpen = true;
            }
            t.lastLap = std::max(t.lastLap, event.lap);
            break;

        case RaceEventType::PitEntry:
            ++t.summary.stopCount;
            t.summary.stopLaps.push_back(event.lap);
            t.summary.stintLengthsLaps.push_back(event.lap - t.stintStartLap);
            t.stintStartLap = event.lap;
            t.stintOpen = false;
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
