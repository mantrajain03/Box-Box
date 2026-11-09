#ifndef BOXBOX_DRIVER_ANALYTICS_HPP
#define BOXBOX_DRIVER_ANALYTICS_HPP

/// Driver and lap analytics (plan sec. 4.3 and 8.4; owner Mantra).
///
/// Everything here is computed from LapRecords: what the simulator actually
/// ran, recorded lap by lap from outside by LapRecorder. Nothing re-derives a
/// lap time or invents a race outcome, and RaceSimulator knows nothing about
/// this file.
///
///   LapRecorder       watches a race and produces the LapRecords
///   DriverAnalytics   per-car summaries, found by car number through a hash map
///   selectLaps        the K fastest or slowest laps, with a bounded heap
///   trailingWindows   fastest, slowest and average lap over each car's last W
///                     laps, with two MonotonicDeques
///   summarizeWindows  the W-lap stretches that stand out in a car's race
///   rankDrivers       pace and consistency rankings with explicit tie-breaks
///
/// Conventions:
///   * laps are 1-based (PRD sec. 3.1); a car's laps run 1 .. n in order
///   * lap times include traffic and exclude pit loss (see LapRecord)
///   * recorded lap times are compared EXACTLY, with < only, like
///     Leaderboard::isAhead and SegmentTree: a selection must return the very
///     lap a full sort or a plain scan would. Ties fall through to explicit
///     tie-breakers.
///   * window averages are computed sums, which two ways of summing can make
///     differ in the last bits, so those alone are compared with
///     definitelyLess and kTimeEpsilon (see summarizeWindows)
///
/// No sector analytics. The simulator records no per-car sector times (see
/// LapRecord), and the plan (sec. 4.3E and 8.4) rules out presenting the
/// track's configured sector times as a car's measured performance.

#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "boxbox/LapRecord.hpp"
#include "boxbox/RaceSimulator.hpp"

namespace boxbox {

// ---------------------------------------------------------------------------
// Recording
// ---------------------------------------------------------------------------

/// Records every car's laps as LapRecords by watching a RaceSimulator from
/// outside, through its public getters only. Use it as
///
///     LapRecorder recorder(simulator);  // on the grid, before lap 1
///     while (simulator.advanceLap()) {
///         recorder.observe(simulator);
///     }
///
/// How each field is measured. The recorder keeps every car's state from the
/// previous call and compares it with the state now:
///   lapTime      Driver::lastLapTime()
///   trafficTime  the rise in Driver::trafficLoss() over the lap
///   pitStop      Driver::stopsMade() went up; pitTime is then
///                Track::pitLoss(), the amount the simulator charged
///   compoundIndex, tireAge, stint
///                the car's state BEFORE the lap. After a stop the driver
///                already shows the new set, which is not what the lap ran on.
///   position     RaceSimulator::standings() after the lap
///
/// Not the same as RaceLapRecorder (tire module), which keeps only each car's
/// lap seconds, in a segment tree, for its range queries.
///
/// COSTS, D = cars, N = race laps:
///   construct  O(D log D) - the car numbers are checked distinct by sorting
///   observe    O(D log D) per lap: O(D) to compare states, plus one call to
///              RaceSimulator::standings, which is O(D log D). O(N*D log D)
///              over a race.
///   storage    O(N*D): one record per car per lap
class LapRecorder {
public:
    /// \throws std::invalid_argument if any car has already run a lap - the
    ///         recorder must see the race from the grid - or two cars share a
    ///         number, since records tell cars apart by number.
    explicit LapRecorder(const RaceSimulator& simulator);

    /// Record a LapRecord for every car that completed a lap since the last
    /// call. Records are appended lap by lap, and within a lap in grid order.
    /// A call with no new lap records nothing.
    /// \throws std::invalid_argument if the field changed, or a car completed
    ///         more than one lap since the last call (a missed observe) or
    ///         went backwards (a reset). A rejected call records nothing.
    void observe(const RaceSimulator& simulator);

