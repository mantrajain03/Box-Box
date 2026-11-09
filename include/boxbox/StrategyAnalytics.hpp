#ifndef BOXBOX_STRATEGY_ANALYTICS_HPP
#define BOXBOX_STRATEGY_ANALYTICS_HPP

/// Strategy lab (plan sec. 4.1 and 4.6, owner Sarthak): custom-plan evaluation,
/// pit-loss and compound sensitivity sweeps, the K cheapest plans, the best
/// plan for each stop count, and strategy comparison.
///
/// Everything here is built on PitStrategy's public interface. No lap time is
/// computed in this file: every number comes from PitStrategy::evaluatePlan or
/// PitStrategy::optimize, so a custom plan, an alternative and the optimum are
/// always totalled by the same arithmetic. Nothing here prints or parses.
///
/// Plans are PlannedStint lists (compound index, laps), the form evaluatePlan
/// already takes.

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "boxbox/PitAnalytics.hpp"
#include "boxbox/PitStrategy.hpp"
#include "boxbox/RaceSimulator.hpp"

namespace boxbox {

/// Largest number of points a sweep may produce, and the most plans
/// kBestPlans will return. Both bound time and memory (see each function).
inline constexpr std::size_t kMaxSweepPoints = 1000;
inline constexpr std::size_t kMaxAlternatives = 50;

/// A plan with a label, for comparisons.
struct NamedPlan {
    std::string name;
    std::vector<PlannedStint> plan;
};

/// Verdict of validatePlan.
struct PlanValidation {
    bool valid = false;
    std::string message;  ///< empty when valid; otherwise the FIRST problem found
};

/// One stint of an evaluated plan.
struct StintReport {
    std::size_t compoundIndex = 0;
    std::string compoundName;
    int firstLap = 0;       ///< 1-based, inclusive
    int lastLap = 0;        ///< 1-based, inclusive
    int laps = 0;
    double time = 0.0;      ///< green-flag seconds, no pit loss
    double share = 0.0;     ///< time / green-flag total, 0..1
};

/// Laps run on one compound over a whole plan.
struct CompoundLaps {
    std::size_t compoundIndex = 0;
    std::string compoundName;
    int laps = 0;
};

/// A plan evaluated under the model, with its distance from the optimum.
/// When `feasible` is false only `name`, `reason` and `plan` are meaningful.
struct StrategyReport {
    std::string name;
    std::vector<PlannedStint> plan;
    bool feasible = false;
    std::string reason;  ///< why the plan is infeasible; empty when feasible

    std::vector<StintReport> stints;
    std::vector<CompoundLaps> compoundUsage;  ///< compounds in order of first use
    int stops = 0;
    double greenTime = 0.0;
    double pitTime = 0.0;
    double totalTime = 0.0;

