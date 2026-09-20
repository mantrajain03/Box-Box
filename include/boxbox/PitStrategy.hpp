#ifndef BOXBOX_PIT_STRATEGY_HPP
#define BOXBOX_PIT_STRATEGY_HPP

#include <cstddef>
#include <string>
#include <vector>

#include "boxbox/TireCompound.hpp"
#include "boxbox/Track.hpp"

namespace boxbox {

/// A stint as a caller specifies it: which compound, and for how many laps.
/// The minimal input form, before any cost has been worked out.
struct PlannedStint {
    std::size_t compoundIndex = 0;
    int laps = 0;
};

/// One planned stint: a run of consecutive laps on a single set of tires.
struct Stint {
    std::size_t compoundIndex = 0;  ///< index into PitStrategy::compounds()
    std::string compoundName;
    int startLap = 1;               ///< 1-based number of this stint's first lap
    int laps = 0;                   ///< stint length; tire ages 0 .. laps-1
    double time = 0.0;              ///< green-flag seconds; excludes pit loss
};

/// The outcome of the optimizer (PRD FR-1).
struct StrategyResult {
    bool feasible = false;

    std::vector<Stint> stints;
    std::vector<int> pitLaps;  ///< a stop happens at the END of each of these laps

    double greenTime = 0.0;  ///< sum of stint times, excluding pit losses
    double pitTime = 0.0;    ///< stops * Track::pitLoss()
    double totalTime = 0.0;  ///< greenTime + pitTime

    /// The raw dp[totalLaps] value. Equals totalTime up to floating-point
    /// summation order; kept separately so the DP can be checked directly.
    ///
    /// Only meaningful for a result that came from optimize(). A plan costed
    /// by evaluatePlan() never ran a DP, and reports kUnreachable here.
    double dpValue = 0.0;

    int stopCount() const noexcept { return static_cast<int>(pitLaps.size()); }

    /// Sum of the stint lengths. For a feasible result this is exactly the
    /// track's total lap count.
    int plannedLaps() const noexcept;
};

/// Minimum-time pit-stop scheduler.
///
/// Holds its own copy of the Track and the compound set, so there is no
/// lifetime coupling to the caller's objects and no RaceSimulator is needed to
/// construct or exercise it (NFR-3).
///
/// Complexity, with N = track laps, C = compounds, M = largest maxStintLaps:
///   * construction   O(C*M) time and space, to build the prefix-sum table
///   * stintCost      O(1)
///   * optimize       O(N*C*M) time, O(N) extra space
class PitStrategy {
public:
    /// \throws std::invalid_argument if `compounds` is empty.
    PitStrategy(Track track, std::vector<TireCompound> compounds);

    /// Compute the minimum-time stint plan by dynamic programming.
    ///
    /// A zero-lap race is feasible with an empty plan and zero time.
    ///
    /// On `feasible`: with the compound set non-empty and every maxStintLaps
    /// at least 1 (both enforced by the constructors), any lap count can be
    /// covered by repeating one-lap stints, so today this flag is always true
    /// for a successfully constructed PitStrategy. It is reported anyway so
    /// callers already branch on it before a future constraint - a stop-count
    /// cap, or a rule forcing two distinct compounds - makes it reachable.
    ///
    /// Deterministic: ties break toward the first candidate enumerated in
    /// (compound declaration order, ascending stint length). See NFR-5.
    StrategyResult optimize() const;

    /// Green-flag seconds for a `laps`-lap stint on compound `compoundIndex`,
    /// excluding pit loss. Returns 0.0 for laps <= 0 and kUnreachable when the
    /// stint is longer than the compound allows or the index is out of range.
    /// O(1): one prefix-table lookup.
    double stintCost(std::size_t compoundIndex, int laps) const noexcept;

    /// The tire-only part of stintCost: the sum of lapDelta over ages
    /// 0 .. laps-1, with the track's contribution left out. O(1).
    double stintDelta(std::size_t compoundIndex, int laps) const noexcept;

    /// Cost an arbitrary plan under exactly the model optimize() minimises.
    ///
    /// Shares one code path with optimize(), so an entered plan and a computed
    /// plan are always totalled the same way. Returns feasible == false when
    /// the plan is malformed: an empty or over-long stint, an out-of-range
    /// compound, or a lap total that does not match the track exactly.
    ///
    /// O(S) for an S-stint plan.
    StrategyResult evaluatePlan(const std::vector<PlannedStint>& plan) const;

    /// The fewest-stop plan that runs the whole race on a single compound,
    /// splitting the distance as evenly as possible and giving any remainder
    /// laps to the earliest stints.
    ///
    /// Not an optimizer: it is a reference plan to compare the optimum
    /// against, and the material for a rival car in the simulator. Returns
    /// feasible == false for an out-of-range compound index. O(S).
    StrategyResult singleCompoundPlan(std::size_t compoundIndex) const;

    const Track& track() const noexcept { return track_; }
    const std::vector<TireCompound>& compounds() const noexcept { return compounds_; }

    /// Largest maxStintLaps across the compound set.
    int maxStintLaps() const noexcept;

private:
    void buildPrefixTable();

    /// Validate a stint list and total it into a StrategyResult. The single
    /// place where a plan becomes a time, shared by optimize() and
    /// evaluatePlan() so the two can never drift apart.
    StrategyResult buildResult(const std::vector<PlannedStint>& plan) const;

    Track track_;
    std::vector<TireCompound> compounds_;

    /// ageCostPrefix_[c][L] = sum of compounds_[c].lapDelta(a) for a in [0, L).
    /// Row c has maxStintLaps(c) + 1 entries, so index L is valid for
    /// 0 <= L <= maxStintLaps(c).
    std::vector<std::vector<double>> ageCostPrefix_;
};

}  // namespace boxbox

#endif  // BOXBOX_PIT_STRATEGY_HPP
