#include "boxbox/PitAnalytics.hpp"

#include <algorithm>

namespace boxbox {

DriverPitSummary PitAnalytics::summaryFor(int driverNumber) const {
    DriverPitSummary summary;
    summary.driverNumber = driverNumber;

    int currentStintStartLap = 0;
    bool inStint = false;

    for (const RaceEvent& event : log_.entries()) {
        if (event.driverNumber != driverNumber) {
            continue;
        }

        switch (event.type) {
            case RaceEventType::PitEntry:
                ++summary.stopCount;
                summary.stopLaps.push_back(event.lap);
                if (inStint) {
                    summary.stintLengthsLaps.push_back(event.lap - currentStintStartLap);
                }
                break;

            case RaceEventType::CompoundChange:
                summary.compoundIndicesUsed.push_back(event.newCompoundIndex);
                currentStintStartLap = event.lap;
                inStint = true;
                break;

            default:
                break;
        }
    }

    return summary;
}

std::vector<DriverPitSummary> PitAnalytics::allSummaries() const {
    std::vector<int> seenDrivers;
    for (const RaceEvent& event : log_.entries()) {
        if (std::find(seenDrivers.begin(), seenDrivers.end(), event.driverNumber) ==
            seenDrivers.end()) {
            seenDrivers.push_back(event.driverNumber);
        }
    }

    std::vector<DriverPitSummary> summaries;
    summaries.reserve(seenDrivers.size());
    for (int driverNumber : seenDrivers) {
        summaries.push_back(summaryFor(driverNumber));
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