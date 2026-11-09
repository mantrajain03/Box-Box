#include "boxbox/StrategyAnalytics.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

#include "boxbox/Common.hpp"

namespace boxbox {

namespace {

std::vector<PlannedStint> planOf(const StrategyResult& result) {
    std::vector<PlannedStint> plan;
    plan.reserve(result.stints.size());
    for (const Stint& stint : result.stints) {
        PlannedStint p;
        p.compoundIndex = stint.compoundIndex;
        p.laps = stint.laps;
        plan.push_back(p);
    }
    return plan;
}

std::string compoundLabel(const PitStrategy& strategy, std::size_t index) {
    return index < strategy.compounds().size() ? strategy.compounds()[index].name()
                                               : "compound " + std::to_string(index);
}

}  // namespace

namespace {

/// The first stint of `firstCompound` for `stopLap` laps, then the best way to
/// run the remaining laps on fresh tires.
std::vector<PlannedStint> planStoppingAt(const PitStrategy& strategy, std::size_t firstCompound,
                                         int stopLap) {
    Track rest = strategy.track();
    rest.setTotalLaps(strategy.track().totalLaps() - stopLap);
    const PitStrategy continuation(rest, strategy.compounds());

    std::vector<PlannedStint> plan;
    plan.push_back(PlannedStint{firstCompound, stopLap});
    const std::vector<PlannedStint> tail = planOf(continuation.optimize());
    plan.insert(plan.end(), tail.begin(), tail.end());
    return plan;
}

}  // namespace

FirstStopComparison compareFirstStopLaps(const PitStrategy& strategy, int earlierLap, int laterLap) {
    const int raceLaps = strategy.track().totalLaps();
    if (earlierLap < 1 || earlierLap >= laterLap || laterLap >= raceLaps) {
        throw std::invalid_argument(
            "compareFirstStopLaps: need 1 <= earlier lap < later lap < the " +
            std::to_string(raceLaps) + " race laps");
    }
    FirstStopComparison out;
    const std::vector<PlannedStint> optimal = planOf(strategy.optimize());
    out.firstCompound = optimal.empty() ? 0U : optimal.front().compoundIndex;
    out.earlierPlan = planStoppingAt(strategy, out.firstCompound, earlierLap);
    out.laterPlan = planStoppingAt(strategy, out.firstCompound, laterLap);
    out.result = PitAnalytics::compareUndercutOvercut(strategy, out.earlierPlan, out.laterPlan,
                                                      earlierLap, laterLap);
    return out;
}

bool samePlan(const std::vector<PlannedStint>& a, const std::vector<PlannedStint>& b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].compoundIndex != b[i].compoundIndex || a[i].laps != b[i].laps) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Validation and evaluation
// ---------------------------------------------------------------------------

// O(S). Reports the first problem; the conditions are exactly those of
// PitStrategy::buildResult, with a reason attached.
PlanValidation validatePlan(const PitStrategy& strategy, const std::vector<PlannedStint>& plan) {
    PlanValidation verdict;
    const int raceLaps = strategy.track().totalLaps();

    if (plan.empty()) {
        if (raceLaps > 0) {
            verdict.message = "the plan has no stints but the race is " +
                              std::to_string(raceLaps) + " laps";
            return verdict;
        }
        verdict.valid = true;
        return verdict;
    }

    long long covered = 0;
    for (std::size_t i = 0; i < plan.size(); ++i) {
        const PlannedStint& stint = plan[i];
        const std::string where = "stint " + std::to_string(i + 1) + ": ";
        if (stint.compoundIndex >= strategy.compounds().size()) {
            verdict.message = where + "compound index " + std::to_string(stint.compoundIndex) +
                              " is not in the compound set";
            return verdict;
        }
        const TireCompound& compound = strategy.compounds()[stint.compoundIndex];
        if (stint.laps <= 0) {
            verdict.message = where + "needs at least one lap, got " + std::to_string(stint.laps);
            return verdict;
        }
        if (stint.laps > compound.maxStintLaps()) {
            verdict.message = where + std::to_string(stint.laps) + " laps on " + compound.name() +
                              " exceeds its maximum stint of " +
                              std::to_string(compound.maxStintLaps());
            return verdict;
        }
        covered += stint.laps;
    }
    if (covered != raceLaps) {
        verdict.message = "the stints cover " + std::to_string(covered) + " laps but the race is " +
                          std::to_string(raceLaps) + " laps (a plan must cover it exactly)";
        return verdict;
    }
    verdict.valid = true;
    return verdict;
}

