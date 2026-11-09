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

    /// The log is held by reference, so a temporary would dangle.
    explicit PitAnalytics(EventLog&&) = delete;

    /// Per-driver stop count, stint lengths, compounds used, and stop laps.
    /// Every stint is reported, including the first and the final one, so the
    /// stint lengths sum to the laps the driver completed.
    /// O(n) over the log's entries for n total events.
    DriverPitSummary summaryFor(int driverNumber) const;

    /// Summaries for every distinct driver number seen in the log, in first-
    /// appearance order. One pass with a hash map from driver number to slot:
    /// O(n) expected for n events (O(n * D) worst case under pathological
    /// hashing), O(D) extra space.
    std::vector<DriverPitSummary> allSummaries() const;

    /// Total pit loss charged to `driverNumber`, i.e. (stopCount) * pitLoss,
    /// using the same pitLoss value the race was run with. O(n) over the log:
    /// it builds this driver's summary first, so it is NOT O(stops).
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

    /// Simplified undercut/overcut: evaluates two stint plans via
    /// PitStrategy::evaluatePlan and reports the time difference. This
    /// reuses the canonical lap-time model rather than re-deriving it, per
    /// the plan's instruction to use "the existing plan evaluator."
    ///
    /// This function does not construct plans, only compares two the caller
    /// supplies. `earlierPlan`'s first stint must end on `earlierLap` and
    /// `laterPlan`'s first stint on `laterLap`; the rest of each plan is
    /// whatever the caller wants to compare. Simplified model: no traffic,
    /// tire warm-up or real pit-window rules.
    /// \throws std::invalid_argument if earlierLap >= laterLap, a plan is
    ///         empty, or a plan's first stint does not end on its stated lap.
    static UndercutOvercutResult compareUndercutOvercut(
        const PitStrategy& strategy, const std::vector<PlannedStint>& earlierPlan,
        const std::vector<PlannedStint>& laterPlan, int earlierLap, int laterLap);

private:
    const EventLog& log_;
};

}  // namespace boxbox

#endif  // BOXBOX_PITANALYTICS_HPP