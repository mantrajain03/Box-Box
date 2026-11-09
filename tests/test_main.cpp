// Box Box: assert-based test harness (Phase 5).
//
// Hand-written on purpose. A test framework would be an external dependency
// and would break NFR-1, so this file provides its own CHECK macros, its own
// seeded pseudo-random generator, and its own brute-force reference
// implementation to check the dynamic program against.
//
// Unlike a bare assert(), a failing CHECK records the failure and carries on,
// so one run reports every problem rather than only the first. The process
// exits non-zero if anything failed, which is what makes `make test` a gate.

#include <cmath>
#include <cstdint>
#include <cstddef>
#include <algorithm>
#include <initializer_list>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "boxbox/Common.hpp"
#include "boxbox/Driver.hpp"
#include "boxbox/Leaderboard.hpp"
#include "boxbox/PitStrategy.hpp"
#include "boxbox/RaceSimulator.hpp"
#include "boxbox/TireCompound.hpp"
#include "boxbox/Track.hpp"

#include "DriverAnalyticsTests.hpp"
#include "TireModuleTests.hpp"

namespace {

using boxbox::Driver;
using boxbox::Leaderboard;
using boxbox::LeaderboardEntry;
using boxbox::OvertakeRule;
using boxbox::PitStrategy;
using boxbox::PlannedStint;
using boxbox::RaceSimulator;
using boxbox::Standing;
using boxbox::StrategyResult;
using boxbox::Stint;
using boxbox::TireCompound;
using boxbox::Track;

// ---------------------------------------------------------------------------
// Harness
// ---------------------------------------------------------------------------

int g_checks = 0;
int g_failures = 0;
std::string g_section;

void beginSection(const std::string& name) {
    g_section = name;
    std::cout << "\n== " << name << '\n';
}

void reportCheck(bool ok, const char* expression, const char* file, int line,
                 const std::string& detail) {
    ++g_checks;
    if (ok) {
        return;
    }
    ++g_failures;
    std::cout << "   FAIL  " << file << ':' << line << "  in " << g_section << '\n';
    std::cout << "         " << expression << '\n';
    if (!detail.empty()) {
        std::cout << "         " << detail << '\n';
    }
}

std::string describe(double actual, double expected, double tolerance) {
    std::ostringstream out;
    out << std::setprecision(12) << "actual " << actual << "   expected " << expected
        << "   diff " << std::fabs(actual - expected) << "   tol " << tolerance;
    return out.str();
}

#define CHECK(expr) reportCheck((expr), #expr, __FILE__, __LINE__, std::string())

#define CHECK_MSG(expr, detail) reportCheck((expr), #expr, __FILE__, __LINE__, (detail))

#define CHECK_NEAR(actual, expected, tolerance)                                         \
    reportCheck(std::fabs((actual) - (expected)) <= (tolerance),                        \
                #actual " ~= " #expected, __FILE__, __LINE__,                           \
                describe((actual), (expected), (tolerance)))

#define CHECK_THROWS(statement)                                                         \
    do {                                                                                \
        bool threw = false;                                                             \
        try {                                                                           \
            statement;                                                                  \
        } catch (const std::exception&) {                                               \
            threw = true;                                                               \
        }                                                                               \
        reportCheck(threw, "throws: " #statement, __FILE__, __LINE__, std::string());   \
    } while (false)

/// Explicitly seeded 64-bit LCG. Nothing in the tests is allowed to be random
/// in the sense of varying between runs (NFR-5); this varies the *inputs* while
/// staying byte-identical from run to run and across platforms.
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
// Builders
// ---------------------------------------------------------------------------

Track makeTrack(int laps, double pitLoss, const std::vector<double>& sectors) {
    Track track("Test Circuit", laps, pitLoss);
    for (std::size_t i = 0; i < sectors.size(); ++i) {
        track.addSector("S" + std::to_string(i + 1U), sectors[i]);
    }
    return track;
}

/// The worked example from PRD sec. 7: one 10 s sector, 5 s pit loss, a
/// degrading compound A capped at 3 laps and a flat compound B.
std::vector<TireCompound> handCheckCompounds() {
    std::vector<TireCompound> set;
    set.emplace_back("A", 0.0, 1.0, 3);
    set.emplace_back("B", 2.0, 0.0, 4);
    return set;
}

/// The Stint form of a plan - what Driver and RaceSimulator::addDriver take -
/// built by hand, so that no validation runs on the way in.
std::vector<Stint> asStints(const std::vector<PlannedStint>& plan,
                            const std::vector<TireCompound>& compounds) {
    std::vector<Stint> stints;
    int startLap = 1;
    for (const PlannedStint& planned : plan) {
        Stint stint;
        stint.compoundIndex = planned.compoundIndex;
        stint.compoundName = compounds[planned.compoundIndex].name();
        stint.startLap = startLap;
        stint.laps = planned.laps;
        stints.push_back(stint);
        startLap += planned.laps;
    }
    return stints;
}

/// Flat compounds for the FR-9 hand cases. Nothing degrades, so on a one-sector
/// 100 s track every lap on a compound takes exactly 100 s + basePace, and each
/// pace gap sits at a chosen place relative to a 1.0 s pass margin.
std::vector<TireCompound> flatCompounds() {
    std::vector<TireCompound> set;
    set.emplace_back("FAST", 0.0, 0.0, 10);   // 100.0 s a lap
    set.emplace_back("SLOW", 0.4, 0.0, 10);   // 100.4 s: 0.4 s off FAST, inside the margin
    set.emplace_back("EDGE", 1.0, 0.0, 10);   // 101.0 s: exactly the margin off FAST
    set.emplace_back("CRAWL", 1.5, 0.0, 10);  // 101.5 s: beyond the margin
    return set;
}

constexpr std::size_t kFast = 0;
constexpr std::size_t kSlow = 1;
constexpr std::size_t kEdge = 2;
constexpr std::size_t kCrawl = 3;

/// A 3-lap race on the flat compounds - one 100 s sector, 20 s pit loss - with
/// one car per plan, entered as car 1, 2, ... in order and run to the flag.
/// With `overtaking`, the rule is set explicitly to minGap 0.5 s and
/// passMargin 1.0 s, so the hand cases do not lean on the defaults.
RaceSimulator flatRace(const std::vector<std::vector<PlannedStint>>& plans, bool overtaking) {
    const Track track = makeTrack(3, 20.0, {100.0});
    const PitStrategy strategy(track, flatCompounds());

    RaceSimulator simulator(track, flatCompounds());
    if (overtaking) {
        OvertakeRule rule;
        rule.enabled = true;
        rule.minGap = 0.5;
        rule.passMargin = 1.0;
        simulator.setOvertakeRule(rule);
    }
    for (std::size_t i = 0; i < plans.size(); ++i) {
        simulator.addDriver(static_cast<int>(i) + 1, "CAR" + std::to_string(i + 1U),
                            strategy.evaluatePlan(plans[i]));
    }
    simulator.runToFinish();
    return simulator;
}

// ---------------------------------------------------------------------------
// Brute-force reference for the optimizer
// ---------------------------------------------------------------------------

/// Exhaustively enumerate every legal stint decomposition and return the best
/// total time. Exponential, and deliberately written without reference to the
/// DP so that agreement between the two means something. Only ever called with
/// small lap counts.
double bruteForceBest(const PitStrategy& strategy, int lapsLeft, bool isFirstStint) {
    if (lapsLeft == 0) {
        return 0.0;
    }

    const double pitLoss = strategy.track().pitLoss();
    double best = boxbox::kUnreachable;

    for (std::size_t c = 0; c < strategy.compounds().size(); ++c) {
        const int longest = std::min(lapsLeft, strategy.compounds()[c].maxStintLaps());
        for (int length = 1; length <= longest; ++length) {
            const double rest = bruteForceBest(strategy, lapsLeft - length, false);
            if (!boxbox::isReachable(rest)) {
                continue;
            }
            const double candidate =
                (isFirstStint ? 0.0 : pitLoss) + strategy.stintCost(c, length) + rest;
            if (candidate < best) {
                best = candidate;
            }
        }
    }
    return best;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

void testTrack() {
    beginSection("Track (FR-3)");

    const Track three = makeTrack(50, 22.0, {28.4, 31.2, 30.4});
    CHECK(three.sectorCount() == 3U);
    CHECK_NEAR(three.baseLapTime(), 90.0, boxbox::kTimeEpsilon);
    CHECK(three.totalLaps() == 50);
    CHECK_NEAR(three.pitLoss(), 22.0, boxbox::kTimeEpsilon);

    // Single-sector track: the Phase 5 edge case.
    const Track single = makeTrack(10, 5.0, {95.5});
    CHECK(single.sectorCount() == 1U);
    CHECK_NEAR(single.baseLapTime(), 95.5, boxbox::kTimeEpsilon);

    // No sectors at all is legal and contributes nothing to a lap.
    const Track bare = makeTrack(10, 5.0, {});
    CHECK(bare.sectorCount() == 0U);
    CHECK_NEAR(bare.baseLapTime(), 0.0, boxbox::kTimeEpsilon);

    // A zero-lap race is a legal Track, not an error.
    const Track empty = makeTrack(0, 22.0, {90.0});
    CHECK(empty.totalLaps() == 0);

    Track mutableTrack = makeTrack(10, 5.0, {90.0});
    mutableTrack.setTotalLaps(20);
    mutableTrack.setPitLoss(17.5);
    CHECK(mutableTrack.totalLaps() == 20);
    CHECK_NEAR(mutableTrack.pitLoss(), 17.5, boxbox::kTimeEpsilon);

    CHECK_THROWS(Track("Bad", -1, 20.0));
    CHECK_THROWS(Track("Bad", 10, -1.0));
    CHECK_THROWS(mutableTrack.addSector("Negative", -1.0));
    CHECK_THROWS(mutableTrack.setTotalLaps(-3));
    CHECK_THROWS(mutableTrack.setPitLoss(-0.5));
}

void testTireCompound() {
    beginSection("TireCompound (FR-4)");

    // Purely linear: no cliff configured.
    const TireCompound linear("LINEAR", -0.5, 0.1, 20);
    CHECK(!linear.hasCliff());
    CHECK_NEAR(linear.lapDelta(0), -0.5, boxbox::kTimeEpsilon);
    CHECK_NEAR(linear.lapDelta(1), -0.4, boxbox::kTimeEpsilon);
    CHECK_NEAR(linear.lapDelta(10), 0.5, boxbox::kTimeEpsilon);

    // Negative ages clamp to a fresh set rather than running the model backwards.
    CHECK_NEAR(linear.lapDelta(-5), linear.lapDelta(0), boxbox::kTimeEpsilon);

    // Cliff at age 5, costing an extra 0.2 s per lap beyond it.
    const TireCompound cliffed("CLIFF", 0.0, 0.1, 20, 5, 0.2);
    CHECK(cliffed.hasCliff());
    CHECK_NEAR(cliffed.lapDelta(4), 0.4, boxbox::kTimeEpsilon);   // below the cliff
    CHECK_NEAR(cliffed.lapDelta(5), 0.5, boxbox::kTimeEpsilon);   // exactly at it
    CHECK_NEAR(cliffed.lapDelta(6), 0.8, boxbox::kTimeEpsilon);   // 0.6 + 0.2*1
    CHECK_NEAR(cliffed.lapDelta(8), 1.4, boxbox::kTimeEpsilon);   // 0.8 + 0.2*3

    CHECK(linear.code() == 'L');
    CHECK(TireCompound("soft", 0.0, 0.0, 1).code() == 'S');

    CHECK_THROWS(TireCompound("Bad", 0.0, 0.1, 0));
    CHECK_THROWS(TireCompound("Bad", 0.0, -0.1, 10));
    CHECK_THROWS(TireCompound("Bad", 0.0, 0.1, 10, 5, -0.2));

    // The default set must be ordered softest to hardest in every respect.
    const std::vector<TireCompound> defaults = boxbox::defaultCompounds();
    CHECK(defaults.size() == 3U);
    for (std::size_t i = 1; i < defaults.size(); ++i) {
        CHECK_MSG(defaults[i].basePace() > defaults[i - 1U].basePace(),
                  "harder compounds must be slower on a fresh lap");
        CHECK_MSG(defaults[i].degradationPerLap() < defaults[i - 1U].degradationPerLap(),
                  "harder compounds must degrade more slowly");
        CHECK_MSG(defaults[i].maxStintLaps() > defaults[i - 1U].maxStintLaps(),
                  "harder compounds must run longer");
    }
}

void testStintCostTable() {
    beginSection("Prefix-sum table and stintCost");

    const PitStrategy strategy(makeTrack(4, 5.0, {10.0}), handCheckCompounds());

    // A of length L: 10L + sum of ages 0..L-1.
    CHECK_NEAR(strategy.stintCost(0, 1), 10.0, boxbox::kTimeEpsilon);
    CHECK_NEAR(strategy.stintCost(0, 2), 21.0, boxbox::kTimeEpsilon);
    CHECK_NEAR(strategy.stintCost(0, 3), 33.0, boxbox::kTimeEpsilon);

    // B of length L: 10L + 2L.
    CHECK_NEAR(strategy.stintCost(1, 1), 12.0, boxbox::kTimeEpsilon);
    CHECK_NEAR(strategy.stintCost(1, 4), 48.0, boxbox::kTimeEpsilon);

    // Zero laps costs nothing; past maxStintLaps is unreachable, not expensive.
    CHECK_NEAR(strategy.stintCost(0, 0), 0.0, boxbox::kTimeEpsilon);
    CHECK(!boxbox::isReachable(strategy.stintCost(0, 4)));
    CHECK(!boxbox::isReachable(strategy.stintCost(1, 5)));
    CHECK(!boxbox::isReachable(strategy.stintCost(99, 1)));

    CHECK(strategy.maxStintLaps() == 4);

    // The table must agree with summing lapDelta the slow way, for every
    // compound and every legal length.
    const std::vector<TireCompound>& compounds = strategy.compounds();
    for (std::size_t c = 0; c < compounds.size(); ++c) {
        double runningDelta = 0.0;
        for (int length = 1; length <= compounds[c].maxStintLaps(); ++length) {
            runningDelta += compounds[c].lapDelta(length - 1);
            CHECK_NEAR(strategy.stintDelta(c, length), runningDelta, boxbox::kTimeEpsilon);
            CHECK_NEAR(strategy.stintCost(c, length),
                       static_cast<double>(length) * strategy.track().baseLapTime() +
                           runningDelta,
                       boxbox::kTimeEpsilon);
        }
    }

    CHECK_THROWS(PitStrategy(makeTrack(4, 5.0, {10.0}), {}));
}

void testOptimizerAgainstHandComputation() {
    beginSection("FR-1: DP against the hand-computed case");

    const PitStrategy strategy(makeTrack(4, 5.0, {10.0}), handCheckCompounds());
    const StrategyResult result = strategy.optimize();

    // Worked by hand in PRD sec. 7:
    //   dp[1]=10 (A x1)  dp[2]=21 (A x2)  dp[3]=33 (A x3)  dp[4]=47 (A x2 + A x2)
    CHECK(result.feasible);
    CHECK_NEAR(result.dpValue, 47.0, boxbox::kTimeEpsilon);
    CHECK_NEAR(result.totalTime, 47.0, boxbox::kTimeEpsilon);
    CHECK_NEAR(result.greenTime, 42.0, boxbox::kTimeEpsilon);
    CHECK_NEAR(result.pitTime, 5.0, boxbox::kTimeEpsilon);

    CHECK(result.stints.size() == 2U);
    CHECK(result.stopCount() == 1);
    CHECK(result.plannedLaps() == 4);

    if (result.stints.size() == 2U) {
        CHECK(result.stints[0].compoundName == "A");
        CHECK(result.stints[0].laps == 2);
        CHECK(result.stints[0].startLap == 1);
        CHECK(result.stints[1].compoundName == "A");
        CHECK(result.stints[1].laps == 2);
        CHECK(result.stints[1].startLap == 3);
    }
    if (result.pitLaps.size() == 1U) {
        CHECK(result.pitLaps[0] == 2);
    }

    // dp[N] and the total rebuilt from the backtracked plan are computed by
    // different arithmetic and must still agree.
    CHECK_NEAR(result.totalTime, result.dpValue, boxbox::kClosureTolerance);
}

void testOptimizerAgainstBruteForce() {
    beginSection("FR-1: DP against exhaustive enumeration");

    Lcg random(0x5EEDF1A9ULL);

    for (int trial = 0; trial < 40; ++trial) {
        const int laps = random.nextInt(1, 9);
        const double pitLoss = random.nextDouble(0.0, 30.0);
        const double baseLap = random.nextDouble(20.0, 100.0);

        std::vector<TireCompound> compounds;
        const int compoundCount = random.nextInt(1, 3);
        for (int c = 0; c < compoundCount; ++c) {
            const bool withCliff = (random.nextInt(0, 1) == 1);
            compounds.emplace_back("C" + std::to_string(c),
                                   random.nextDouble(-1.5, 1.5),   // basePace
                                   random.nextDouble(0.0, 0.3),    // degradation
                                   random.nextInt(1, 6),           // maxStintLaps
                                   withCliff ? random.nextInt(0, 4) : -1,
                                   withCliff ? random.nextDouble(0.0, 0.5) : 0.0);
        }

        const PitStrategy strategy(makeTrack(laps, pitLoss, {baseLap}), compounds);
        const StrategyResult result = strategy.optimize();
        const double reference = bruteForceBest(strategy, laps, true);

        std::ostringstream detail;
        detail << "trial " << trial << "  laps " << laps << "  compounds " << compoundCount;

        CHECK_MSG(result.feasible == boxbox::isReachable(reference), detail.str());
        if (result.feasible && boxbox::isReachable(reference)) {
            CHECK_NEAR(result.dpValue, reference, boxbox::kClosureTolerance);
            CHECK_NEAR(result.totalTime, reference, boxbox::kClosureTolerance);
            CHECK_MSG(result.plannedLaps() == laps, detail.str());
        }
    }
}

void testStintLengthsCoverRace() {
    beginSection("FR-1: stint lengths sum to exactly totalLaps");

    Lcg random(0xC0FFEE11ULL);

    for (int trial = 0; trial < 60; ++trial) {
        const int laps = random.nextInt(1, 80);
        const PitStrategy strategy(
            makeTrack(laps, random.nextDouble(0.0, 40.0), {random.nextDouble(60.0, 110.0)}),
            boxbox::defaultCompounds());

        const StrategyResult result = strategy.optimize();

        std::ostringstream detail;
        detail << "laps " << laps << "  planned " << result.plannedLaps();

        CHECK_MSG(result.feasible, detail.str());
        CHECK_MSG(result.plannedLaps() == laps, detail.str());
        CHECK_MSG(result.stopCount() == static_cast<int>(result.stints.size()) - 1,
                  detail.str());

        // Stints must tile the race with no gap and no overlap.
        int expectedStart = 1;
        for (const Stint& stint : result.stints) {
            CHECK_MSG(stint.laps >= 1, detail.str());
            CHECK_MSG(stint.startLap == expectedStart, detail.str());
            expectedStart += stint.laps;
        }
        CHECK_MSG(expectedStart == laps + 1, detail.str());

        // Every pit lap must fall strictly inside the race.
        for (const int pitLap : result.pitLaps) {
            CHECK_MSG(pitLap >= 1 && pitLap < laps, detail.str());
        }
    }
}

void testZeroPitLossFavoursShortStints() {
    beginSection("FR-1: zero pit loss favours many short stints on the softest tire");

    const std::vector<TireCompound> compounds = boxbox::defaultCompounds();

    // "Softest" here means quickest on a fresh set, which is what a free stop
    // makes optimal: pit every lap and always run age 0.
    std::size_t softest = 0;
    for (std::size_t c = 1; c < compounds.size(); ++c) {
        if (compounds[c].lapDelta(0) < compounds[softest].lapDelta(0)) {
            softest = c;
        }
    }

    const int laps = 50;
    const PitStrategy strategy(makeTrack(laps, 0.0, {90.0}), compounds);
    const StrategyResult result = strategy.optimize();

    CHECK(result.feasible);
    CHECK(result.plannedLaps() == laps);
    CHECK_MSG(static_cast<int>(result.stints.size()) == laps,
              "a free pit stop should produce one stint per lap");
    CHECK(result.stopCount() == laps - 1);

    bool allSoftest = true;
    bool allSingleLap = true;
    for (const Stint& stint : result.stints) {
        allSoftest = allSoftest && (stint.compoundIndex == softest);
        allSingleLap = allSingleLap && (stint.laps == 1);
    }
    CHECK_MSG(allSoftest, "every stint should be on the quickest fresh compound");
    CHECK_MSG(allSingleLap, "every stint should be a single lap");

    CHECK_NEAR(result.pitTime, 0.0, boxbox::kTimeEpsilon);
    CHECK_NEAR(result.totalTime,
               static_cast<double>(laps) * (90.0 + compounds[softest].lapDelta(0)),
               boxbox::kClosureTolerance);
}

void testHighPitLossCollapsesStops() {
    beginSection("FR-1: a huge pit loss collapses to the fewest legal stops");

    const std::vector<TireCompound> compounds = boxbox::defaultCompounds();

    int longestStint = 0;
    for (const TireCompound& compound : compounds) {
        longestStint = std::max(longestStint, compound.maxStintLaps());
    }

    // A race no compound can cover in one go: at least one stop is forced.
    {
        const int laps = longestStint + 5;
        const PitStrategy strategy(makeTrack(laps, 1.0e6, {90.0}), compounds);
        const StrategyResult result = strategy.optimize();

        const int fewestStints = (laps + longestStint - 1) / longestStint;
        CHECK(result.feasible);
        CHECK_MSG(result.stopCount() == fewestStints - 1,
                  "an unaffordable stop should force the minimum stint count");
        CHECK(result.plannedLaps() == laps);
    }

    // A race that fits inside one stint: no stop at all.
    {
        const int laps = longestStint;
        const PitStrategy strategy(makeTrack(laps, 1.0e6, {90.0}), compounds);
        const StrategyResult result = strategy.optimize();

        CHECK(result.feasible);
        CHECK_MSG(result.stopCount() == 0, "a race that fits one stint should not stop");
        CHECK(result.stints.size() == 1U);
        CHECK_NEAR(result.pitTime, 0.0, boxbox::kTimeEpsilon);
    }
}

void testLoopClosure() {
    beginSection("FR-7 / FR-8: optimizer and simulator agree");

    Lcg random(0xA5A5F00DULL);

    for (int trial = 0; trial < 60; ++trial) {
        const int laps = random.nextInt(0, 78);
        const double pitLoss = random.nextDouble(0.0, 35.0);
        const double baseLap = random.nextDouble(55.0, 115.0);

        const Track track = makeTrack(laps, pitLoss, {baseLap});
        const PitStrategy strategy(track, boxbox::defaultCompounds());
        const StrategyResult plan = strategy.optimize();

        std::ostringstream detail;
        detail << "laps " << laps << "  pitLoss " << pitLoss << "  baseLap " << baseLap;

        CHECK_MSG(plan.feasible, detail.str());
        if (!plan.feasible) {
            continue;
        }

        RaceSimulator simulator(track, boxbox::defaultCompounds());
        simulator.addDriver(1, "OPTIMAL", plan);  // FR-7: no hand-written stop laps
        simulator.runToFinish();

        const Driver& driver = simulator.drivers().front();

        CHECK_MSG(driver.finished(), detail.str());
        CHECK_MSG(driver.lapsCompleted() == laps, detail.str());
        CHECK_MSG(driver.stopsMade() == plan.stopCount(), detail.str());

        // The check the whole project exists to pass. The tolerance comes from
        // the core and is never widened here.
        CHECK_NEAR(driver.cumulativeTime(), plan.totalTime, boxbox::kClosureTolerance);
    }
}

void testSimulatorEdgeCases() {
    beginSection("Simulator edge cases");

    const std::vector<TireCompound> compounds = boxbox::defaultCompounds();

    // Zero laps: an empty plan is the only legal one, and it closes at 0.0.
    {
        const Track track = makeTrack(0, 22.0, {90.0});
        const PitStrategy strategy(track, compounds);
        const StrategyResult plan = strategy.optimize();

        CHECK(plan.feasible);
        CHECK(plan.stints.empty());
        CHECK_NEAR(plan.totalTime, 0.0, boxbox::kTimeEpsilon);

        RaceSimulator simulator(track, compounds);
        simulator.addDriver(1, "NOBODY", plan);
        CHECK(simulator.finished());
        CHECK(!simulator.advanceLap());

        simulator.runToFinish();
        CHECK_NEAR(simulator.drivers().front().cumulativeTime(), 0.0,
                   boxbox::kClosureTolerance);
    }

    // One lap: one stint, no stop.
    {
        const Track track = makeTrack(1, 22.0, {90.0});
        const PitStrategy strategy(track, compounds);
        const StrategyResult plan = strategy.optimize();

        CHECK(plan.stints.size() == 1U);
        CHECK(plan.stopCount() == 0);

        RaceSimulator simulator(track, compounds);
        simulator.addDriver(1, "SOLO", plan);
        simulator.runToFinish();

        const Driver& driver = simulator.drivers().front();
        CHECK(driver.lapsCompleted() == 1);
        CHECK(driver.stopsMade() == 0);
        CHECK_NEAR(driver.cumulativeTime(), plan.totalTime, boxbox::kClosureTolerance);
    }

    // A driver with no planned stops over a long race.
    {
        const int laps = compounds.back().maxStintLaps();
        const Track track = makeTrack(laps, 22.0, {90.0});
        RaceSimulator simulator(track, compounds);

        std::vector<PlannedStint> plan;
        plan.push_back(PlannedStint{compounds.size() - 1U, laps});

        const PitStrategy strategy(track, compounds);
        const StrategyResult costed = strategy.evaluatePlan(plan);
        CHECK(costed.feasible);
        CHECK(costed.stopCount() == 0);

        simulator.addDriver(7, "NO-STOPS", costed);
        simulator.runToFinish();

        const Driver& driver = simulator.drivers().front();
        CHECK(driver.stopsMade() == 0);
        CHECK(driver.tireAge() == laps);
        CHECK_NEAR(driver.cumulativeTime(), costed.totalTime, boxbox::kClosureTolerance);
    }

    // A stop immediately before the final lap: the last stint is one lap long,
    // and no pit loss may be charged after the chequered flag.
    {
        const int laps = 10;
        const Track track = makeTrack(laps, 20.0, {90.0});
        const PitStrategy strategy(track, compounds);

        std::vector<PlannedStint> plan;
        plan.push_back(PlannedStint{0U, laps - 1});
        plan.push_back(PlannedStint{0U, 1});

        const StrategyResult costed = strategy.evaluatePlan(plan);
        CHECK(costed.feasible);
        CHECK(costed.stopCount() == 1);
        if (costed.pitLaps.size() == 1U) {
            CHECK_MSG(costed.pitLaps[0] == laps - 1, "the only stop ends lap 9, not lap 10");
        }
        CHECK_NEAR(costed.pitTime, 20.0, boxbox::kTimeEpsilon);

        RaceSimulator simulator(track, compounds);
        simulator.addDriver(1, "LATE-STOP", costed);
        simulator.runToFinish();

        const Driver& driver = simulator.drivers().front();
        CHECK_MSG(driver.stopsMade() == 1, "exactly one pit loss, none after the flag");
        CHECK(driver.tireAge() == 1);
        CHECK_NEAR(driver.cumulativeTime(), costed.totalTime, boxbox::kClosureTolerance);
    }

    // A zero-lap stint is not a plan: rejected at both entry points. This is
    // what makes "a stop on lap 0" impossible to express.
    {
        Stint empty;
        empty.compoundIndex = 0;
        empty.laps = 0;
        empty.compoundName = compounds[0].name();

        std::vector<Stint> badPlan;
        badPlan.push_back(empty);

        CHECK_THROWS(Driver(1, "BAD", badPlan));

        RaceSimulator simulator(makeTrack(10, 20.0, {90.0}), compounds);
        CHECK_THROWS(simulator.addDriver(1, "BAD", badPlan));

        // A plan that does not cover the race is rejected too.
        Stint shortStint;
        shortStint.compoundIndex = 0;
        shortStint.laps = 3;
        shortStint.compoundName = compounds[0].name();
        CHECK_THROWS(simulator.addDriver(2, "SHORT", std::vector<Stint>{shortStint}));

        // As is an infeasible StrategyResult.
        CHECK_THROWS(simulator.addDriver(3, "INFEASIBLE", StrategyResult{}));
    }

    // Single-sector track, driven end to end.
    {
        const Track track = makeTrack(12, 18.0, {95.5});
        const PitStrategy strategy(track, compounds);
        const StrategyResult plan = strategy.optimize();

        RaceSimulator simulator(track, compounds);
        simulator.addDriver(1, "SINGLE-SECTOR", plan);
        simulator.runToFinish();

        CHECK_NEAR(simulator.drivers().front().cumulativeTime(), plan.totalTime,
                   boxbox::kClosureTolerance);
    }
}

void testCompoundSwitching() {
    beginSection("FR-5 / FR-6: compound switching and lap-time fidelity");

    const std::vector<TireCompound> compounds = boxbox::defaultCompounds();
    const int laps = 20;
    const Track track = makeTrack(laps, 20.0, {90.0});

    std::vector<PlannedStint> plan;
    plan.push_back(PlannedStint{0U, 10});  // SOFT
    plan.push_back(PlannedStint{2U, 10});  // HARD

    const PitStrategy strategy(track, compounds);
    const StrategyResult costed = strategy.evaluatePlan(plan);
    CHECK(costed.feasible);

    RaceSimulator simulator(track, compounds);
    simulator.addDriver(1, "SWITCHER", costed);

    // Before the stop: on SOFT, and the lap time must track the tire's age.
    for (int lap = 1; lap <= 10; ++lap) {
        const Driver& driver = simulator.drivers().front();
        const double expected = 90.0 + compounds[0].lapDelta(lap - 1);
        CHECK_NEAR(simulator.lapTimeFor(driver), expected, boxbox::kTimeEpsilon);
        CHECK(driver.currentCompoundName() == "SOFT");
        CHECK(driver.tireAge() == lap - 1);
        simulator.advanceLap();
    }

    // FR-6: the switch happened at the stop, on fresh rubber.
    {
        const Driver& driver = simulator.drivers().front();
        CHECK_MSG(driver.stopsMade() == 1, "exactly one stop after lap 10");
        CHECK_MSG(driver.currentCompoundName() == "HARD", "compound changed at the stop");
        CHECK_MSG(driver.tireAge() == 0, "tire age reset at the stop");
    }

    // After the stop: on HARD, ages counting from zero again.
    for (int lap = 1; lap <= 10; ++lap) {
        const Driver& driver = simulator.drivers().front();
        const double expected = 90.0 + compounds[2].lapDelta(lap - 1);
        CHECK_NEAR(simulator.lapTimeFor(driver), expected, boxbox::kTimeEpsilon);
        simulator.advanceLap();
    }

    CHECK(simulator.finished());
    CHECK_NEAR(simulator.drivers().front().cumulativeTime(), costed.totalTime,
               boxbox::kClosureTolerance);

    // FR-5: two cars on different compounds over identical laps must diverge.
    {
        RaceSimulator race(track, compounds);
        race.addDriver(1, "ALL-SOFT", strategy.singleCompoundPlan(0));
        race.addDriver(2, "ALL-HARD", strategy.singleCompoundPlan(2));
        race.runToFinish();

        const double softTime = race.drivers()[0].cumulativeTime();
        const double hardTime = race.drivers()[1].cumulativeTime();

        CHECK_MSG(!boxbox::nearlyEqual(softTime, hardTime, 1.0e-3),
                  "different compounds must produce different race times");
        CHECK(race.drivers()[0].lapsCompleted() == race.drivers()[1].lapsCompleted());
    }
}

void testEvaluatePlan() {
    beginSection("evaluatePlan and singleCompoundPlan");

    const std::vector<TireCompound> compounds = boxbox::defaultCompounds();
    const int laps = 50;
    const PitStrategy strategy(makeTrack(laps, 22.0, {90.0}), compounds);

    // Costing the optimizer's own plan must reproduce the optimizer's total.
    const StrategyResult best = strategy.optimize();
    std::vector<PlannedStint> asPlanned;
    for (const Stint& stint : best.stints) {
        asPlanned.push_back(PlannedStint{stint.compoundIndex, stint.laps});
    }
    const StrategyResult recosted = strategy.evaluatePlan(asPlanned);

    CHECK(recosted.feasible);
    CHECK_NEAR(recosted.totalTime, best.totalTime, boxbox::kTimeEpsilon);
    CHECK(!boxbox::isReachable(recosted.dpValue));  // no DP ran for this one

    // No hand-entered plan may beat the optimum.
    for (std::size_t c = 0; c < compounds.size(); ++c) {
        const StrategyResult reference = strategy.singleCompoundPlan(c);
        CHECK(reference.feasible);
        CHECK(reference.plannedLaps() == laps);
        CHECK_MSG(reference.totalTime >= best.totalTime - boxbox::kClosureTolerance,
                  "the DP optimum must not be beaten by a single-compound plan");
    }

    // Malformed plans are rejected rather than silently costed.
    CHECK(!strategy.evaluatePlan({PlannedStint{0U, laps - 1}}).feasible);   // too short
    CHECK(!strategy.evaluatePlan({PlannedStint{0U, laps + 1}}).feasible);   // too long
    CHECK(!strategy.evaluatePlan({PlannedStint{0U, 0}}).feasible);          // empty stint
    CHECK(!strategy.evaluatePlan({PlannedStint{99U, laps}}).feasible);      // no such compound
    CHECK(!strategy.evaluatePlan({}).feasible);                            // empty plan
    CHECK(!strategy.singleCompoundPlan(99U).feasible);

    // A stint longer than its compound allows is rejected.
    const int overLong = compounds[0].maxStintLaps() + 1;
    const PitStrategy shortRace(makeTrack(overLong, 22.0, {90.0}), compounds);
    CHECK(!shortRace.evaluatePlan({PlannedStint{0U, overLong}}).feasible);
}

void testLeaderboard() {
    beginSection("FR-2: min-heap leaderboard");

    // An empty heap reports empty and refuses to be read.
    {
        Leaderboard board;
        CHECK(board.empty());
        CHECK(board.size() == 0U);
        CHECK_THROWS(board.pop());
        CHECK_THROWS(board.peek());
        CHECK(board.drainOrdered().empty());
    }

    // Ordering rule, checked directly.
    {
        LeaderboardEntry ahead;
        ahead.driverNumber = 44;
        ahead.lapsCompleted = 10;
        ahead.cumulativeTime = 900.0;

        LeaderboardEntry slower = ahead;
        slower.driverNumber = 1;
        slower.cumulativeTime = 901.0;

        LeaderboardEntry lapped = ahead;
        lapped.driverNumber = 1;
        lapped.lapsCompleted = 9;
        lapped.cumulativeTime = 100.0;

        CHECK_MSG(Leaderboard::isAhead(ahead, slower), "less elapsed time is ahead");
        CHECK_MSG(!Leaderboard::isAhead(slower, ahead), "and the rule is antisymmetric");
        CHECK_MSG(Leaderboard::isAhead(ahead, lapped),
                  "more laps completed beats a smaller clock");

        LeaderboardEntry tied = ahead;
        tied.driverNumber = 99;
        CHECK_MSG(Leaderboard::isAhead(ahead, tied), "an exact tie breaks on car number");
        CHECK_MSG(!Leaderboard::isAhead(tied, ahead), "and breaks it only one way");
    }

    // push/pop against a brute-force sort, on seeded pseudo-random data.
    Lcg random(0xBEEF2024ULL);

    for (int trial = 0; trial < 30; ++trial) {
        const int count = random.nextInt(1, 60);

        std::vector<LeaderboardEntry> entries;
        for (int i = 0; i < count; ++i) {
            LeaderboardEntry entry;
            entry.driverIndex = static_cast<std::size_t>(i);
            entry.driverNumber = i + 1;
            // Deliberately coarse, so exact ties on time really do occur and
            // the car-number tie-break gets exercised.
            entry.lapsCompleted = random.nextInt(8, 10);
            entry.cumulativeTime = static_cast<double>(random.nextInt(0, 40));
            entries.push_back(entry);
        }

        std::vector<LeaderboardEntry> expected = entries;
        std::sort(expected.begin(), expected.end(), Leaderboard::isAhead);

        std::ostringstream detail;
        detail << "trial " << trial << "  entries " << count;

        // Route 1: O(n) heapify through the constructor.
        {
            Leaderboard board(entries);
            CHECK_MSG(board.size() == entries.size(), detail.str());
            CHECK_MSG(board.peek().driverNumber == expected.front().driverNumber,
                      detail.str());

            const std::vector<LeaderboardEntry> actual = board.ordered();
            CHECK_MSG(board.size() == entries.size(), "ordered() must not drain the heap");

            bool matches = (actual.size() == expected.size());
            for (std::size_t i = 0; matches && i < actual.size(); ++i) {
                matches = (actual[i].driverNumber == expected[i].driverNumber);
            }
            CHECK_MSG(matches, "heapify order must match a brute-force sort: " + detail.str());
        }

        // Route 2: n successive O(log n) pushes.
        {
            Leaderboard board;
            for (const LeaderboardEntry& entry : entries) {
                board.push(entry);
            }

            const std::vector<LeaderboardEntry> actual = board.drainOrdered();
            CHECK_MSG(board.empty(), "drainOrdered must empty the heap");

            bool matches = (actual.size() == expected.size());
            for (std::size_t i = 0; matches && i < actual.size(); ++i) {
                matches = (actual[i].driverNumber == expected[i].driverNumber);
            }
            CHECK_MSG(matches, "push order must match a brute-force sort: " + detail.str());
        }

        // takeTop(k) must equal the first k of the full ordering.
        {
            Leaderboard board(entries);
            const std::size_t wanted = static_cast<std::size_t>(random.nextInt(1, count));
            const std::vector<LeaderboardEntry> top = board.takeTop(wanted);

            CHECK_MSG(top.size() == wanted, detail.str());
            bool matches = true;
            for (std::size_t i = 0; matches && i < top.size(); ++i) {
                matches = (top[i].driverNumber == expected[i].driverNumber);
            }
            CHECK_MSG(matches, "takeTop must agree with the full order: " + detail.str());
        }

        // Asking for more than the field returns the whole field.
        {
            Leaderboard board(entries);
            CHECK_MSG(board.takeTop(entries.size() + 10U).size() == entries.size(),
                      detail.str());
        }
    }
}

void testStandings() {
    beginSection("Standings through the leaderboard");

    const std::vector<TireCompound> compounds = boxbox::defaultCompounds();
    const int laps = 30;
    const Track track = makeTrack(laps, 22.0, {90.0});
    const PitStrategy strategy(track, compounds);

    RaceSimulator simulator(track, compounds);
    simulator.addDriver(1, "OPTIMAL", strategy.optimize());
    for (std::size_t c = 0; c < compounds.size(); ++c) {
        simulator.addDriver(static_cast<int>(c) + 2, "REF" + std::to_string(c),
                            strategy.singleCompoundPlan(c));
    }
    simulator.runToFinish();

    const std::vector<Standing> table = simulator.standings();
    CHECK(table.size() == simulator.drivers().size());

    // Positions run 1..D, times are non-decreasing, gaps grow from zero.
    for (std::size_t i = 0; i < table.size(); ++i) {
        CHECK(table[i].position == static_cast<int>(i) + 1);
        if (i > 0) {
            const double previous = simulator.drivers()[table[i - 1U].driverIndex].cumulativeTime();
            const double current = simulator.drivers()[table[i].driverIndex].cumulativeTime();
            CHECK_MSG(current >= previous, "standings must be ordered by race time");
            CHECK_MSG(table[i].gapToLeader >= table[i - 1U].gapToLeader - boxbox::kTimeEpsilon,
                      "gaps must not shrink down the order");
        }
    }
    CHECK_NEAR(table.front().gapToLeader, 0.0, boxbox::kTimeEpsilon);

    // The optimizer's car must win a race it was optimized for.
    CHECK_MSG(simulator.drivers()[table.front().driverIndex].number() == 1,
              "the optimal plan should win against single-compound references");

    // topK must be a prefix of the full classification.
    for (std::size_t k = 0; k <= table.size() + 1U; ++k) {
        const std::vector<Standing> top = simulator.topK(k);
        CHECK(top.size() == std::min(k, table.size()));
        for (std::size_t i = 0; i < top.size(); ++i) {
            CHECK(top[i].driverIndex == table[i].driverIndex);
            CHECK(top[i].position == table[i].position);
        }
    }

    CHECK(simulator.snapshotEntries().size() == simulator.drivers().size());
}

void testDeterminism() {
    beginSection("NFR-5: determinism");

    const std::vector<TireCompound> compounds = boxbox::defaultCompounds();
    const Track track = makeTrack(57, 21.5, {29.1, 30.7, 31.4});
    const PitStrategy strategy(track, compounds);

    const StrategyResult first = strategy.optimize();
    const StrategyResult second = strategy.optimize();

    CHECK(first.stints.size() == second.stints.size());
    CHECK(first.pitLaps == second.pitLaps);
    CHECK_NEAR(first.totalTime, second.totalTime, 0.0);  // exact: no arithmetic differs
    CHECK_NEAR(first.dpValue, second.dpValue, 0.0);

    bool identical = (first.stints.size() == second.stints.size());
    for (std::size_t i = 0; identical && i < first.stints.size(); ++i) {
        identical = (first.stints[i].compoundIndex == second.stints[i].compoundIndex) &&
                    (first.stints[i].laps == second.stints[i].laps) &&
                    (first.stints[i].startLap == second.stints[i].startLap);
    }
    CHECK_MSG(identical, "two optimize() calls must return byte-identical plans");

    // The same race run twice must finish identically, including the ordering.
    RaceSimulator raceA(track, compounds);
    RaceSimulator raceB(track, compounds);
    for (std::size_t c = 0; c < compounds.size(); ++c) {
        raceA.addDriver(static_cast<int>(c) + 1, "D", strategy.singleCompoundPlan(c));
        raceB.addDriver(static_cast<int>(c) + 1, "D", strategy.singleCompoundPlan(c));
    }
    raceA.runToFinish();
    raceB.runToFinish();

    const std::vector<Standing> tableA = raceA.standings();
    const std::vector<Standing> tableB = raceB.standings();
    CHECK(tableA.size() == tableB.size());
    for (std::size_t i = 0; i < tableA.size() && i < tableB.size(); ++i) {
        CHECK(tableA[i].driverIndex == tableB[i].driverIndex);
        CHECK_NEAR(tableA[i].gapToLeader, tableB[i].gapToLeader, 0.0);
    }

    // reset() must return the field to the grid exactly.
    raceA.reset();
    CHECK(raceA.lapsRun() == 0);
    for (const Driver& driver : raceA.drivers()) {
        CHECK(driver.lapsCompleted() == 0);
        CHECK(driver.stopsMade() == 0);
        CHECK(driver.tireAge() == 0);
        CHECK(driver.stintIndex() == 0U);
        CHECK_NEAR(driver.cumulativeTime(), 0.0, 0.0);
    }
    raceA.runToFinish();
    for (std::size_t i = 0; i < raceA.drivers().size(); ++i) {
        CHECK_NEAR(raceA.drivers()[i].cumulativeTime(), raceB.drivers()[i].cumulativeTime(),
                   0.0);
    }
}

void testToleranceHelpers() {
    beginSection("Common: float comparison helpers");

    CHECK(boxbox::nearlyEqual(1.0, 1.0 + boxbox::kTimeEpsilon / 2.0));
    CHECK(!boxbox::nearlyEqual(1.0, 1.0 + boxbox::kTimeEpsilon * 100.0));

    CHECK(boxbox::definitelyLess(1.0, 2.0));
    CHECK(!boxbox::definitelyLess(2.0, 1.0));
    CHECK_MSG(!boxbox::definitelyLess(1.0, 1.0 + boxbox::kTimeEpsilon / 2.0),
              "a tie within tolerance is not an improvement; this is the DP tie-break");

    CHECK(boxbox::isReachable(0.0));
    CHECK(!boxbox::isReachable(boxbox::kUnreachable));
    CHECK(boxbox::kClosureTolerance > boxbox::kTimeEpsilon);
}

void testStopsOutsideTheRace() {
    beginSection("Edge cases: a stop on lap 0, and a stop after the final lap");

    // A stop at the end of lap 0 is a zero-lap opening stint; a stop at the
    // end of the final lap is a zero-lap closing stint. Both plans below still
    // cover the 10-lap race exactly, so the lap-total check passes and ONLY
    // the zero-lap guard stands between each plan and a phantom pit stop.
    const std::vector<TireCompound> compounds = boxbox::defaultCompounds();
    const int laps = 10;
    const Track track = makeTrack(laps, 20.0, {90.0});
    const PitStrategy strategy(track, compounds);

    const std::vector<PlannedStint> stopOnLapZero = {PlannedStint{0U, 0}, PlannedStint{0U, laps}};
    const std::vector<PlannedStint> stopAfterFlag = {PlannedStint{0U, laps}, PlannedStint{0U, 0}};

    CHECK_MSG(!strategy.evaluatePlan(stopOnLapZero).feasible,
              "a stop at the end of lap 0 must be rejected");
    CHECK_MSG(!strategy.evaluatePlan(stopAfterFlag).feasible,
              "a stop at the end of the final lap must be rejected");

    // Control: the same distance with the empty stint left out is legal.
    const StrategyResult noStop = strategy.evaluatePlan({PlannedStint{0U, laps}});
    CHECK(noStop.feasible);
    CHECK(noStop.stopCount() == 0);

    // The same two shapes at both simulator entry points.
    CHECK_THROWS(Driver(1, "LAP-ZERO", asStints(stopOnLapZero, compounds)));
    CHECK_THROWS(Driver(2, "AFTER-FLAG", asStints(stopAfterFlag, compounds)));

    RaceSimulator simulator(track, compounds);
    CHECK_THROWS(simulator.addDriver(1, "LAP-ZERO", asStints(stopOnLapZero, compounds)));
    CHECK_THROWS(simulator.addDriver(2, "AFTER-FLAG", asStints(stopAfterFlag, compounds)));
    CHECK_MSG(simulator.drivers().empty(), "a rejected plan must not leave a car behind");

    // NFR-2: the lap total is summed once, at construction. It must still be
    // the true total.
    const Driver valid(3, "VALID", noStop.stints);
    CHECK(valid.plannedLaps() == laps);
    CHECK(!valid.finished());
}

void testOvertakeRuleSettings() {
    beginSection("FR-9: rule settings and validation");

    RaceSimulator simulator(makeTrack(10, 20.0, {90.0}), boxbox::defaultCompounds());

    CHECK_MSG(!simulator.overtakeRule().enabled, "overtaking must be off by default");
    CHECK_NEAR(simulator.overtakeRule().minGap, 0.5, boxbox::kTimeEpsilon);
    CHECK_NEAR(simulator.overtakeRule().passMargin, 1.0, boxbox::kTimeEpsilon);

    OvertakeRule good;
    good.enabled = true;
    good.minGap = 0.8;
    good.passMargin = 0.0;  // legal: any quicker car gets by
    simulator.setOvertakeRule(good);
    CHECK(simulator.overtakeRule().enabled);
    CHECK_NEAR(simulator.overtakeRule().minGap, 0.8, boxbox::kTimeEpsilon);
    CHECK_NEAR(simulator.overtakeRule().passMargin, 0.0, boxbox::kTimeEpsilon);

    const double inf = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();

    for (const double gap : {0.0, -0.5, boxbox::kMinOvertakeGap / 2.0, inf, nan}) {
        OvertakeRule bad = good;
        bad.minGap = gap;
        CHECK_THROWS(simulator.setOvertakeRule(bad));
    }
    for (const double margin : {-0.1, -inf, inf, nan}) {
        OvertakeRule bad = good;
        bad.passMargin = margin;
        CHECK_THROWS(simulator.setOvertakeRule(bad));
    }

    // A rejected rule leaves the previous one in place.
    CHECK_NEAR(simulator.overtakeRule().minGap, 0.8, boxbox::kTimeEpsilon);
    CHECK_NEAR(simulator.overtakeRule().passMargin, 0.0, boxbox::kTimeEpsilon);

    // The floor itself is accepted.
    OvertakeRule atFloor = good;
    atFloor.minGap = boxbox::kMinOvertakeGap;
    simulator.setOvertakeRule(atFloor);
    CHECK_NEAR(simulator.overtakeRule().minGap, boxbox::kMinOvertakeGap, boxbox::kTimeEpsilon);
}

void testOvertakingHandCases() {
    beginSection("FR-9: pass or hold, hand-computed");

    const std::vector<PlannedStint> fast3 = {PlannedStint{kFast, 3}};
    const double eps = boxbox::kTimeEpsilon;

    // (a) Equal pace from the grid. Every car starts at 0.0, so the grid is
    // car-number order. On lap 1 each car is held exactly minGap behind the
    // one ahead; after that it arrives exactly minGap behind - clear, not
    // caught - and loses nothing more.
    {
        const RaceSimulator race = flatRace({fast3, fast3, fast3}, true);
        const std::vector<Driver>& cars = race.drivers();
        CHECK_NEAR(cars[0].cumulativeTime(), 300.0, eps);
        CHECK_NEAR(cars[1].cumulativeTime(), 300.5, eps);
        CHECK_NEAR(cars[2].cumulativeTime(), 301.0, eps);
        CHECK_NEAR(cars[0].trafficLoss(), 0.0, eps);
        CHECK_NEAR(cars[1].trafficLoss(), 0.5, eps);
        CHECK_NEAR(cars[2].trafficLoss(), 1.0, eps);

        // With the rule off the same three cars never interact.
        const RaceSimulator noRule = flatRace({fast3, fast3, fast3}, false);
        for (const Driver& car : noRule.drivers()) {
            CHECK_NEAR(car.cumulativeTime(), 300.0, eps);
            CHECK_NEAR(car.trafficLoss(), 0.0, eps);
        }
    }

    // (b) Held by a slower car. Car 2 is 0.4 s a lap quicker - inside the
    // 1.0 s margin - so it can never get by:
    //   lap 1  car 1 crosses 100.4   car 2 would cross 100.0, held to 100.9  (0.9 lost)
    //   lap 2  car 1 crosses 200.8   car 2 would cross 200.9, held to 201.3  (0.4 lost)
    //   lap 3  car 1 crosses 301.2   car 2 would cross 301.3, held to 301.7  (0.4 lost)
    {
        const RaceSimulator race = flatRace({{PlannedStint{kSlow, 3}}, fast3}, true);
        const std::vector<Driver>& cars = race.drivers();
        CHECK_NEAR(cars[0].cumulativeTime(), 301.2, eps);
        CHECK_NEAR(cars[1].cumulativeTime(), 301.7, eps);
        CHECK_NEAR(cars[0].trafficLoss(), 0.0, eps);
        CHECK_NEAR(cars[1].trafficLoss(), 1.7, eps);

        const std::vector<Standing> table = race.standings();
        CHECK_MSG(table.size() == 2U && table[0].driverIndex == 0U,
                  "the held car must be classified behind the car holding it");
        if (table.size() == 2U) {
            CHECK_NEAR(table[1].gapToLeader, 0.5, eps);
        }

        // Without the rule the quicker car wins by 1.2 s: the hold reversed it.
        const RaceSimulator noRule = flatRace({{PlannedStint{kSlow, 3}}, fast3}, false);
        CHECK_NEAR(noRule.drivers()[0].cumulativeTime(), 301.2, eps);
        CHECK_NEAR(noRule.drivers()[1].cumulativeTime(), 300.0, eps);
    }

    // (c) A clear pass: 1.5 s a lap quicker, beyond the margin. Car 2 takes
    // the lead on lap 1 and loses nothing.
    {
        const RaceSimulator race = flatRace({{PlannedStint{kCrawl, 3}}, fast3}, true);
        const std::vector<Driver>& cars = race.drivers();
        CHECK_NEAR(cars[0].cumulativeTime(), 304.5, eps);
        CHECK_NEAR(cars[1].cumulativeTime(), 300.0, eps);
        CHECK_NEAR(cars[0].trafficLoss(), 0.0, eps);
        CHECK_NEAR(cars[1].trafficLoss(), 0.0, eps);
        CHECK_MSG(race.standings().front().driverIndex == 1U, "the quicker car must lead");
    }

    // (d) A pace advantage of exactly passMargin is enough to pass.
    {
        const RaceSimulator race = flatRace({{PlannedStint{kEdge, 3}}, fast3}, true);
        CHECK_NEAR(race.drivers()[0].cumulativeTime(), 303.0, eps);
        CHECK_NEAR(race.drivers()[1].cumulativeTime(), 300.0, eps);
        CHECK_NEAR(race.drivers()[1].trafficLoss(), 0.0, eps);
    }

    // (e) The pit lane is not raced. Car 1 stops at the end of lap 1. Car 2 is
    // held behind it on that lap - car 1 was on track in front of it - but car
    // 1's 20 s pit loss is never treated as on-track time for car 2:
    //   lap 1     car 1 crosses 100.0, then pits (120.0); car 2 held to 100.5
    //   laps 2-3  car 2 leads in clean air; car 1 rejoins 19.5 s back
    {
        const RaceSimulator race =
            flatRace({{PlannedStint{kFast, 1}, PlannedStint{kFast, 2}}, fast3}, true);
        const std::vector<Driver>& cars = race.drivers();
        CHECK(cars[0].stopsMade() == 1);
        CHECK_NEAR(cars[0].cumulativeTime(), 320.0, eps);
        CHECK_NEAR(cars[0].trafficLoss(), 0.0, eps);
        CHECK_NEAR(cars[1].cumulativeTime(), 300.5, eps);
        CHECK_NEAR(cars[1].trafficLoss(), 0.5, eps);
    }

    // (f) Cars on different lap counts never interact. Car 2 joins a lap late,
    // so it is always a lap down on car 1 and is never held by it, even though
    // its race clock is far smaller than car 1's.
    {
        const Track track = makeTrack(3, 20.0, {100.0});
        const PitStrategy strategy(track, flatCompounds());
        RaceSimulator simulator(track, flatCompounds());

        OvertakeRule rule;
        rule.enabled = true;
        rule.minGap = 0.5;
        rule.passMargin = 1.0;
        simulator.setOvertakeRule(rule);

        simulator.addDriver(1, "EARLY", strategy.evaluatePlan(fast3));
        simulator.advanceLap();
        simulator.addDriver(2, "LATE", strategy.evaluatePlan(fast3));
        for (int guard = 0; guard < 10 && simulator.advanceLap(); ++guard) {
            // advanceLap does the work; the guard bounds a broken loop
        }

        const std::vector<Driver>& cars = simulator.drivers();
        CHECK(simulator.finished());
        CHECK_NEAR(cars[0].cumulativeTime(), 300.0, eps);
        CHECK_NEAR(cars[1].cumulativeTime(), 300.0, eps);
        CHECK_NEAR(cars[1].trafficLoss(), 0.0, eps);
    }
}

void testOvertakingProperties() {
    beginSection("FR-9: properties over seeded fields");

    const std::vector<TireCompound> compounds = boxbox::defaultCompounds();
    const int lastCompound = static_cast<int>(compounds.size()) - 1;
    Lcg random(0x0BE7A11DULL);

    bool anyHeld = false;

    for (int trial = 0; trial < 30; ++trial) {
        // Each draw is its own statement: argument evaluation order is
        // unspecified, and the draws must happen in a fixed order (NFR-5).
        const int laps = random.nextInt(1, 60);
        const double pitLoss = random.nextDouble(0.0, 30.0);
        const double baseLap = random.nextDouble(60.0, 110.0);
        const Track track = makeTrack(laps, pitLoss, {baseLap});
        const PitStrategy strategy(track, compounds);

        // A field of random but legal plans, so cars meet on different
        // compounds, at different tire ages, and around each other's stops.
        const int carCount = random.nextInt(2, 8);
        std::vector<StrategyResult> plans;
        bool allFeasible = true;
        for (int car = 0; car < carCount; ++car) {
            std::vector<PlannedStint> plan;
            for (int remaining = laps; remaining > 0;) {
                const std::size_t c = static_cast<std::size_t>(random.nextInt(0, lastCompound));
                const int longest = std::min(remaining, compounds[c].maxStintLaps());
                const int length = random.nextInt(1, longest);
                plan.push_back(PlannedStint{c, length});
                remaining -= length;
            }
            plans.push_back(strategy.evaluatePlan(plan));
            allFeasible = allFeasible && plans.back().feasible;
        }

        OvertakeRule rule;
        rule.enabled = true;
        rule.minGap = random.nextDouble(0.1, 1.5);
        rule.passMargin = random.nextDouble(0.0, 2.0);

        std::ostringstream detail;
        detail << "trial " << trial << "  laps " << laps << "  cars " << carCount << "  minGap "
               << rule.minGap << "  passMargin " << rule.passMargin;

        CHECK_MSG(allFeasible, "every generated plan must be legal: " + detail.str());
        if (!allFeasible) {
            continue;
        }

        RaceSimulator off(track, compounds);  // never told about the rule
        RaceSimulator on(track, compounds);
        RaceSimulator onAgain(track, compounds);
        on.setOvertakeRule(rule);
        onAgain.setOvertakeRule(rule);

        for (int car = 0; car < carCount; ++car) {
            const std::string name = "CAR" + std::to_string(car + 1);
            const StrategyResult& plan = plans[static_cast<std::size_t>(car)];
            off.addDriver(car + 1, name, plan);
            on.addDriver(car + 1, name, plan);
            onAgain.addDriver(car + 1, name, plan);
        }
        off.runToFinish();
        on.runToFinish();
        onAgain.runToFinish();

        for (std::size_t i = 0; i < plans.size(); ++i) {
            const Driver& clean = off.drivers()[i];
            const Driver& raced = on.drivers()[i];

            // Off by default: every car runs exactly its own plan, untouched.
            CHECK_NEAR(clean.cumulativeTime(), plans[i].totalTime, boxbox::kClosureTolerance);
            CHECK_NEAR(clean.trafficLoss(), 0.0, 0.0);

            // On: the plan still runs lap for lap and stop for stop...
            CHECK_MSG(raced.lapsCompleted() == laps, detail.str());
            CHECK_MSG(raced.stopsMade() == plans[i].stopCount(), detail.str());

            // ...traffic can only cost time, and costs exactly what it recorded.
            CHECK_MSG(raced.trafficLoss() >= 0.0, detail.str());
            CHECK_MSG(raced.cumulativeTime() >=
                          clean.cumulativeTime() - boxbox::kClosureTolerance,
                      "traffic must never make a car faster: " + detail.str());
            CHECK_NEAR(raced.cumulativeTime() - clean.cumulativeTime(), raced.trafficLoss(),
                       boxbox::kClosureTolerance);

            // Deterministic: the same field twice gives the same race, exactly.
            CHECK_NEAR(raced.cumulativeTime(), onAgain.drivers()[i].cumulativeTime(), 0.0);

            anyHeld = anyHeld || raced.trafficLoss() > 0.0;
        }

        const std::vector<Standing> tableA = on.standings();
        const std::vector<Standing> tableB = onAgain.standings();
        bool sameOrder = (tableA.size() == tableB.size());
        for (std::size_t i = 0; sameOrder && i < tableA.size(); ++i) {
            sameOrder = (tableA[i].driverIndex == tableB[i].driverIndex);
        }
        CHECK_MSG(sameOrder, "identical input must give an identical classification: " +
                                 detail.str());

        // reset() must clear traffic losses with everything else, so that a
        // rerun reproduces the first run exactly.
        on.reset();
        for (const Driver& driver : on.drivers()) {
            CHECK_NEAR(driver.trafficLoss(), 0.0, 0.0);
        }
        on.runToFinish();
        for (std::size_t i = 0; i < plans.size(); ++i) {
            CHECK_NEAR(on.drivers()[i].cumulativeTime(), onAgain.drivers()[i].cumulativeTime(),
                       0.0);
        }

        // FR-8 with the rule ON: a lone car is never obstructed, so the
        // optimizer's own plan still closes within kClosureTolerance.
        const StrategyResult best = strategy.optimize();
        RaceSimulator solo(track, compounds);
        solo.setOvertakeRule(rule);
        solo.addDriver(1, "SOLO", best);
        solo.runToFinish();
        CHECK_NEAR(solo.drivers().front().cumulativeTime(), best.totalTime,
                   boxbox::kClosureTolerance);
        CHECK_NEAR(solo.drivers().front().trafficLoss(), 0.0, 0.0);
    }

    CHECK_MSG(anyHeld, "the seeded fields must actually exercise the hold branch");
}

}  // namespace

int main() {
    std::cout << "Box Box test harness\n";
    std::cout << "---------------------\n";

    testToleranceHelpers();
    testTrack();
    testTireCompound();
    testStintCostTable();
    testOptimizerAgainstHandComputation();
    testOptimizerAgainstBruteForce();
    testStintLengthsCoverRace();
    testZeroPitLossFavoursShortStints();
    testHighPitLossCollapsesStops();
    testCompoundSwitching();
    testLoopClosure();
    testSimulatorEdgeCases();
    testEvaluatePlan();
    testLeaderboard();
    testStandings();
    testDeterminism();
    testStopsOutsideTheRace();
    testOvertakeRuleSettings();
    testOvertakingHandCases();
    testOvertakingProperties();

    const TireTestTally tires = runTireModuleTests();
    g_checks += tires.checks;
    g_failures += tires.failures;

    const DriverTestTally drivers = runDriverAnalyticsTests();
    g_checks += drivers.checks;
    g_failures += drivers.failures;

    std::cout << "\n---------------------\n";
    std::cout << g_checks << " checks, " << g_failures << " failed\n";

    if (g_failures != 0) {
        std::cout << "FAILED\n";
        return 1;
    }
    std::cout << "OK\n";
    return 0;
}
