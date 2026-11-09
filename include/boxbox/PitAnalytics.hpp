#ifndef BOXBOX_PITANALYTICS_HPP
#define BOXBOX_PITANALYTICS_HPP

#include <string>
#include <vector>
#include "boxbox/EventLog.hpp"
#include "boxbox/LapRecord.hpp"
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

/// Inclusive lap interval a stint covers (1-based).
struct StintInterval {
    int firstLap = 0;
    int lastLap = 0;
};

/// Shortest / longest / mean stint over some set of stints. All zero for none.
struct StintStats {
    int count = 0;
    int shortestLaps = 0;
    int longestLaps = 0;
    double meanLaps = 0.0;
};

/// One bar of a stint-length histogram: stints of firstLaps..lastLaps laps.
struct StintHistogramBin {
    int firstLaps = 0;
    int lastLaps = 0;
    int count = 0;
};

/// One pit stop, as logged.
struct PitStopRecord {
    int driverNumber = 0;
    int lap = 0;           ///< the stop is at the END of this lap
    double time = 0.0;     ///< race clock at the stop (pit loss included)
    int fromCompound = -1;
    int toCompound = -1;
};

/// Stops inside a lap interval.
struct PitWindowSummary {
    int fromLap = 0;
    int toLap = 0;
    int stops = 0;
    int drivers = 0;        ///< distinct cars that stopped in the interval
    int earliestLap = 0;    ///< 0 when there are no stops
    int latestLap = 0;
    double pitLossTotal = 0.0;
};

/// A driver's allowed stop window, for the greedy window problem.
struct StopWindow {
    int driverNumber = 0;
    int earliestLap = 0;
    int latestLap = 0;
};

/// Two drivers' stops compared one by one, in the order they were made.
struct StopTimingComparison {
    struct Row {
        int stopIndex = 0;    ///< 1-based: first stop, second stop, ...
        int lapA = 0;
        int lapB = 0;
        int lapDifference = 0;  ///< lapA - lapB; negative means A stopped earlier
    };
    int driverA = 0;
    int driverB = 0;
    std::vector<Row> rows;      ///< one per stop both made
    int extraStopsA = 0;        ///< stops A made beyond B's count
    int extraStopsB = 0;
};

/// Traffic lost in the laps right after a stop.
struct TrafficAfterStop {
    int driverNumber = 0;
    int stopLap = 0;
    int lapsConsidered = 0;     ///< fewer than asked when the race ends first
    double trafficSeconds = 0.0;
};

/// Result of checking that stints tile a race.
struct CoverageReport {
    bool tiles = false;
    std::string message;        ///< empty when the stints tile the race
};

/// Prefix sums of stops per lap over the whole field, so the number of stops
/// in any lap interval is answered in O(1). Built once by
/// PitAnalytics::buildStopIndex.
class StopLapIndex {
public:
    StopLapIndex() = default;
    int raceLaps() const noexcept { return raceLaps_; }
    /// Stops made at the end of laps fromLap..toLap, both included. O(1).
    /// \throws std::out_of_range unless 1 <= fromLap <= toLap <= raceLaps().
    int stopsInLaps(int fromLap, int toLap) const;
    /// Stops made at the end of laps 1..lap. O(1). lap = 0 gives 0.
    /// \throws std::out_of_range unless 0 <= lap <= raceLaps().
    int cumulativeStops(int lap) const;

private:
    friend class PitAnalytics;
    int raceLaps_ = 0;
    std::vector<int> prefix_;  ///< prefix_[k] = stops at the end of laps 1..k
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

    // --- plan sec. 4.5: stint statistics, chronology, windows, coverage ---

    /// The stints of one summary as lap intervals, in order. O(S).
    static std::vector<StintInterval> stintIntervals(const DriverPitSummary& summary);

    /// Count, shortest, longest and mean of the stint lengths in `summary`.
    /// O(S).
    static StintStats stintStats(const DriverPitSummary& summary);

    /// The same over every stint of every car in the log. O(n) expected.
    StintStats fieldStintStats() const;

    /// Stint-length histogram over the whole field: bins of `binWidth` laps
    /// starting at the shortest stint, empty bins included, so the counts sum
    /// to the number of stints. O(n + B) for B bins. Empty for no stints.
    /// \throws std::invalid_argument if binWidth < 1.
    std::vector<StintHistogramBin> stintHistogram(int binWidth) const;

    /// Pit-lane time lost by the whole field: the number of stops times
    /// `pitLoss`. The simulator charges a stop as one lump (entry, tire change
    /// and exit share a timestamp), so this IS the pit-lane time. O(n).
    double fieldPitLoss(double pitLoss) const;

    /// Every stop in the log, sorted by (time, car number, lap). The log is
    /// already in canonical order; this sorts anyway so the report does not
    /// depend on that. O(n + s log s) for s stops.
    std::vector<PitStopRecord> chronologicalPitStops() const;

    /// Prefix sums of stops per lap, for a race of `raceLaps` laps. O(n + N).
    /// \throws std::invalid_argument if raceLaps < 0 or a stop is outside it.
    StopLapIndex buildStopIndex(int raceLaps) const;

    /// Stops, distinct cars, earliest and latest stop lap, and pit loss for
    /// laps fromLap..toLap. O(n).
    /// \throws std::invalid_argument unless 1 <= fromLap <= toLap.
    PitWindowSummary windowSummary(int fromLap, int toLap, double pitLoss) const;

    /// Stops of two cars compared in order. O(n).
    StopTimingComparison compareStopTiming(int driverA, int driverB) const;

    /// Do these stints tile laps 1..raceLaps exactly? Sorts a copy by first
    /// lap, then reports the first gap, overlap or out-of-range stint.
    /// O(S log S).
    static CoverageReport checkStintCoverage(std::vector<StintInterval> stints, int raceLaps);

    /// Greedy interval stabbing: the fewest laps such that every window holds
    /// at least one chosen lap, i.e. the fewest distinct "pit calls" that let
    /// every car stop inside its allowed window. Sort by latest lap, take a
    /// window's latest lap whenever it is not already covered. Returns the
    /// chosen laps ascending. O(W log W). Optimal (standard exchange argument,
    /// and checked against brute force in the tests).
    /// \throws std::invalid_argument if a window has earliestLap > latestLap
    ///         or a lap below 1.
    static std::vector<int> minimumStopLaps(std::vector<StopWindow> windows);

    /// For every stop in `records` (a LapRecord with pitStop set), the
    /// traffic seconds the same car lost over its next `lapsAfter` laps. The
    /// laps after a stop are fewer than asked when the race ends first.
    /// Ordered by car number then stop lap. O(R log R): each car's laps are
    /// sorted by lap number, then prefix sums and one binary search per stop.
    /// \throws std::invalid_argument if lapsAfter < 1.
    static std::vector<TrafficAfterStop> trafficAfterStops(const std::vector<LapRecord>& records,
                                                           int lapsAfter);

private:
    const EventLog& log_;
};

}  // namespace boxbox

#endif  // BOXBOX_PITANALYTICS_HPP