// Box Box: checks for the strategy lab (plan sec. 4.1, 4.6 and 8.3).
//
// Covers validatePlan, evaluateCustom, pitLossSweep, compoundSweep, kBestPlans,
// bestForStopCount, compoundOnlyBest, compareStrategies, standardCandidates and
// cleanAirVsTraffic. The references are written without the DP or the prefix
// table: exhaustive enumeration of every plan, costed lap by lap. Same harness
// style as test_main.cpp: a failing check is recorded and the run carries on.

#include "StrategyTests.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "boxbox/Common.hpp"
#include "boxbox/LapTime.hpp"
#include "boxbox/PitStrategy.hpp"
#include "boxbox/RaceSimulator.hpp"
#include "boxbox/StrategyAnalytics.hpp"
#include "boxbox/TireCompound.hpp"
#include "boxbox/Track.hpp"

namespace {

using boxbox::CompoundParameter;
using boxbox::NamedPlan;
using boxbox::OvertakeRule;
using boxbox::PitStrategy;
using boxbox::PlannedStint;
using boxbox::StrategyReport;
using boxbox::TireCompound;
using boxbox::Track;

// ---------------------------------------------------------------------------
// Harness
// ---------------------------------------------------------------------------

StrategyTestTally g_tally;
std::string g_section;

void beginSection(const std::string& name) {
    g_section = name;
    std::cout << "\n== " << name << '\n';
}

void reportCheck(bool ok, const char* expression, const char* file, int line,
                 const std::string& detail) {
    ++g_tally.checks;
    if (ok) {
        return;
    }
    ++g_tally.failures;
    std::cout << "   FAIL  " << file << ':' << line << "  in " << g_section << '\n';
    std::cout << "         " << expression << '\n';
    if (!detail.empty()) {
        std::cout << "         " << detail << '\n';
    }
}

std::string describe(double actual, double expected, double tolerance) {
    std::ostringstream out;
    out << std::setprecision(17) << "actual " << actual << "   expected " << expected
        << "   diff " << std::fabs(actual - expected) << "   tol " << tolerance;
    return out.str();
}

#define SCHECK(expr) reportCheck((expr), #expr, __FILE__, __LINE__, std::string())

#define SCHECK_NEAR(actual, expected, tolerance)                                        \
    reportCheck(std::fabs((actual) - (expected)) <= (tolerance),                        \
                #actual " ~= " #expected, __FILE__, __LINE__,                           \
                describe((actual), (expected), (tolerance)))

#define SCHECK_THROWS(statement, ExType)                                                \
    do {                                                                                \
        bool threw = false;                                                             \
        try {                                                                           \
            statement;                                                                  \
        } catch (const ExType&) {                                                       \
            threw = true;                                                               \
        } catch (...) {                                                                 \
        }                                                                               \
        reportCheck(threw, "throws " #ExType ": " #statement, __FILE__, __LINE__,       \
                    std::string());                                                     \
    } while (false)

/// Same seeded LCG as the other test files: inputs vary, the run does not.
class Lcg {
public:
    explicit Lcg(std::uint64_t seed) noexcept : state_(seed) {}
    std::uint64_t next() noexcept {
        state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
        return state_ >> 33U;
    }
    int nextInt(int low, int high) noexcept {
        const std::uint64_t span = static_cast<std::uint64_t>(high - low + 1);
        return low + static_cast<int>(next() % span);
    }
    double nextDouble(double low, double high) noexcept {
        const double unit = static_cast<double>(next() % 1000001ULL) / 1000000.0;
        return low + (high - low) * unit;
    }

private:
    std::uint64_t state_;
};

// ---------------------------------------------------------------------------
// References, written without the DP and without the prefix table
// ---------------------------------------------------------------------------

/// Lap by lap: sum canonicalLapTime over each stint's ages, plus pit loss
/// between stints.
double directCost(const Track& track, const std::vector<TireCompound>& compounds,
                  const std::vector<PlannedStint>& plan) {
    double total = 0.0;
    for (std::size_t s = 0; s < plan.size(); ++s) {
        for (int age = 0; age < plan[s].laps; ++age) {
            total += boxbox::canonicalLapTime(track, compounds[plan[s].compoundIndex], age);
        }
        if (s + 1U < plan.size()) {
            total += track.pitLoss();
        }
    }
    return total;
}

void enumerate(const std::vector<TireCompound>& compounds, int remaining,
               std::vector<PlannedStint>& current, std::vector<std::vector<PlannedStint>>& out) {
    if (remaining == 0) {
        out.push_back(current);
        return;
    }
    for (std::size_t c = 0; c < compounds.size(); ++c) {
        for (int len = 1; len <= std::min(remaining, compounds[c].maxStintLaps()); ++len) {
            current.push_back(PlannedStint{c, len});
            enumerate(compounds, remaining - len, current, out);
            current.pop_back();
        }
    }
}

/// Every legal plan for the race, with its directly summed cost, cheapest first.
struct Ranked {
    std::vector<PlannedStint> plan;
    double cost = 0.0;
};

std::vector<Ranked> allPlans(const Track& track, const std::vector<TireCompound>& compounds) {
    std::vector<std::vector<PlannedStint>> plans;
    std::vector<PlannedStint> current;
    enumerate(compounds, track.totalLaps(), current, plans);
    std::vector<Ranked> ranked;
    for (auto& p : plans) {
        Ranked r;
        r.cost = directCost(track, compounds, p);
        r.plan = std::move(p);
        ranked.push_back(std::move(r));
    }
    std::stable_sort(ranked.begin(), ranked.end(),
                     [](const Ranked& a, const Ranked& b) { return a.cost < b.cost; });
    return ranked;
}

/// A small random race: 1..8 laps, 1..3 compounds with stints of 1..5 laps.
struct Case {
    Track track{"Case", 1, 0.0};
    std::vector<TireCompound> compounds;
};

Case randomCase(Lcg& rng, int maxLaps) {
    Case c;
    c.track = Track("Case", rng.nextInt(1, maxLaps), rng.nextDouble(0.0, 30.0));
    c.track.addSector("S1", rng.nextDouble(20.0, 60.0));
    c.track.addSector("S2", rng.nextDouble(20.0, 60.0));
    const int count = rng.nextInt(1, 3);
    for (int i = 0; i < count; ++i) {
        const int cliff = (rng.nextInt(0, 2) == 0) ? -1 : rng.nextInt(0, 4);
        c.compounds.push_back(TireCompound("C" + std::to_string(i), rng.nextDouble(-1.0, 1.0),
                                           rng.nextDouble(0.0, 0.4), rng.nextInt(1, 5), cliff,
                                           rng.nextDouble(0.0, 0.8)));
    }
    return c;
}

/// The hand-worked race from the driver-analytics tests: one 10 s sector, 4
/// laps, 5 s pit loss. A: +0 s, 1 s/lap wear, up to 2 laps. B: +2 s, no wear,
/// up to 4 laps.
struct Hand {
    Track track{"Hand", 4, 5.0};
    std::vector<TireCompound> compounds;
    Hand() {
        track.addSector("S1", 10.0);
        compounds = {TireCompound("A", 0.0, 1.0, 2), TireCompound("B", 2.0, 0.0, 4)};
    }
};

bool sameCosts(const std::vector<StrategyReport>& reports, const std::vector<Ranked>& ranked,
               std::size_t n) {
    if (reports.size() != n) {
        return false;
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (std::fabs(reports[i].totalTime - ranked[i].cost) > 1e-9) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

void testValidateAndEvaluate() {
    beginSection("validatePlan and evaluateCustom: every reason, and the hand-worked race");

    const Hand h;
    const PitStrategy strategy(h.track, h.compounds);

    SCHECK(boxbox::validatePlan(strategy, {PlannedStint{0, 2}, PlannedStint{1, 2}}).valid);
    SCHECK(boxbox::validatePlan(strategy, {PlannedStint{0, 2}, PlannedStint{1, 2}}).message.empty());

    const auto bad = [&](const std::vector<PlannedStint>& plan, const std::string& fragment) {
        const boxbox::PlanValidation v = boxbox::validatePlan(strategy, plan);
        SCHECK(!v.valid);
        SCHECK(v.message.find(fragment) != std::string::npos);
    };
    bad({}, "no stints");
    bad({PlannedStint{0, 0}, PlannedStint{1, 4}}, "at least one lap");
    bad({PlannedStint{0, -1}, PlannedStint{1, 5}}, "at least one lap");
    bad({PlannedStint{5, 4}}, "not in the compound set");
    bad({PlannedStint{0, 3}, PlannedStint{1, 1}}, "maximum stint");
    bad({PlannedStint{0, 2}, PlannedStint{1, 1}}, "cover 3 laps");
    bad({PlannedStint{1, 4}, PlannedStint{1, 1}}, "cover 5 laps");

    // Hand worked: A2,B2 = 10+11+12+12 + 5 = 50. B4 = 48. A2,A2 = 21+21 + 5 = 47.
    const StrategyReport mixed =
        boxbox::evaluateCustom(strategy, "mixed", {PlannedStint{0, 2}, PlannedStint{1, 2}});
    SCHECK(mixed.feasible);
    SCHECK(mixed.reason.empty());
    SCHECK_NEAR(mixed.totalTime, 50.0, 1e-9);
    SCHECK_NEAR(mixed.greenTime, 45.0, 1e-9);
    SCHECK_NEAR(mixed.pitTime, 5.0, 1e-9);
    SCHECK(mixed.stops == 1);
    SCHECK_NEAR(mixed.optimumTotal, 47.0, 1e-9);
    SCHECK_NEAR(mixed.deltaVsOptimum, 3.0, 1e-9);
    SCHECK_NEAR(mixed.percentVsOptimum, 100.0 * 3.0 / 47.0, 1e-9);
    SCHECK(mixed.stints.size() == 2U);
    if (mixed.stints.size() == 2U) {
        SCHECK(mixed.stints[0].firstLap == 1 && mixed.stints[0].lastLap == 2);
        SCHECK(mixed.stints[1].firstLap == 3 && mixed.stints[1].lastLap == 4);
        SCHECK_NEAR(mixed.stints[0].time, 21.0, 1e-9);
        SCHECK_NEAR(mixed.stints[1].time, 24.0, 1e-9);
        SCHECK_NEAR(mixed.stints[0].share + mixed.stints[1].share, 1.0, 1e-9);
    }
    SCHECK(mixed.compoundUsage.size() == 2U);

    const StrategyReport one = boxbox::evaluateCustom(strategy, "B only", {PlannedStint{1, 4}});
    SCHECK_NEAR(one.totalTime, 48.0, 1e-9);
    SCHECK(one.stops == 0);

    // The optimum itself is a zero-delta plan; a repeated compound merges its laps.
    const StrategyReport twoA =
        boxbox::evaluateCustom(strategy, "AA", {PlannedStint{0, 2}, PlannedStint{0, 2}});
    SCHECK_NEAR(twoA.totalTime, 47.0, 1e-9);
    SCHECK_NEAR(twoA.deltaVsOptimum, 0.0, 1e-9);
    SCHECK(twoA.compoundUsage.size() == 1U && twoA.compoundUsage[0].laps == 4);

    // An infeasible plan is reported with its reason, never as a number.
    const StrategyReport broken = boxbox::evaluateCustom(strategy, "bad", {PlannedStint{0, 3}});
    SCHECK(!broken.feasible);
    SCHECK(!broken.reason.empty());
    SCHECK(broken.stints.empty());

    // Zero-lap race: only the empty plan is valid.
    Track none("None", 0, 5.0);
    none.addSector("S1", 10.0);
    const PitStrategy empty(none, h.compounds);
    SCHECK(boxbox::validatePlan(empty, {}).valid);
    SCHECK(!boxbox::validatePlan(empty, {PlannedStint{0, 1}}).valid);
}

void testEvaluateAgainstDirectSummation() {
    beginSection("evaluateCustom matches lap-by-lap summation over every plan of 60 seeded races");

    Lcg rng(11ULL);
    for (int trial = 0; trial < 60; ++trial) {
        const Case c = randomCase(rng, 7);
        const PitStrategy strategy(c.track, c.compounds);
        const std::vector<Ranked> plans = allPlans(c.track, c.compounds);
        SCHECK(!plans.empty());
        const double best = plans.front().cost;
        SCHECK_NEAR(strategy.optimize().totalTime, best, 1e-9);
        for (std::size_t i = 0; i < plans.size() && i < 25U; ++i) {
            const StrategyReport r = boxbox::evaluateCustom(strategy, "p", plans[i].plan);
            SCHECK(r.feasible);
            SCHECK_NEAR(r.totalTime, plans[i].cost, 1e-9);
            SCHECK_NEAR(r.greenTime + r.pitTime, r.totalTime, 1e-9);
            SCHECK(r.deltaVsOptimum >= -1e-9);
            int laps = 0;
            for (const boxbox::StintReport& s : r.stints) laps += s.laps;
            SCHECK(laps == c.track.totalLaps());
        }
    }
}

void testPitLossSweep() {
    beginSection("pitLossSweep: equals repeated optimize(), and the extremes");

    Lcg rng(22ULL);
    for (int trial = 0; trial < 20; ++trial) {
        const Case c = randomCase(rng, 12);
        const PitStrategy strategy(c.track, c.compounds);
        const double step = rng.nextDouble(2.0, 15.0);
        const boxbox::SweepResult sweep = boxbox::pitLossSweep(strategy, 0.0, 60.0, step);
        const std::size_t expected = static_cast<std::size_t>(std::floor(60.0 / step + 1e-9)) + 1U;
        SCHECK(sweep.rows.size() == expected);

        std::size_t firstChange = sweep.rows.size();
        for (std::size_t i = 0; i < sweep.rows.size(); ++i) {
            const boxbox::SweepRow& row = sweep.rows[i];
            Track t = c.track;
            t.setPitLoss(row.value);
            const PitStrategy independent(t, c.compounds);
            const boxbox::StrategyResult best = independent.optimize();
            SCHECK_NEAR(row.value, static_cast<double>(i) * step, 1e-9);
            SCHECK_NEAR(row.totalTime, best.totalTime, 1e-9);
            SCHECK(row.stops == best.stopCount());
            SCHECK_NEAR(row.totalTime, directCost(t, c.compounds, row.plan), 1e-9);
            if (i == 0) {
                SCHECK(!row.planChanged);
                SCHECK_NEAR(row.deltaFromPrevious, 0.0, 0.0);
            } else {
                SCHECK_NEAR(row.deltaFromPrevious, row.totalTime - sweep.rows[i - 1].totalTime, 1e-9);
                SCHECK(row.deltaFromPrevious >= -1e-9);  // a costlier stop never speeds the race
                SCHECK(row.planChanged == !boxbox::samePlan(row.plan, sweep.rows[i - 1].plan));
                if (!boxbox::samePlan(row.plan, sweep.rows[0].plan) && firstChange == sweep.rows.size()) {
                    firstChange = i;
                }
            }
        }
        SCHECK(sweep.firstChange.has_value() == (firstChange != sweep.rows.size()));
        if (sweep.firstChange) {
            SCHECK(*sweep.firstChange == firstChange);
        }
    }

    // Default compounds, 30 laps: free stops mean stops; a huge loss means none.
    Track track("T", 30, 22.0);
    track.addSector("S1", 90.0);
    const PitStrategy strategy(track, boxbox::defaultCompounds());
    const boxbox::SweepResult wide = boxbox::pitLossSweep(strategy, 0.0, 1.0e6, 1.0e6);
    SCHECK(wide.rows.size() == 2U);
    SCHECK(wide.rows[0].stops > 0);
    SCHECK(wide.rows[1].stops == 0);
    SCHECK(wide.firstChange.has_value() && *wide.firstChange == 1U);
    SCHECK(wide.rows[1].planChanged);

    // Single point, and an end that is not on a step.
    SCHECK(boxbox::pitLossSweep(strategy, 5.0, 5.0, 1.0).rows.size() == 1U);
    SCHECK(boxbox::pitLossSweep(strategy, 0.0, 10.0, 4.0).rows.size() == 3U);  // 0, 4, 8

    SCHECK_THROWS(boxbox::pitLossSweep(strategy, 0.0, 10.0, 0.0), std::invalid_argument);
    SCHECK_THROWS(boxbox::pitLossSweep(strategy, 0.0, 10.0, -1.0), std::invalid_argument);
    SCHECK_THROWS(boxbox::pitLossSweep(strategy, -1.0, 10.0, 1.0), std::invalid_argument);
    SCHECK_THROWS(boxbox::pitLossSweep(strategy, 10.0, 5.0, 1.0), std::invalid_argument);
    SCHECK_THROWS(boxbox::pitLossSweep(strategy, 0.0, std::nan(""), 1.0), std::invalid_argument);
    SCHECK_THROWS(boxbox::pitLossSweep(strategy, 0.0, 10.0, std::nan("")), std::invalid_argument);
    SCHECK_THROWS(boxbox::pitLossSweep(strategy, 0.0, std::numeric_limits<double>::infinity(), 1.0),
                  std::invalid_argument);
    SCHECK_THROWS(boxbox::pitLossSweep(strategy, 0.0, 5000.0, 1.0), std::invalid_argument);  // > cap
}

void testCompoundSweep() {
    beginSection("compoundSweep: equals a hand-rebuilt compound and optimize()");

    Track track("T", 30, 22.0);
    track.addSector("S1", 90.0);
    const PitStrategy strategy(track, boxbox::defaultCompounds());

    // Degradation of SOFT (index 0) from 0 to 0.4.
    const boxbox::SweepResult deg =
        boxbox::compoundSweep(strategy, 0, CompoundParameter::Degradation, 0.0, 0.4, 0.1);
    SCHECK(deg.rows.size() == 5U);
    for (const boxbox::SweepRow& row : deg.rows) {
        std::vector<TireCompound> compounds = boxbox::defaultCompounds();
        const TireCompound& soft = compounds[0];
        compounds[0] = TireCompound(soft.name(), soft.basePace(), row.value, soft.maxStintLaps(),
                                    soft.cliffLap(), soft.cliffDegradationPerLap());
        const PitStrategy independent(track, compounds);
        SCHECK_NEAR(row.totalTime, independent.optimize().totalTime, 1e-9);
        SCHECK(row.stops == independent.optimize().stopCount());
    }
    // A much worse SOFT can only slow the race down.
    for (std::size_t i = 1; i < deg.rows.size(); ++i) {
        SCHECK(deg.rows[i].totalTime >= deg.rows[i - 1].totalTime - 1e-9);
    }

    // Base pace: a very slow SOFT stops being chosen at all.
    const boxbox::SweepResult pace =
        boxbox::compoundSweep(strategy, 0, CompoundParameter::BasePace, -0.6, 5.0, 5.6);
    SCHECK(pace.rows.size() == 2U);
    SCHECK(pace.rows[0].plan.front().compoundIndex == 0U);
    SCHECK(pace.rows[1].plan.front().compoundIndex != 0U || pace.rows[1].plan.size() > 1U);

    // Max stint: shrinking HARD's stint to 1 lap forces many stops when it is the
    // only fast option; whole numbers only.
    const boxbox::SweepResult stint =
        boxbox::compoundSweep(strategy, 2, CompoundParameter::MaxStint, 5.0, 8.0, 1.0);
    SCHECK(stint.rows.size() == 4U);
    SCHECK_THROWS(boxbox::compoundSweep(strategy, 2, CompoundParameter::MaxStint, 5.5, 8.0, 1.0),
                  std::invalid_argument);
    SCHECK_THROWS(boxbox::compoundSweep(strategy, 2, CompoundParameter::CliffLap, 5.0, 8.5, 0.5),
                  std::invalid_argument);
    SCHECK(boxbox::compoundSweep(strategy, 1, CompoundParameter::CliffLap, 10.0, 12.0, 1.0)
               .rows.size() == 3U);
    SCHECK(boxbox::compoundSweep(strategy, 1, CompoundParameter::CliffDegradation, 0.0, 0.2, 0.1)
               .rows.size() == 3U);

    // Rejected inputs.
    SCHECK_THROWS(boxbox::compoundSweep(strategy, 9, CompoundParameter::Degradation, 0.0, 1.0, 0.5),
                  std::out_of_range);
    SCHECK_THROWS(boxbox::compoundSweep(strategy, 0, CompoundParameter::Degradation, -0.2, 0.2, 0.1),
                  std::invalid_argument);  // negative wear
    SCHECK_THROWS(boxbox::compoundSweep(strategy, 0, CompoundParameter::MaxStint, 0.0, 2.0, 1.0),
                  std::invalid_argument);  // a 0-lap stint
    SCHECK_THROWS(boxbox::compoundSweep(strategy, 0, CompoundParameter::BasePace, 0.0, 1.0, 0.0),
                  std::invalid_argument);
}

void testKBestPlans() {
    beginSection("kBestPlans: the K cheapest distinct plans, against exhaustive enumeration");

    const Hand h;
    const PitStrategy hand(h.track, h.compounds);
    const std::vector<StrategyReport> small = boxbox::kBestPlans(hand, 3);
    SCHECK(small.size() == 3U);
    // The three cheapest plans of the hand race: 47 (A2 A2), 48 (B4), then the next.
    const std::vector<Ranked> handAll = allPlans(h.track, h.compounds);
    SCHECK(sameCosts(small, handAll, 3));
    SCHECK_NEAR(small[0].totalTime, 47.0, 1e-9);
    SCHECK_NEAR(small[1].totalTime, 48.0, 1e-9);
    SCHECK(boxbox::samePlan(small[0].plan, hand.optimize().stints.empty()
                                               ? std::vector<PlannedStint>{}
                                               : std::vector<PlannedStint>{
                                                     PlannedStint{hand.optimize().stints[0].compoundIndex,
                                                                  hand.optimize().stints[0].laps},
                                                     PlannedStint{hand.optimize().stints[1].compoundIndex,
                                                                  hand.optimize().stints[1].laps}}));

    Lcg rng(33ULL);
    for (int trial = 0; trial < 60; ++trial) {
        const Case c = randomCase(rng, 8);
        const PitStrategy strategy(c.track, c.compounds);
        const std::vector<Ranked> all = allPlans(c.track, c.compounds);
        for (const std::size_t k : {std::size_t{1}, std::size_t{4}, std::size_t{12}, std::size_t{50}}) {
            const std::vector<StrategyReport> got = boxbox::kBestPlans(strategy, k);
            const std::size_t expectedCount = std::min(k, all.size());
            SCHECK(got.size() == expectedCount);
            if (got.size() != expectedCount) {
                continue;
            }
            SCHECK(sameCosts(got, all, expectedCount));

            // Rank 0 is the optimizer's own plan.
            std::vector<PlannedStint> optimal;
            for (const boxbox::Stint& s : strategy.optimize().stints) {
                optimal.push_back(PlannedStint{s.compoundIndex, s.laps});
            }
            SCHECK(boxbox::samePlan(got[0].plan, optimal));

            bool distinct = true;
            bool ordered = true;
            bool honest = true;
            for (std::size_t i = 0; i < got.size(); ++i) {
                honest = honest && got[i].feasible &&
                         std::fabs(got[i].totalTime - directCost(c.track, c.compounds, got[i].plan)) <= 1e-9;
                if (i > 0) {
                    ordered = ordered && got[i].totalTime >= got[i - 1].totalTime - boxbox::kTimeEpsilon;
                }
                for (std::size_t j = 0; j < i; ++j) {
                    distinct = distinct && !boxbox::samePlan(got[i].plan, got[j].plan);
                }
            }
            SCHECK(distinct);
            SCHECK(ordered);
            SCHECK(honest);
        }
    }

    // Every plan of a tiny race: asking for more than exist returns them all.
    Track tiny("Tiny", 3, 4.0);
    tiny.addSector("S1", 10.0);
    const std::vector<TireCompound> two{TireCompound("A", 0.0, 0.5, 3), TireCompound("B", 1.0, 0.0, 2)};
    const PitStrategy tinyStrategy(tiny, two);
    const std::vector<Ranked> tinyAll = allPlans(tiny, two);
    SCHECK(boxbox::kBestPlans(tinyStrategy, 50).size() == tinyAll.size());

    // Reproducible.
    const std::vector<StrategyReport> a = boxbox::kBestPlans(tinyStrategy, 6);
    const std::vector<StrategyReport> b = boxbox::kBestPlans(tinyStrategy, 6);
    bool same = a.size() == b.size();
    for (std::size_t i = 0; same && i < a.size(); ++i) {
        same = boxbox::samePlan(a[i].plan, b[i].plan) && a[i].totalTime == b[i].totalTime;
    }
    SCHECK(same);

    // Bounds.
    SCHECK_THROWS(boxbox::kBestPlans(tinyStrategy, 0), std::invalid_argument);
    SCHECK_THROWS(boxbox::kBestPlans(tinyStrategy, boxbox::kMaxAlternatives + 1U), std::invalid_argument);

    // Zero-lap race: just the empty optimum.
    Track none("None", 0, 5.0);
    none.addSector("S1", 10.0);
    const PitStrategy zero(none, two);
    const std::vector<StrategyReport> only = boxbox::kBestPlans(zero, 5);
    SCHECK(only.size() == 1U);
    SCHECK(only[0].plan.empty() && only[0].feasible);
}

void testBestForStopCount() {
    beginSection("bestForStopCount: against enumeration, and the infeasible counts");

    Lcg rng(44ULL);
    for (int trial = 0; trial < 50; ++trial) {
        const Case c = randomCase(rng, 8);
        const PitStrategy strategy(c.track, c.compounds);
        const std::vector<Ranked> all = allPlans(c.track, c.compounds);
        const int maxStops = rng.nextInt(0, c.track.totalLaps());
        const std::vector<boxbox::StopCountResult> rows = boxbox::bestForStopCount(strategy, maxStops);
        SCHECK(rows.size() == static_cast<std::size_t>(maxStops) + 1U);

        double bestOverall = boxbox::kUnreachable;
        for (const boxbox::StopCountResult& row : rows) {
            double expected = boxbox::kUnreachable;
            for (const Ranked& r : all) {
                if (static_cast<int>(r.plan.size()) - 1 == row.stops) {
                    expected = std::min(expected, r.cost);
                }
            }
            SCHECK(row.feasible == boxbox::isReachable(expected));
            if (row.feasible) {
                SCHECK_NEAR(row.report.totalTime, expected, 1e-9);
                SCHECK(row.report.stops == row.stops);
                SCHECK(row.report.feasible);
                bestOverall = std::min(bestOverall, row.report.totalTime);
            } else {
                SCHECK(!row.reason.empty());
            }
        }
        // When every stop count up to the cheapest plan's is covered, the minimum is the optimum.
        const int optimalStops = strategy.optimize().stopCount();
        if (maxStops >= optimalStops) {
            SCHECK_NEAR(bestOverall, strategy.optimize().totalTime, 1e-9);
        }
    }

    // Hand case: A and B can't cover 4 laps in one stint of A (max 2), so 0 stops is B4 = 48;
    // 1 stop's best is A2 A2 = 47; 4 stops would need 5 stints for 4 laps.
    const Hand h;
    const PitStrategy hand(h.track, h.compounds);
    const std::vector<boxbox::StopCountResult> rows = boxbox::bestForStopCount(hand, 4);
    SCHECK(rows.size() == 5U);
    SCHECK(rows[0].feasible && std::fabs(rows[0].report.totalTime - 48.0) <= 1e-9);
    SCHECK(rows[1].feasible && std::fabs(rows[1].report.totalTime - 47.0) <= 1e-9);
    SCHECK(rows[3].feasible);   // 4 one-lap stints on A: 40 + 15
    SCHECK(!rows[4].feasible && !rows[4].reason.empty());

    SCHECK_THROWS(boxbox::bestForStopCount(hand, -1), std::invalid_argument);
    SCHECK_THROWS(boxbox::bestForStopCount(hand, 5), std::invalid_argument);
}

void testComparisonSets() {
    beginSection("compoundOnlyBest, standardCandidates and compareStrategies");

    Lcg rng(55ULL);
    for (int trial = 0; trial < 40; ++trial) {
        const Case c = randomCase(rng, 8);
        const PitStrategy strategy(c.track, c.compounds);
        const double optimum = strategy.optimize().totalTime;

        const std::vector<StrategyReport> only = boxbox::compoundOnlyBest(strategy);
        SCHECK(only.size() == c.compounds.size());
        for (std::size_t i = 0; i < only.size(); ++i) {
            const PitStrategy single(c.track, {c.compounds[i]});
            SCHECK(only[i].feasible);
            SCHECK_NEAR(only[i].totalTime, single.optimize().totalTime, 1e-9);
            SCHECK(only[i].totalTime >= optimum - 1e-9);
            for (const PlannedStint& s : only[i].plan) {
                SCHECK(s.compoundIndex == i);
            }
        }

        const std::vector<NamedPlan> candidates = boxbox::standardCandidates(strategy, 3);
        SCHECK(!candidates.empty() && candidates.front().name == "optimal");
        for (std::size_t a = 0; a < candidates.size(); ++a) {
            for (std::size_t b = 0; b < a; ++b) {
                SCHECK(!boxbox::samePlan(candidates[a].plan, candidates[b].plan));
            }
        }
        const std::vector<StrategyReport> reports = boxbox::compareStrategies(strategy, candidates);
        SCHECK(reports.size() == candidates.size());
        for (const StrategyReport& r : reports) {
            SCHECK(r.feasible);
            SCHECK(r.deltaVsOptimum >= -1e-9);
            SCHECK_NEAR(r.optimumTotal, optimum, 1e-9);
            SCHECK_NEAR(r.totalTime, directCost(c.track, c.compounds, r.plan), 1e-9);
        }
        SCHECK_NEAR(reports.front().deltaVsOptimum, 0.0, 1e-9);
    }

    // An infeasible candidate stays in the comparison, with its reason.
    const Hand h;
    const PitStrategy hand(h.track, h.compounds);
    const std::vector<StrategyReport> mixed = boxbox::compareStrategies(
        hand, {NamedPlan{"good", {PlannedStint{0, 2}, PlannedStint{0, 2}}},
               NamedPlan{"too short", {PlannedStint{1, 3}}}});
    SCHECK(mixed.size() == 2U);
    SCHECK(mixed[0].feasible && !mixed[1].feasible && !mixed[1].reason.empty());
    SCHECK(boxbox::compareStrategies(hand, {}).empty());
}

void testCleanAirVsTraffic() {
    beginSection("cleanAirVsTraffic: clean totals close against the plans; traffic only costs time");

    Track track("Race", 30, 21.0);
    track.addSector("S1", 90.0);
    const std::vector<TireCompound> compounds = boxbox::defaultCompounds();
    const PitStrategy strategy(track, compounds);
    const std::vector<NamedPlan> plans = boxbox::standardCandidates(strategy, 2);
    SCHECK(plans.size() >= 3U);

    OvertakeRule rule;
    rule.enabled = true;
    rule.minGap = 5.0;
    rule.passMargin = 10.0;
    const std::vector<boxbox::TrafficComparisonRow> rows = boxbox::cleanAirVsTraffic(strategy, plans, rule);
    SCHECK(rows.size() == plans.size());

    std::vector<int> cleanSeen(rows.size() + 1U, 0);
    std::vector<int> busySeen(rows.size() + 1U, 0);
    bool anyLoss = false;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const boxbox::StrategyResult planned = strategy.evaluatePlan(plans[i].plan);
        SCHECK_NEAR(rows[i].cleanTime, planned.totalTime, boxbox::kClosureTolerance);  // FR-8 per car
        SCHECK(rows[i].trafficLoss >= 0.0);
        SCHECK_NEAR(rows[i].trafficTime, rows[i].cleanTime + rows[i].trafficLoss, boxbox::kClosureTolerance);
        SCHECK(rows[i].trafficTime >= rows[i].cleanTime - boxbox::kTimeEpsilon);
        SCHECK(rows[i].cleanPosition >= 1 && rows[i].cleanPosition <= static_cast<int>(rows.size()));
        SCHECK(rows[i].trafficPosition >= 1 && rows[i].trafficPosition <= static_cast<int>(rows.size()));
        ++cleanSeen[static_cast<std::size_t>(rows[i].cleanPosition)];
        ++busySeen[static_cast<std::size_t>(rows[i].trafficPosition)];
        anyLoss = anyLoss || rows[i].trafficLoss > 0.0;
        SCHECK(rows[i].name == plans[i].name);
    }
    for (std::size_t p = 1; p <= rows.size(); ++p) {
        SCHECK(cleanSeen[p] == 1);
        SCHECK(busySeen[p] == 1);
    }
    SCHECK(anyLoss);  // a 5 s gap rule with these rivals must hold someone up

    // A disabled rule changes nothing.
    const std::vector<boxbox::TrafficComparisonRow> same =
        boxbox::cleanAirVsTraffic(strategy, plans, OvertakeRule{});
    for (const boxbox::TrafficComparisonRow& r : same) {
        SCHECK_NEAR(r.trafficLoss, 0.0, 0.0);
        SCHECK_NEAR(r.trafficTime, r.cleanTime, 0.0);
    }

    // A single car is never held.
    const std::vector<boxbox::TrafficComparisonRow> solo =
        boxbox::cleanAirVsTraffic(strategy, {plans.front()}, rule);
    SCHECK(solo.size() == 1U);
    SCHECK_NEAR(solo[0].trafficLoss, 0.0, 0.0);

    SCHECK_THROWS(boxbox::cleanAirVsTraffic(strategy, {}, rule), std::invalid_argument);
    SCHECK_THROWS(boxbox::cleanAirVsTraffic(strategy, {NamedPlan{"bad", {PlannedStint{0, 3}}}}, rule),
                  std::invalid_argument);
    OvertakeRule tinyGap = rule;
    tinyGap.minGap = 0.0;
    SCHECK_THROWS(boxbox::cleanAirVsTraffic(strategy, plans, tinyGap), std::invalid_argument);

    // Determinism.
    const std::vector<boxbox::TrafficComparisonRow> again = boxbox::cleanAirVsTraffic(strategy, plans, rule);
    bool identical = again.size() == rows.size();
    for (std::size_t i = 0; identical && i < rows.size(); ++i) {
        identical = again[i].trafficTime == rows[i].trafficTime &&
                    again[i].cleanPosition == rows[i].cleanPosition &&
                    again[i].trafficPosition == rows[i].trafficPosition;
    }
    SCHECK(identical);
}

void testLoopClosureRegression() {
    beginSection("Regression: every plan the lab produces still closes against the simulator");

    Lcg rng(66ULL);
    for (int trial = 0; trial < 30; ++trial) {
        const Case c = randomCase(rng, 10);
        const PitStrategy strategy(c.track, c.compounds);
        const std::vector<StrategyReport> alternatives = boxbox::kBestPlans(strategy, 6);
        for (const StrategyReport& r : alternatives) {
            boxbox::RaceSimulator sim(c.track, c.compounds);
            sim.addDriver(1, "SOLO", strategy.evaluatePlan(r.plan));
            sim.runToFinish();
            SCHECK_NEAR(sim.drivers().front().cumulativeTime(), r.totalTime, boxbox::kClosureTolerance);
        }
    }
}

}  // namespace

StrategyTestTally runStrategyAnalysisTests() {
    g_tally = StrategyTestTally();

    testValidateAndEvaluate();
    testEvaluateAgainstDirectSummation();
    testPitLossSweep();
    testCompoundSweep();
    testKBestPlans();
    testBestForStopCount();
    testComparisonSets();
    testCleanAirVsTraffic();
    testLoopClosureRegression();

    return g_tally;
}