    /// Car numbers in grid order: the order of RaceSimulator::drivers().
    const std::vector<int>& field() const noexcept { return field_; }

    /// Display names, parallel to field().
    const std::vector<std::string>& names() const noexcept { return names_; }

    /// Every record so far.
    const std::vector<LapRecord>& records() const noexcept { return records_; }

private:
    /// What the recorder needs to remember about a car between two calls.
    struct CarState {
        int laps = 0;
        std::size_t compoundIndex = 0;
        int tireAge = 0;
        int stint = 0;
        int stops = 0;
        double trafficLoss = 0.0;
    };

    static CarState stateOf(const Driver& driver) noexcept;

    std::vector<int> field_;
    std::vector<std::string> names_;
    std::vector<CarState> before_;  ///< each car's state at the last call
    std::vector<LapRecord> records_;
};

// ---------------------------------------------------------------------------
// Per-car summaries
// ---------------------------------------------------------------------------

/// One stint as it was actually run, rebuilt from the lap records.
struct StintSummary {
    int stint = 0;                  ///< 0-based
    std::size_t compoundIndex = 0;
    int firstLap = 0;
    int lastLap = 0;
    int laps = 0;                   ///< lastLap - firstLap + 1
    double time = 0.0;              ///< sum of lapTime: traffic in, pit loss out
};

/// Laps a car ran on one compound, over all of its stints on it.
struct CompoundUsage {
    std::size_t compoundIndex = 0;
    int laps = 0;
};

/// One car's race, summarised from its lap records (plan sec. 10D).
///
/// Definitions. n = laps, and every sum runs over the car's records:
///   totalTime         sum of lapTime + pitTime, added lap by lap in the order
///                     the simulator charges them, so it reproduces
///                     Driver::cumulativeTime() exactly
///   greenTime         sum of lapTime: time on track, traffic included
///   pitTime           sum of pitTime. totalTime = greenTime + pitTime.
///   trafficTime       sum of trafficTime: the part of greenTime lost behind
///                     other cars, never added to anything a second time
///   stops             laps that ended in a stop
///   averageLap        greenTime / n
///   bestLap, worstLap the fastest and slowest lapTime, earliest lap on ties
///   lapRange          worstLap.lapTime - bestLap.lapTime
///   meanAbsDeviation  (1/n) * sum of |lapTime - averageLap|: how far a lap
///                     typically strays from the car's own average. A project
///                     statistic on simulated laps, not a real-world rating.
///   positions         positions[k] is the running position after lap k + 1
/// With n = 0 every statistic is 0, bestLap.lap and worstLap.lap are 0, and
/// the lists are empty.
struct DriverSummary {
    int driverNumber = 0;
    std::string name;  ///< display label; empty when none was supplied
    int laps = 0;
    double totalTime = 0.0;
    double greenTime = 0.0;
    double pitTime = 0.0;
    double trafficTime = 0.0;
    int stops = 0;
    double averageLap = 0.0;
    LapRecord bestLap;
    LapRecord worstLap;
    double lapRange = 0.0;
    double meanAbsDeviation = 0.0;
    std::vector<StintSummary> stints;      ///< in race order
    std::vector<CompoundUsage> compounds;  ///< by compound index, compounds used only
    std::vector<int> positions;
    int bestPosition = 0;   ///< highest place held after any lap; 0 when n = 0
    int worstPosition = 0;  ///< lowest place held after any lap; 0 when n = 0
};

/// Per-car summaries over a set of lap records, with lookup by car number.
///
/// Records are checked as they are added, so every summary rests on records
/// that make sense together: each car's laps run 1, 2, 3, ... in order (other
/// cars' records may sit in between), times are finite, traffic and pit time
/// are not negative, pit time appears only on a stop lap, the stint starts at
/// 0 and moves on exactly after each stop, the compound stays the same within
/// a stint, and every position lies in 1 .. field size.
///
/// The hash map (plan sec. 4.3A) takes a car number to that car's slot in
/// vectors kept in grid order. It is only ever used for lookup and never
/// iterated, so its bucket order cannot reach any output (NFR-5).
///
/// COSTS, D = cars, R = records:
///   construct          O(D + R) average: one hash lookup per record, O(1) on
///                      average, plus sorting each car's stints by compound for
///                      its usage list, O(S log S) for S stints. A lookup is
///                      O(D) in the worst case under pathological hashing, so
///                      O(R*D) at worst.
///   summary, lapTimes  O(1) average, O(D) worst case: one lookup
///   summaries, records O(1)
///   storage            O(D + R)
class DriverAnalytics {
public:
    /// \param field   every car in the race, by number, in grid order
    /// \param records completed laps
    /// \throws std::invalid_argument if a number repeats in `field`, or a
    ///         record names a car outside it or breaks the rules above.
    DriverAnalytics(const std::vector<int>& field, std::vector<LapRecord> records);

