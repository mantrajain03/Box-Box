#ifndef BOXBOX_PITANALYTICS_HPP
#define BOXBOX_PITANALYTICS_HPP

#include <string>
#include <vector>
#include "boxbox/EventLog.hpp"
#include "boxbox/PitStrategy.hpp"

/// Pit-stop and stint analytics, computed from a completed EventLog
/// (owned by Pratyaksh).
///
/// PitAnalytics is a pure reader: it never mutates the log it is given and
/// never drives a simulation itself. It consumes the canonical event record
/// that RaceSimulator (via the optional event sink) and EventLog already
/// produced, so these numbers are a summary of what actually happened, not a
/// second, independent calculation that could silently disagree with it.
namespace boxbox {

/// One driver's stop-and-stint summary, derived entirely from its
/// PitEntry/CompoundChange/PitExit events in the log.
struct DriverPitSummary {
    int driverNumber = 0;
    int stopCount = 0;
    std::vector<int> stintLengthsLaps;   ///< laps run on each successive set
    std::vector<int> compoundIndicesUsed;///< one entry per stint, in order
    std::vector<int> stopLaps;           ///< the lap each PitEntry occurred on
};

/// A simplified undercut/overcut comparison between two candidate pit laps
/// for the same driver, evaluated with PitStrategy's own plan evaluator so
/// the comparison uses the project's one canonical cost model.
struct UndercutOvercutResult {
    int earlierStopLap = 0;
    int laterStopLap = 0;
    double earlierPlanTotalTime = 0.0;
    double laterPlanTotalTime = 0.0;
    /// Positive means the earlier stop (the "undercut") is faster.
    double timeDeltaSeconds = 0.0;
};

class PitAnalytics {
public:
    /// \param log a completed race's event log. Not copied internally beyond
    ///        what each query needs; PitAnalytics does not take ownership.
    explicit PitAnalytics(const EventLog& log) : log_(log) {}

    /// Per-driver stop count, stint lengths, compounds used, and stop laps.
    /// O(n) over the log's entries for n total events.
    DriverPitSummary summaryFor(int driverNumber) const;

    /// Summaries for every distinct driver number seen in the log, in first-
    /// appearance order. O(n).
    std::vector<DriverPitSummary> allSummaries() const;

    /// Total pit loss charged to `driverNumber`, i.e. (stopCount) * pitLoss,
    /// using the same pitLoss value the race was run with. O(stops).
    double totalPitLoss(int driverNumber, double pitLoss) const;

    /// The inclusive [earliestStopLap, latestStopLap] range across every
    /// driver's recorded stops. Returns {0, 0} if the log has no stops.
    /// O(n).
    struct PitWindow {
        int earliestStopLap = 0;
        int latestStopLap = 0;
        bool hasStops = false;
    };
    PitWindow pitWindow() const;

        /// Simplified undercut/overcut: evaluates the SAME stint plan twice,
    /// once stopping at `earlierLap` and once at `laterLap`, via
    /// PitStrategy::evaluatePlan, and reports the time difference. This
    /// reuses the canonical lap-time model rather than re-deriving it, per
    /// the plan's instruction to use "the existing plan evaluator."
    ///
    /// `baselinePlan` must already reflect a stop at `earlierLap`: this
    /// function does not construct plans itself, only compares two the
    /// caller supplies. `laterPlan` must be the same race shifted so its
    /// first stint instead ends at `laterLap`.
    static UndercutOvercutResult compareUndercutOvercut(
        const PitStrategy& strategy, const std::vector<PlannedStint>& earlierPlan,
        const std::vector<PlannedStint>& laterPlan, int earlierLap, int laterLap);

private:
    const EventLog& log_;
};

}  // namespace boxbox

#endif  // BOXBOX_PITANALYTICS_HPP