StrategyReport evaluateAgainst(const PitStrategy& strategy, const std::string& name,
                               const std::vector<PlannedStint>& plan, double optimumTotal) {
    StrategyReport report;
    report.name = name;
    report.plan = plan;
    report.optimumTotal = optimumTotal;

    const PlanValidation verdict = validatePlan(strategy, plan);
    if (!verdict.valid) {
        report.reason = verdict.message;
        return report;
    }
    const StrategyResult result = strategy.evaluatePlan(plan);
    if (!result.feasible) {
        report.reason = "evaluatePlan rejected a plan that validatePlan accepted";
        return report;
    }

    report.feasible = true;
    report.stops = result.stopCount();
    report.greenTime = result.greenTime;
    report.pitTime = result.pitTime;
    report.totalTime = result.totalTime;
    report.deltaVsOptimum = result.totalTime - optimumTotal;
    report.percentVsOptimum =
        optimumTotal > 0.0 ? 100.0 * report.deltaVsOptimum / optimumTotal : 0.0;

    for (const Stint& s : result.stints) {
        StintReport row;
        row.compoundIndex = s.compoundIndex;
        row.compoundName = s.compoundName;
        row.firstLap = s.startLap;
        row.lastLap = s.startLap + s.laps - 1;
        row.laps = s.laps;
        row.time = s.time;
        row.share = result.greenTime > 0.0 ? s.time / result.greenTime : 0.0;
        report.stints.push_back(row);

        auto used = std::find_if(report.compoundUsage.begin(), report.compoundUsage.end(),
                                 [&](const CompoundLaps& c) {
                                     return c.compoundIndex == s.compoundIndex;
                                 });
        if (used == report.compoundUsage.end()) {
            CompoundLaps c;
            c.compoundIndex = s.compoundIndex;
            c.compoundName = s.compoundName;
            c.laps = s.laps;
            report.compoundUsage.push_back(c);
        } else {
            used->laps += s.laps;
        }
    }
    return report;
}

StrategyReport evaluateCustom(const PitStrategy& strategy, const std::string& name,
                              const std::vector<PlannedStint>& plan) {
    const StrategyResult best = strategy.optimize();
    return evaluateAgainst(strategy, name, plan, best.feasible ? best.totalTime : 0.0);
}

std::vector<StrategyReport> compareStrategies(const PitStrategy& strategy,
                                              const std::vector<NamedPlan>& plans) {
    const StrategyResult best = strategy.optimize();
    const double optimumTotal = best.feasible ? best.totalTime : 0.0;
    std::vector<StrategyReport> reports;
    reports.reserve(plans.size());
    for (const NamedPlan& p : plans) {
        reports.push_back(evaluateAgainst(strategy, p.name, p.plan, optimumTotal));
    }
    return reports;
}

// ---------------------------------------------------------------------------
// Sweeps
// ---------------------------------------------------------------------------

namespace {

/// The sweep values from..to by step, computed as from + i*step so error does
/// not accumulate. O(P).
std::vector<double> sweepValues(double from, double to, double step) {
    if (!std::isfinite(from) || !std::isfinite(to) || !std::isfinite(step)) {
        throw std::invalid_argument("sweep: from, to and step must be finite numbers");
    }
    if (step <= 0.0) {
        throw std::invalid_argument("sweep: step must be greater than zero");
    }
    if (to < from) {
        throw std::invalid_argument("sweep: to must not be below from");
    }
    const double points = std::floor((to - from) / step + kTimeEpsilon) + 1.0;
    if (points > static_cast<double>(kMaxSweepPoints)) {
        throw std::invalid_argument("sweep: more than " + std::to_string(kMaxSweepPoints) +
                                    " points; use a larger step");
    }
    std::vector<double> values;
    const auto count = static_cast<std::size_t>(points);
    values.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        values.push_back(from + static_cast<double>(i) * step);
    }
    return values;
}