    /// Everything a LapRecorder saw, with the cars' display names.
    explicit DriverAnalytics(const LapRecorder& recorder);

    /// One summary per car, in grid order.
    const std::vector<DriverSummary>& summaries() const noexcept { return summaries_; }

    /// \throws std::out_of_range for a car number outside the field.
    const DriverSummary& summary(int driverNumber) const;

    /// The car's lap times, laps 1 .. n in order, as trailingWindows takes them.
    /// \throws std::out_of_range for a car number outside the field.
    const std::vector<double>& lapTimes(int driverNumber) const;

    bool contains(int driverNumber) const;

    /// The checked records, in the order given.
    const std::vector<LapRecord>& records() const noexcept { return records_; }

private:
    std::size_t slotOf(int driverNumber) const;

    std::vector<LapRecord> records_;
    std::vector<DriverSummary> summaries_;           ///< grid order
    std::vector<std::vector<double>> lapTimes_;      ///< grid order
    std::unordered_map<int, std::size_t> slot_;      ///< car number -> grid slot
};

// ---------------------------------------------------------------------------
// Fastest and slowest laps
// ---------------------------------------------------------------------------

enum class LapOrder { Fastest, Slowest };

/// The order selectLaps returns laps in: by lapTime (ascending for Fastest,
/// descending for Slowest), then the earlier lap, then the lower car number.
/// Exact comparisons only. That is a strict total order on records with
/// distinct (car, lap), which DriverAnalytics guarantees, so the K "best"
/// laps are always the same K in the same order (NFR-5). Exposed so a test
/// can check selectLaps against a full sort.
bool lapRanksAhead(const LapRecord& a, const LapRecord& b, LapOrder order) noexcept;

/// Which laps selectLaps considers. The default considers every lap.
struct LapFilter {
    std::optional<int> driverNumber;           ///< only this car
    std::optional<std::size_t> compoundIndex;  ///< only laps run on this compound
    int firstLap = 1;                          ///< inclusive
    int lastLap = std::numeric_limits<int>::max();  ///< inclusive
};

/// The k laps that rank first under lapRanksAhead among the records that
/// pass `filter`, best first (plan sec. 4.3B): the fastest or slowest K laps
/// of the race, of one car, on one compound, or within a range of laps.
///
/// Selection uses a hand-written bounded binary heap holding at most k laps,
/// with the WORST of them at the root: a new lap only has to beat the root to
/// get in, and replacing the root costs one sift-down. Sorting every lap
/// would cost O(R log R); the heap only ever orders k of them.
///
/// k = 0 gives an empty list; a k above the number of matching laps gives
/// all of them, still in order.
///
/// COST: O(R log k) to scan R records, plus O(k log k) to put the k kept in
/// order - O(R log k) overall, since k <= R once capped - and O(k) space.
/// \throws std::invalid_argument if a lap the filter lets through has a lap
///         time that is not finite.
std::vector<LapRecord> selectLaps(const std::vector<LapRecord>& records, std::size_t k,
                                  LapOrder order, const LapFilter& filter = LapFilter{});

// ---------------------------------------------------------------------------
// Sliding windows
// ---------------------------------------------------------------------------

/// A run of one car's laps ending on lastLap: laps firstLap .. lastLap.
struct LapWindow {
    int firstLap = 0;
    int lastLap = 0;
    int laps = 0;          ///< lastLap - firstLap + 1
    double fastest = 0.0;  ///< fastest lap time in the window
    int fastestLap = 0;    ///< its lap number, the earliest on ties
    double slowest = 0.0;  ///< slowest lap time in the window
    int slowestLap = 0;    ///< its lap number, the earliest on ties
    double average = 0.0;  ///< mean lap time over the window

