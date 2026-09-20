#include "boxbox/PitStrategy.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "boxbox/Common.hpp"

namespace boxbox {

int StrategyResult::plannedLaps() const noexcept {
    int laps = 0;
    for (const Stint& stint : stints) {
        laps += stint.laps;
    }
    return laps;
}

PitStrategy::PitStrategy(Track track, std::vector<TireCompound> compounds)
    : track_(std::move(track)), compounds_(std::move(compounds)) {
    if (compounds_.empty()) {
        throw std::invalid_argument("PitStrategy: at least one tire compound is required");
    }
    buildPrefixTable();
}

// O(C*M) time and space, C = compounds, M = largest maxStintLaps.
//
// Row c holds the running sum of lapDelta over tire ages. Entry L is the cost
// of running exactly L laps on that compound with the track's contribution
// removed, so entry 0 is 0.0 and the L-th lap of a stint is run at age L-1.
// Building this once is what makes every later stintCost query O(1) regardless
// of stint length, which in turn keeps the DP transition O(1).
void PitStrategy::buildPrefixTable() {
    ageCostPrefix_.clear();
    ageCostPrefix_.resize(compounds_.size());

    for (std::size_t c = 0; c < compounds_.size(); ++c) {
        const TireCompound& compound = compounds_[c];
        const int maxLaps = compound.maxStintLaps();

        std::vector<double>& prefix = ageCostPrefix_[c];
        prefix.assign(static_cast<std::size_t>(maxLaps) + 1U, 0.0);

        for (int laps = 1; laps <= maxLaps; ++laps) {
            prefix[static_cast<std::size_t>(laps)] =
                prefix[static_cast<std::size_t>(laps - 1)] + compound.lapDelta(laps - 1);
        }
    }
}

// O(1).
double PitStrategy::stintDelta(std::size_t compoundIndex, int laps) const noexcept {
    if (laps <= 0) {
        return 0.0;
    }
    if (compoundIndex >= ageCostPrefix_.size()) {
        return kUnreachable;
    }
    const std::vector<double>& prefix = ageCostPrefix_[compoundIndex];
    if (static_cast<std::size_t>(laps) >= prefix.size()) {
        return kUnreachable;  // longer than this compound's maxStintLaps
    }
    return prefix[static_cast<std::size_t>(laps)];
}

// O(1). PRD sec. 3.3:  stintCost(c, L) = L * baseLapTime + ageCostPrefix[c][L]
double PitStrategy::stintCost(std::size_t compoundIndex, int laps) const noexcept {
    const double delta = stintDelta(compoundIndex, laps);
    if (!isReachable(delta)) {
        return kUnreachable;
    }
    if (laps <= 0) {
        return 0.0;
    }
    return static_cast<double>(laps) * track_.baseLapTime() + delta;
}

int PitStrategy::maxStintLaps() const noexcept {
    int longest = 0;
    for (const TireCompound& compound : compounds_) {
        longest = std::max(longest, compound.maxStintLaps());
    }
    return longest;
}

// The one place a stint list becomes a race time. Both optimize() and
// evaluatePlan() funnel through here, so a hand-entered plan and a computed
// plan are always totalled by identical arithmetic. O(S) for S stints.
//
// PRD sec. 3.4:  totalTime = sum(stintCost) + (stints - 1) * pitLoss
StrategyResult PitStrategy::buildResult(const std::vector<PlannedStint>& plan) const {
    StrategyResult result;
    result.dpValue = kUnreachable;  // no DP ran; optimize() overwrites this

    const int totalLaps = track_.totalLaps();

    if (plan.empty()) {
        // Only a zero-lap race is legitimately covered by an empty plan.
        result.feasible = (totalLaps <= 0);
        return result;
    }

    int coveredLaps = 0;
    for (const PlannedStint& stint : plan) {
        if (stint.laps <= 0 || stint.compoundIndex >= compounds_.size()) {
            return StrategyResult{};  // feasible stays false
        }
        if (stint.laps > compounds_[stint.compoundIndex].maxStintLaps()) {
            return StrategyResult{};
        }
        coveredLaps += stint.laps;
    }
    if (coveredLaps != totalLaps) {
        return StrategyResult{};  // a plan must cover the race exactly
    }

    int startLap = 1;
    for (std::size_t i = 0; i < plan.size(); ++i) {
        Stint stint;
        stint.compoundIndex = plan[i].compoundIndex;
        stint.compoundName = compounds_[stint.compoundIndex].name();
        stint.startLap = startLap;
        stint.laps = plan[i].laps;
        stint.time = stintCost(stint.compoundIndex, stint.laps);

        result.greenTime += stint.time;
        result.stints.push_back(std::move(stint));

        startLap += plan[i].laps;

        // A stop follows every stint but the last, at the end of its final lap.
        if (i + 1U < plan.size()) {
            result.pitLaps.push_back(startLap - 1);
        }
    }

    result.pitTime = track_.pitLoss() * static_cast<double>(result.pitLaps.size());
    result.totalTime = result.greenTime + result.pitTime;
    result.feasible = true;
    return result;
}

StrategyResult PitStrategy::evaluatePlan(const std::vector<PlannedStint>& plan) const {
    return buildResult(plan);
}

// Fewest stints that fit under the compound's maxStintLaps, with the distance
// split as evenly as possible and remainder laps handed to the earliest
// stints. Deterministic, and independent of the DP. O(S).
StrategyResult PitStrategy::singleCompoundPlan(std::size_t compoundIndex) const {
    if (compoundIndex >= compounds_.size()) {
        return StrategyResult{};
    }

    const int totalLaps = track_.totalLaps();
    if (totalLaps <= 0) {
        return buildResult({});
    }

    const int maxStint = compounds_[compoundIndex].maxStintLaps();
    const int stintCount = (totalLaps + maxStint - 1) / maxStint;  // ceil division
    const int baseLength = totalLaps / stintCount;
    const int remainder = totalLaps % stintCount;

    std::vector<PlannedStint> plan;
    plan.reserve(static_cast<std::size_t>(stintCount));
    for (int i = 0; i < stintCount; ++i) {
        PlannedStint stint;
        stint.compoundIndex = compoundIndex;
        stint.laps = baseLength + ((i < remainder) ? 1 : 0);
        plan.push_back(stint);
    }

    return buildResult(plan);
}

// ---------------------------------------------------------------------------
// FR-1: the optimizer.
//
//   dp[0] = 0
//   dp[l] = min over compounds c, over stint lengths k in [1, min(l, maxStint(c))]
//           of  dp[l-k] + stintCost(c, k) + (l-k > 0 ? pitLoss : 0)
//
// The pit-loss term is charged on entering any stint that is not the first:
// the car starts the race on its opening set, and the race ends rather than
// pitting after the last one, so an m-stint plan makes exactly m-1 stops.
//
// Cost: O(N*C*M) time. Every (lap, compound, stint length) triple is examined
// once and each costs O(1) thanks to the prefix table. O(N) extra space for dp
// and choice. Backtracking is O(N) in the worst case of all one-lap stints.
// ---------------------------------------------------------------------------
StrategyResult PitStrategy::optimize() const {
    StrategyResult result;

    const int totalLaps = track_.totalLaps();

    // A zero-lap race is trivially satisfied by an empty plan. Reported as
    // feasible so that callers do not have to special-case it.
    if (totalLaps <= 0) {
        result.feasible = true;
        return result;
    }

    const double pitLoss = track_.pitLoss();
    const std::size_t compoundCount = compounds_.size();
    const std::size_t stateCount = static_cast<std::size_t>(totalLaps) + 1U;

    struct Choice {
        std::size_t compoundIndex = 0;
        int laps = 0;  // 0 marks "no transition recorded"
    };

    std::vector<double> dp(stateCount, kUnreachable);
    std::vector<Choice> choice(stateCount);
    dp[0] = 0.0;

    for (int lap = 1; lap <= totalLaps; ++lap) {
        const std::size_t lapIndex = static_cast<std::size_t>(lap);

        for (std::size_t c = 0; c < compoundCount; ++c) {
            const int longestStint = std::min(lap, compounds_[c].maxStintLaps());

            for (int len = 1; len <= longestStint; ++len) {
                const std::size_t prevIndex = static_cast<std::size_t>(lap - len);
                if (!isReachable(dp[prevIndex])) {
                    continue;
                }

                const double cost = stintCost(c, len);
                if (!isReachable(cost)) {
                    continue;  // defensive: longestStint already excludes these
                }

                const double stopPenalty = (lap - len > 0) ? pitLoss : 0.0;
                const double candidate = dp[prevIndex] + cost + stopPenalty;

                // Strict improvement only, so near-equal candidates leave the
                // incumbent in place and the first one enumerated wins (NFR-5).
                if (definitelyLess(candidate, dp[lapIndex])) {
                    dp[lapIndex] = candidate;
                    choice[lapIndex] = Choice{c, len};
                }
            }
        }
    }

    const std::size_t finalIndex = static_cast<std::size_t>(totalLaps);
    if (!isReachable(dp[finalIndex])) {
        return result;  // feasible stays false
    }

    // Backtrack: walk choice[] from the finish line back to lap 0, collecting
    // stints in reverse. O(N) worst case, when every stint is one lap.
    std::vector<PlannedStint> plan;
    for (int lap = totalLaps; lap > 0;) {
        const Choice step = choice[static_cast<std::size_t>(lap)];
        if (step.laps <= 0 || step.laps > lap) {
            // Unreachable given a finite dp[totalLaps]; guarded anyway so a
            // future change can never turn this into an infinite loop.
            return StrategyResult{};
        }
        PlannedStint stint;
        stint.compoundIndex = step.compoundIndex;
        stint.laps = step.laps;
        plan.push_back(stint);
        lap -= step.laps;
    }
    std::reverse(plan.begin(), plan.end());

    // Total the reconstructed plan through the shared path rather than copying
    // dp[N], so the two are computed independently and can be compared. They
    // agree up to floating-point summation order.
    result = buildResult(plan);
    result.dpValue = dp[finalIndex];

    return result;
}

}  // namespace boxbox