template <class MakeStrategy>
SweepResult runSweep(const std::vector<double>& values, MakeStrategy&& make) {
    SweepResult out;
    out.rows.reserve(values.size());
    for (std::size_t i = 0; i < values.size(); ++i) {
        const PitStrategy variant = make(values[i]);
        const StrategyResult best = variant.optimize();
        if (!best.feasible) {
            throw std::invalid_argument("sweep: no feasible plan at value " +
                                        std::to_string(values[i]));
        }
        SweepRow row;
        row.value = values[i];
        row.totalTime = best.totalTime;
        row.stops = best.stopCount();
        row.plan = planOf(best);
        if (i > 0) {
            row.deltaFromPrevious = row.totalTime - out.rows[i - 1].totalTime;
            row.planChanged = !samePlan(row.plan, out.rows[i - 1].plan);
            if (!out.firstChange && !samePlan(row.plan, out.rows[0].plan)) {
                out.firstChange = i;
            }
        }
        out.rows.push_back(std::move(row));
    }
    return out;
}

int wholeNumber(double value, const char* what) {
    const double rounded = std::round(value);
    if (std::fabs(value - rounded) > kTimeEpsilon || std::fabs(rounded) > 1.0e6) {
        throw std::invalid_argument(std::string("compoundSweep: ") + what +
                                    " values must be whole numbers");
    }
    return static_cast<int>(rounded);
}

}  // namespace

SweepResult pitLossSweep(const PitStrategy& strategy, double from, double to, double step) {
    if (std::isfinite(from) && from < 0.0) {
        throw std::invalid_argument("pitLossSweep: pit loss cannot be negative");
    }
    const std::vector<double> values = sweepValues(from, to, step);
    return runSweep(values, [&](double pitLoss) {
        Track track = strategy.track();
        track.setPitLoss(pitLoss);
        return PitStrategy(track, strategy.compounds());
    });
}

SweepResult compoundSweep(const PitStrategy& strategy, std::size_t compoundIndex,
                          CompoundParameter parameter, double from, double to, double step) {
    if (compoundIndex >= strategy.compounds().size()) {
        throw std::out_of_range("compoundSweep: compound index " + std::to_string(compoundIndex) +
                                " is not in the compound set");
    }
    const std::vector<double> values = sweepValues(from, to, step);
    const TireCompound& base = strategy.compounds()[compoundIndex];

    return runSweep(values, [&](double v) {
        double basePace = base.basePace();
        double degradation = base.degradationPerLap();
        int maxStint = base.maxStintLaps();
        int cliffLap = base.cliffLap();
        double cliffRate = base.cliffDegradationPerLap();
        switch (parameter) {
            case CompoundParameter::BasePace: basePace = v; break;
            case CompoundParameter::Degradation: degradation = v; break;
            case CompoundParameter::MaxStint: maxStint = wholeNumber(v, "max stint"); break;
            case CompoundParameter::CliffLap: cliffLap = wholeNumber(v, "cliff lap"); break;
            case CompoundParameter::CliffDegradation: cliffRate = v; break;
        }
        std::vector<TireCompound> compounds = strategy.compounds();
        compounds[compoundIndex] =
            TireCompound(base.name(), basePace, degradation, maxStint, cliffLap, cliffRate);
        return PitStrategy(strategy.track(), compounds);
    });
}

// ---------------------------------------------------------------------------
// K cheapest plans
// ---------------------------------------------------------------------------