    double optimumTotal = 0.0;       ///< PitStrategy::optimize() total, for reference
    double deltaVsOptimum = 0.0;     ///< totalTime - optimumTotal; >= 0 up to rounding
    double percentVsOptimum = 0.0;   ///< 100 * delta / optimumTotal (0 if optimum is 0)
};

/// One row of a sensitivity sweep.
struct SweepRow {
    double value = 0.0;               ///< the swept parameter at this row
    double totalTime = 0.0;           ///< optimal total at this value
    int stops = 0;
    std::vector<PlannedStint> plan;   ///< the optimal plan at this value
    double deltaFromPrevious = 0.0;   ///< totalTime - previous row's; 0 for row 0
    bool planChanged = false;         ///< differs from the previous row's plan; false for row 0
};

struct SweepResult {
    std::vector<SweepRow> rows;
    /// Index of the first row whose plan differs from row 0's, if any.
    std::optional<std::size_t> firstChange;
};

/// Compound parameter that compoundSweep varies.
enum class CompoundParameter { BasePace, Degradation, MaxStint, CliffLap, CliffDegradation };

/// Best plan using exactly `stops` stops.
struct StopCountResult {
    int stops = 0;
    bool feasible = false;
    std::string reason;
    StrategyReport report;
};

/// One car's clean-air and traffic results in cleanAirVsTraffic.
struct TrafficComparisonRow {
    std::string name;
    double cleanTime = 0.0;        ///< total race time with overtaking off
    double trafficTime = 0.0;      ///< total race time with the rule on
    double trafficLoss = 0.0;      ///< seconds the rule cost this car (>= 0)
    int cleanPosition = 0;         ///< 1-based finishing position, rule off
    int trafficPosition = 0;       ///< 1-based finishing position, rule on
};

/// Check a plan and say why it fails. O(S) for S stints. The same conditions
/// as PitStrategy::evaluatePlan, which only reports "not feasible".
PlanValidation validatePlan(const PitStrategy& strategy, const std::vector<PlannedStint>& plan);

/// Evaluate one plan, or explain why it cannot be evaluated. O(S) plus one
/// optimize() call for the reference optimum (O(N*C*M)).
StrategyReport evaluateCustom(const PitStrategy& strategy, const std::string& name,
                              const std::vector<PlannedStint>& plan);

/// As evaluateCustom, with the optimum total supplied by the caller so a
/// batch of plans runs optimize() once. O(S).
StrategyReport evaluateAgainst(const PitStrategy& strategy, const std::string& name,
                               const std::vector<PlannedStint>& plan, double optimumTotal);

/// Optimal plan at each pit loss from..to in steps of `step` (both ends
/// included when they fall on a step). Each point builds a PitStrategy with
/// that pit loss and calls optimize(): no approximation.
///
/// COST: O(P*N*C*M) for P points. Memory O(P*S).
/// \throws std::invalid_argument if from, to or step is not finite, from < 0,
///         to < from, step <= 0, or the sweep would exceed kMaxSweepPoints.
SweepResult pitLossSweep(const PitStrategy& strategy, double from, double to, double step);

/// Optimal plan as one compound parameter varies from..to in steps of `step`.
/// The compound is rebuilt with the new value (all else unchanged) and the
/// optimum recomputed. MaxStint and CliffLap values must be whole numbers.
///
/// COST: O(P*(C*M + N*C*M)). Memory O(P*S).
/// \throws std::out_of_range for a bad compound index; std::invalid_argument
///         for a bad range (as pitLossSweep), a non-whole MaxStint/CliffLap
///         value, or a value the compound rejects (e.g. a negative degradation).
SweepResult compoundSweep(const PitStrategy& strategy, std::size_t compoundIndex,
                          CompoundParameter parameter, double from, double to, double step);

/// The `count` cheapest DISTINCT plans, cheapest first. Entry 0 is always
/// PitStrategy::optimize()'s own plan.
///
/// Method: the lap DAG's DP, keeping the `count` + 1 best (cost, predecessor)
/// entries at every lap instead of one (one spare, because the optimizer's own
/// plan may be among them and is dropped from the tail), merged from every (compound, length,
/// predecessor rank) candidate. A path is a unique sequence of stints, so
/// distinct entries are distinct plans. Exact: it is the true K cheapest within
/// `count`, not a heuristic; the cap is the bound `kMaxAlternatives`. Every
/// returned plan is re-costed with evaluatePlan.
///
/// Order is exact cost (no tolerance), then compound index, stint length and
/// predecessor rank, so ties are deterministic. Entry 0 may differ from a
/// sort-by-cost order only by the optimizer's kTimeEpsilon tie rule.
///
/// COST: O(N*C*M*K) candidates, sorted per lap: O(N*C*M*K log(C*M*K)) time.
/// Memory is O(N*K) for the kept entries plus O(C*M*K) for one lap's candidate
/// buffer, which is rebuilt every lap. Each returned plan is also validated and
/// costed in O(S). Fewer than `count` plans come back when fewer exist.
/// \throws std::invalid_argument if count is 0 or above kMaxAlternatives.
std::vector<StrategyReport> kBestPlans(const PitStrategy& strategy, std::size_t count);

/// Best plan with exactly 0, 1, ..., maxStops stops (one entry each). A count
/// with no feasible plan (stints too short for the distance, or more stops than
/// laps allow) is reported infeasible with a reason.
///
/// COST: O(S*N*C*M) time and O(S*N) memory for S = maxStops + 1, plus one
/// optimize() call (O(N*C*M)) for the reference optimum. S can be as large as N,
/// so the memory is quadratic in the race length in the worst case (about
/// 24 bytes per (stop count, lap) cell); the CLI caps --max at the race laps.
/// \throws std::invalid_argument if maxStops is negative or above the race laps.
std::vector<StopCountResult> bestForStopCount(const PitStrategy& strategy, int maxStops);

/// For each compound, the best plan that uses only that compound, evaluated in
/// the full strategy (compound indexes refer to `strategy`). A compound that
/// cannot cover the distance cannot occur, since stints may repeat, so each is
/// feasible. O(C * N*M).
std::vector<StrategyReport> compoundOnlyBest(const PitStrategy& strategy);

/// Evaluate several named plans against the same optimum under one cost model.
/// Infeasible plans are kept, with their reason. O(sum of S) plus one optimize().
std::vector<StrategyReport> compareStrategies(const PitStrategy& strategy,
                                              const std::vector<NamedPlan>& plans);

/// The standard comparison set: "optimal", one "all-<compound>" single-compound
/// plan per compound, the best plan for 0..maxStops stops ("<n>-stop"), and the
/// best plan using only each compound ("only-<compound>").
/// Duplicates by plan are removed, keeping the first name.
std::vector<NamedPlan> standardCandidates(const PitStrategy& strategy, int maxStops);

/// Run the same plans as one field twice: overtaking off, then `rule` on.
/// Car k+1 runs plans[k]. Returns a row per plan.
///
/// With the rule off every car runs alone in effect, so cleanTime equals the
/// plan's evaluated total within kClosureTolerance (FR-8); trafficLoss is the
/// extra time the rule cost, never negative.
/// COST: O(N*D log D) per race for D plans.
/// \throws std::invalid_argument if a plan is infeasible, there are no plans,
///         or `rule` is rejected by RaceSimulator::setOvertakeRule.
std::vector<TrafficComparisonRow> cleanAirVsTraffic(const PitStrategy& strategy,
                                                    const std::vector<NamedPlan>& plans,
                                                    const OvertakeRule& rule);

/// An undercut / overcut question answered with the project's cost model.
struct FirstStopComparison {
    UndercutOvercutResult result;               ///< totals and the delta: later minus earlier, so positive means the earlier stop is faster
    std::vector<PlannedStint> earlierPlan;
    std::vector<PlannedStint> laterPlan;
    std::size_t firstCompound = 0;              ///< the optimum's opening compound, used by both
};

/// Simplified undercut / overcut (plan sec. 4.5): the same car stops at the end
/// of `earlierLap`, or `laterLap`, then drives the best continuation. Both
/// plans open on the optimum's first compound; after the stop, tires are fresh,
/// so the rest of the race is an independent race of the remaining laps, solved
/// by PitStrategy::optimize on a copy of the track with that many laps. Both are
/// costed by PitAnalytics::compareUndercutOvercut, i.e. PitStrategy::evaluatePlan.
///
/// Simplified model: clean air, no tire warm-up, no traffic, no real pit-window
/// rules. A first stint longer than its compound allows makes that plan
/// infeasible, reported as kUnreachable, never as a number.
/// COST: three optimize() calls (the full race, to find the opening compound,
/// and one continuation per plan), each on at most N laps, so O(N*C*M).
/// \throws std::invalid_argument unless 1 <= earlierLap < laterLap < race laps.
FirstStopComparison compareFirstStopLaps(const PitStrategy& strategy, int earlierLap, int laterLap);

/// True when two plans are the same list of (compound, laps).
bool samePlan(const std::vector<PlannedStint>& a, const std::vector<PlannedStint>& b) noexcept;

}  // namespace boxbox

#endif  // BOXBOX_STRATEGY_ANALYTICS_HPP