    /// slowest - fastest: the window's lap-time spread.
    double spread() const noexcept { return slowest - fastest; }
};

/// For each lap k of one car (lapTimes[k - 1] is lap k), the window of the
/// last W laps run so far: laps max(1, k - W + 1) .. k (plan sec. 4.3C).
/// The first W - 1 windows are shorter because the race has not run W laps
/// yet, so W = 1 gives every lap on its own, and a W at or above the number
/// of laps gives the running best and worst since lap 1.
///
/// Fastest and slowest come from two MonotonicDeques and the average from a
/// prefix-sum table, each O(1) amortised per lap, against O(W) per lap to
/// rescan every window.
///
/// COST: O(n) time and O(n) space for n laps; the deques hold at most
/// min(W, n) laps each.
/// \throws std::invalid_argument if window is 0 or a lap time is not finite.
std::vector<LapWindow> trailingWindows(const std::vector<double>& lapTimes, std::size_t window);

/// The W-lap stretches that stand out in one car's race (plan sec. 4.3C/D).
///
///   quickest  the window with the lowest average lap: the car's best pace
///   slowest   the window with the highest average lap
///   dropOff   slowest.average - quickest.average: how far its pace fell
///   widest    the window with the largest spread (slowest - fastest lap):
///             the car's sliding-window pace variation
///
/// Only full-length windows of exactly W laps are compared, so an early,
/// shorter window cannot win on one fresh-tire lap. A car with fewer than W
/// laps has no full window; its whole race is then the one window considered
/// and `complete` is false. With no laps at all, every window is empty (lap 0).
///
/// Windows are scanned in race order and a later one replaces the current
/// pick only when it is better by more than kTimeEpsilon (definitelyLess), so
/// near-equal stretches go to the earliest - the optimizer's own tie rule.
/// Averages come from prefix sums, which differ from a direct sum in the last
/// bits, and the tolerance keeps that noise from deciding the pick.
struct WindowSummary {
    std::size_t window = 0;
    bool complete = false;  ///< the car ran at least `window` laps
    LapWindow quickest;
    LapWindow slowest;
    LapWindow widest;
    double dropOff = 0.0;
};

/// COST: O(n), on top of trailingWindows. \throws as trailingWindows.
WindowSummary summarizeWindows(const std::vector<double>& lapTimes, std::size_t window);

// ---------------------------------------------------------------------------
// Rankings
// ---------------------------------------------------------------------------

/// What rankDrivers orders by. Lower is better for every metric.
enum class RankMetric {
    AverageLap,       ///< pace: DriverSummary::averageLap
    BestLap,          ///< one-lap pace: bestLap.lapTime
    LapRange,         ///< consistency: worst lap minus best lap
    MeanAbsDeviation  ///< consistency: mean distance from the car's own average
};

/// The value rankDrivers compares for one car. Meaningful only when
/// summary.laps > 0.
double rankValue(const DriverSummary& summary, RankMetric metric) noexcept;

/// Car numbers ordered by `metric`, best first (plan sec. 4.3D).
///
/// Explicit tie-breakers: values are compared exactly, with < only, and
/// equal values go to the lower car number. A car with no laps has no value
/// and ranks after every car that has one, again by car number. That is a
/// strict total order for distinct car numbers, as DriverAnalytics
/// guarantees, so the ranking is the same on every run (NFR-5).
///
/// COST: O(D log D).
std::vector<int> rankDrivers(const std::vector<DriverSummary>& summaries, RankMetric metric);

}  // namespace boxbox

#endif  // BOXBOX_DRIVER_ANALYTICS_HPP