namespace {

struct KEntry {
    double cost = 0.0;
    int prevLap = 0;
    std::size_t compound = 0;
    int laps = 0;
    std::size_t prevRank = 0;
};

// Exact order: cost, then compound, stint length, predecessor rank. No
// tolerance here: a tolerance compare is not transitive, so it is not a valid
// sort key.
bool kEntryBefore(const KEntry& a, const KEntry& b) {
    if (a.cost < b.cost) return true;
    if (b.cost < a.cost) return false;
    if (a.compound != b.compound) return a.compound < b.compound;
    if (a.laps != b.laps) return a.laps < b.laps;
    return a.prevRank < b.prevRank;
}

}  // namespace

std::vector<StrategyReport> kBestPlans(const PitStrategy& strategy, std::size_t count) {
    if (count == 0 || count > kMaxAlternatives) {
        throw std::invalid_argument("kBestPlans: count must be between 1 and " +
                                    std::to_string(kMaxAlternatives));
    }
    const StrategyResult best = strategy.optimize();
    if (!best.feasible) {
        return {};
    }
    const std::vector<PlannedStint> optimalPlan = planOf(best);

    std::vector<StrategyReport> reports;
    reports.push_back(evaluateAgainst(strategy, "optimal", optimalPlan, best.totalTime));

    const int raceLaps = strategy.track().totalLaps();
    if (raceLaps <= 0) {
        return reports;
    }

    // Ask for one extra: the optimizer's plan may be among them and is dropped.
    const std::size_t keep = count + 1U;
    const double pitLoss = strategy.track().pitLoss();
    std::vector<std::vector<KEntry>> table(static_cast<std::size_t>(raceLaps) + 1U);
    table[0].push_back(KEntry{});

    for (int lap = 1; lap <= raceLaps; ++lap) {
        std::vector<KEntry> candidates;
        for (std::size_t c = 0; c < strategy.compounds().size(); ++c) {
            const int longest = std::min(lap, strategy.compounds()[c].maxStintLaps());
            for (int len = 1; len <= longest; ++len) {
                const int prev = lap - len;
                const std::vector<KEntry>& from = table[static_cast<std::size_t>(prev)];
                const double stint = strategy.stintCost(c, len) + (prev > 0 ? pitLoss : 0.0);
                for (std::size_t r = 0; r < from.size(); ++r) {
                    KEntry e;
                    e.cost = from[r].cost + stint;
                    e.prevLap = prev;
                    e.compound = c;
                    e.laps = len;
                    e.prevRank = r;
                    candidates.push_back(e);
                }
            }
        }
        std::sort(candidates.begin(), candidates.end(), kEntryBefore);
        if (candidates.size() > keep) {
            candidates.resize(keep);
        }
        table[static_cast<std::size_t>(lap)] = std::move(candidates);
    }

    const std::vector<KEntry>& finals = table[static_cast<std::size_t>(raceLaps)];
    for (std::size_t rank = 0; rank < finals.size() && reports.size() < count; ++rank) {
        std::vector<PlannedStint> plan;
        int lap = raceLaps;
        std::size_t r = rank;
        while (lap > 0) {
            const KEntry& e = table[static_cast<std::size_t>(lap)][r];
            PlannedStint s;
            s.compoundIndex = e.compound;
            s.laps = e.laps;
            plan.push_back(s);
            r = e.prevRank;
            lap = e.prevLap;
        }
        std::reverse(plan.begin(), plan.end());
        if (samePlan(plan, optimalPlan)) {
            continue;
        }
        reports.push_back(evaluateAgainst(strategy, "alternative " + std::to_string(reports.size()),
                                          plan, best.totalTime));
    }
    return reports;
}

// ---------------------------------------------------------------------------
// Best plan per stop count
// ---------------------------------------------------------------------------

std::vector<StopCountResult> bestForStopCount(const PitStrategy& strategy, int maxStops) {
    const int raceLaps = strategy.track().totalLaps();
    if (maxStops < 0 || maxStops > std::max(raceLaps, 0)) {
        throw std::invalid_argument("bestForStopCount: maxStops must be between 0 and the " +
                                    std::to_string(raceLaps) + " race laps");
    }
    const StrategyResult optimum = strategy.optimize();
    const double optimumTotal = optimum.feasible ? optimum.totalTime : 0.0;

    struct Step {
        std::size_t compound = 0;
        int laps = 0;
    };
    const auto states = static_cast<std::size_t>(std::max(raceLaps, 0)) + 1U;
    const auto stintCounts = static_cast<std::size_t>(maxStops) + 1U;
    // f[m][l]: cheapest way to run l laps in exactly m stints (m >= 1).
    std::vector<std::vector<double>> f(stintCounts + 1U, std::vector<double>(states, kUnreachable));
    std::vector<std::vector<Step>> choice(stintCounts + 1U, std::vector<Step>(states));
    const double pitLoss = strategy.track().pitLoss();

    for (std::size_t m = 1; m <= stintCounts; ++m) {
        for (int lap = 1; lap <= raceLaps; ++lap) {
            const auto l = static_cast<std::size_t>(lap);
            for (std::size_t c = 0; c < strategy.compounds().size(); ++c) {
                const int longest = std::min(lap, strategy.compounds()[c].maxStintLaps());
                for (int len = 1; len <= longest; ++len) {
                    double candidate;
                    if (m == 1) {
                        if (len != lap) continue;  // a single stint is the whole distance
                        candidate = strategy.stintCost(c, len);
                    } else {
                        const double before = f[m - 1][static_cast<std::size_t>(lap - len)];
                        if (!isReachable(before)) continue;
                        candidate = before + strategy.stintCost(c, len) + pitLoss;
                    }
                    if (definitelyLess(candidate, f[m][l])) {
                        f[m][l] = candidate;
                        choice[m][l] = Step{c, len};
                    }
                }
            }
        }
    }

    std::vector<StopCountResult> out;
    for (int stops = 0; stops <= maxStops; ++stops) {
        StopCountResult row;
        row.stops = stops;
        const std::string name = std::to_string(stops) + "-stop";
        if (raceLaps <= 0) {
            row.feasible = (stops == 0);
            if (row.feasible) {
                row.report = evaluateAgainst(strategy, name, {}, optimumTotal);
            } else {
                row.reason = "a race with no laps has no stops";
            }
            out.push_back(std::move(row));
            continue;
        }
        const auto m = static_cast<std::size_t>(stops) + 1U;
        const auto last = static_cast<std::size_t>(raceLaps);
        if (!isReachable(f[m][last])) {
            row.reason = "no plan with exactly " + std::to_string(stops) + " stop(s) covers " +
                         std::to_string(raceLaps) + " laps (stints are limited by each " +
                         "compound's maximum, and every stint needs a lap)";
            out.push_back(std::move(row));
            continue;
        }
        std::vector<PlannedStint> plan;
        int lap = raceLaps;
        for (std::size_t mm = m; mm >= 1; --mm) {
            const Step s = choice[mm][static_cast<std::size_t>(lap)];
            PlannedStint p;
            p.compoundIndex = s.compound;
            p.laps = s.laps;
            plan.push_back(p);
            lap -= s.laps;
        }
        std::reverse(plan.begin(), plan.end());
        row.feasible = true;
        row.report = evaluateAgainst(strategy, name, plan, optimumTotal);
        out.push_back(std::move(row));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Comparison sets
// ---------------------------------------------------------------------------

std::vector<StrategyReport> compoundOnlyBest(const PitStrategy& strategy) {
    const StrategyResult optimum = strategy.optimize();
    const double optimumTotal = optimum.feasible ? optimum.totalTime : 0.0;
    std::vector<StrategyReport> reports;
    for (std::size_t c = 0; c < strategy.compounds().size(); ++c) {
        const PitStrategy single(strategy.track(), {strategy.compounds()[c]});
        std::vector<PlannedStint> plan = planOf(single.optimize());
        for (PlannedStint& s : plan) {
            s.compoundIndex = c;  // back to the full strategy's numbering
        }
        reports.push_back(evaluateAgainst(strategy, "only-" + compoundLabel(strategy, c), plan,
                                          optimumTotal));
    }
    return reports;
}

std::vector<NamedPlan> standardCandidates(const PitStrategy& strategy, int maxStops) {
    std::vector<NamedPlan> out;
    const auto add = [&](const std::string& name, const std::vector<PlannedStint>& plan) {
        for (const NamedPlan& existing : out) {
            if (samePlan(existing.plan, plan)) {
                return;
            }
        }
        out.push_back(NamedPlan{name, plan});
    };

    add("optimal", planOf(strategy.optimize()));
    for (std::size_t c = 0; c < strategy.compounds().size(); ++c) {
        const StrategyResult single = strategy.singleCompoundPlan(c);
        if (single.feasible) {
            add("all-" + compoundLabel(strategy, c), planOf(single));
        }
    }
    // A race of N laps has at most N-1 stops, so asking for more is clamped
    // rather than rejected: the set is "up to maxStops", not "exactly".
    const int cappedStops = std::min(std::max(maxStops, 0), std::max(strategy.track().totalLaps(), 0));
    for (const StopCountResult& row : bestForStopCount(strategy, cappedStops)) {
        if (row.feasible) {
            add(row.report.name, row.report.plan);
        }
    }
    // The best plan that uses only one compound, per compound (often the same
    // plan as "all-<compound>", in which case the earlier name is kept).
    for (const StrategyReport& only : compoundOnlyBest(strategy)) {
        if (only.feasible) {
            add(only.name, only.plan);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Clean air against traffic
// ---------------------------------------------------------------------------

namespace {

RaceSimulator runField(const PitStrategy& strategy, const std::vector<NamedPlan>& plans,
                       const OvertakeRule& rule) {
    RaceSimulator sim(strategy.track(), strategy.compounds());
    sim.setOvertakeRule(rule);
    int number = 1;
    for (const NamedPlan& p : plans) {
        sim.addDriver(number++, p.name, strategy.evaluatePlan(p.plan));
    }
    sim.runToFinish();
    return sim;
}

}  // namespace

std::vector<TrafficComparisonRow> cleanAirVsTraffic(const PitStrategy& strategy,
                                                    const std::vector<NamedPlan>& plans,
                                                    const OvertakeRule& rule) {
    if (plans.empty()) {
        throw std::invalid_argument("cleanAirVsTraffic: at least one plan is required");
    }
    for (const NamedPlan& p : plans) {
        const PlanValidation verdict = validatePlan(strategy, p.plan);
        if (!verdict.valid) {
            throw std::invalid_argument("cleanAirVsTraffic: plan \"" + p.name +
                                        "\" is infeasible: " + verdict.message);
        }
    }

    const OvertakeRule off;  // disabled by default
    const RaceSimulator clean = runField(strategy, plans, off);
    const RaceSimulator busy = runField(strategy, plans, rule);

    std::vector<int> cleanPosition(plans.size(), 0);
    std::vector<int> busyPosition(plans.size(), 0);
    for (const Standing& row : clean.standings()) {
        cleanPosition[row.driverIndex] = row.position;
    }
    for (const Standing& row : busy.standings()) {
        busyPosition[row.driverIndex] = row.position;
    }

    std::vector<TrafficComparisonRow> rows;
    for (std::size_t i = 0; i < plans.size(); ++i) {
        TrafficComparisonRow row;
        row.name = plans[i].name;
        row.cleanTime = clean.drivers()[i].cumulativeTime();
        row.trafficTime = busy.drivers()[i].cumulativeTime();
        row.trafficLoss = busy.drivers()[i].trafficLoss();
        row.cleanPosition = cleanPosition[i];
        row.trafficPosition = busyPosition[i];
        rows.push_back(std::move(row));
    }
    return rows;
}

}  // namespace boxbox
