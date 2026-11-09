// Box Box: checks for the tire module (plan sec. 4.2, owner Maulshree).
//
// Covers SegmentTree, FenwickTree, TireAnalytics and RaceLapAnalytics. Every optimized
// structure is checked against a plain brute-force reference on seeded data,
// plus hand-worked cases. Same harness style as test_main.cpp: a failing
// check is recorded and the run carries on.
//
// The `tires` CLI handler is not linked here, because make test builds
// against the core objects only (PRD 7). It is exercised through bin/boxbox.

#include "TireModuleTests.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "boxbox/Common.hpp"
#include "boxbox/FenwickTree.hpp"
#include "boxbox/Leaderboard.hpp"
#include "boxbox/PitStrategy.hpp"
#include "boxbox/RaceLapAnalytics.hpp"
#include "boxbox/RaceSimulator.hpp"
#include "boxbox/SegmentTree.hpp"
#include "boxbox/TireAnalytics.hpp"
#include "boxbox/TireCompound.hpp"
#include "boxbox/Track.hpp"

namespace {

using boxbox::definitelyLess;
using boxbox::kTimeEpsilon;
using boxbox::PitStrategy;
using boxbox::TireCompound;
using boxbox::Track;
using boxbox::FenwickTree;
using boxbox::SegmentTree;
using boxbox::DegradationProfile;

// ---------------------------------------------------------------------------
// Harness
// ---------------------------------------------------------------------------

TireTestTally g_tally;
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

#define TCHECK(expr) reportCheck((expr), #expr, __FILE__, __LINE__, std::string())

#define TCHECK_NEAR(actual, expected, tolerance)                                        \
    reportCheck(std::fabs((actual) - (expected)) <= (tolerance),                        \
                #actual " ~= " #expected, __FILE__, __LINE__,                           \
                describe((actual), (expected), (tolerance)))

#define TCHECK_THROWS(statement, ExType)                                                \
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

/// Same seeded LCG as test_main.cpp: the inputs vary, the run does not (NFR-5).
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
    std::size_t nextIndex(std::size_t n) noexcept {
        return static_cast<std::size_t>(next() % static_cast<std::uint64_t>(n));
    }
    double nextDouble(double low, double high) noexcept {
        const double unit = static_cast<double>(next() % 1000001ULL) / 1000000.0;
        return low + (high - low) * unit;
    }

private:
    std::uint64_t state_;
};

// ---------------------------------------------------------------------------
// Brute-force references and fixtures
// ---------------------------------------------------------------------------

double bruteSum(const std::vector<double>& v, std::size_t b, std::size_t e) {
    double s = 0.0;
    for (std::size_t i = b; i < e; ++i) {
        s += v[i];
    }
    return s;
}

std::size_t bruteMinIndex(const std::vector<double>& v, std::size_t b, std::size_t e) {
    std::size_t best = b;
    for (std::size_t i = b + 1U; i < e; ++i) {
        if (v[i] < v[best]) {
            best = i;
        }
    }
    return best;
}

std::vector<double> randomValues(Lcg& rng, std::size_t n, bool withDuplicates) {
    std::vector<double> v(n);
    for (double& x : v) {
        x = withDuplicates ? static_cast<double>(rng.nextInt(0, 5)) : rng.nextDouble(-50, 50);
    }
    return v;
}

/// 90 s base lap. Lap count and pit loss do not affect tire analytics.
Track testTrack(int laps = 30, double pitLoss = 20.0) {
    Track track("Test", laps, pitLoss);
    track.addSector("S1", 30.0);
    track.addSector("S2", 30.0);
    track.addSector("S3", 30.0);
    return track;
}

DegradationProfile profileOf(const TireCompound& compound) {
    return DegradationProfile::fromStrategy(PitStrategy(testTrack(), {compound}), 0);
}

/// +0.5 s pace, 0.1 s/lap wear, 6-lap stint, cliff after age 3 adding 0.5 s/lap.
/// Lap times 90.5 90.6 90.7 90.8 | 91.4 92.0 - age 4 is the first that pays.
TireCompound handCompound() {
    return TireCompound("hand", 0.5, 0.1, 6, 3, 0.5);
}

TireCompound randomCompound(Lcg& rng) {
    const int maxStint = rng.nextInt(1, 40);
    const int cliffLap = (rng.nextInt(0, 3) == 0) ? -1 : rng.nextInt(0, 40);
    // One in five has zero wear, the case that exposed the averaging bug.
    const double wear = (rng.nextInt(0, 4) == 0) ? 0.0 : rng.nextDouble(0.0, 0.3);
    const double cliffRate = (rng.nextInt(0, 4) == 0) ? 0.0 : rng.nextDouble(0.0, 0.9);
    return TireCompound("r", rng.nextDouble(-1.0, 1.0), wear, maxStint, cliffLap, cliffRate);
}

/// The documented definitions, written as plain scans with no binary search.
std::optional<int> bruteFirstExceeds(const DegradationProfile& p, double limit) {
    for (std::size_t a = 0; a < p.degradationVsFresh().size(); ++a) {
        if (definitelyLess(limit, p.degradationVsFresh()[a])) {
            return static_cast<int>(a);
        }
    }
    return std::nullopt;
}

std::optional<int> bruteLongestUnder(const DegradationProfile& p, double limit) {
    std::optional<int> best;
    for (int k = 1; k <= p.maxStint(); ++k) {
        if (!definitelyLess(limit, p.averageLapTime()[static_cast<std::size_t>(k)])) {
            best = k;
        }
    }
    return best;
}

// ---------------------------------------------------------------------------
// SegmentTree
// ---------------------------------------------------------------------------

void testSegmentTreeBasics() {
    beginSection("Tire module: SegmentTree basics and invalid input");

    SegmentTree empty;
    TCHECK(empty.empty());
    TCHECK_NEAR(empty.rangeSum(0, 0), 0.0, 0.0);
    TCHECK_THROWS(empty.rangeMin(0, 0), std::invalid_argument);
    TCHECK_THROWS(empty.rangeSum(0, 1), std::out_of_range);
    TCHECK_THROWS(empty.valueAt(0), std::out_of_range);
    TCHECK_THROWS(empty.update(0, 1.0), std::out_of_range);

    SegmentTree one(std::vector<double>{7.5});
    TCHECK(one.size() == 1U);
    TCHECK_NEAR(one.rangeSum(0, 1), 7.5, 0.0);
    TCHECK_NEAR(one.rangeMin(0, 1), 7.5, 0.0);
    TCHECK(one.rangeMinIndex(0, 1) == 0U);
    one.update(0, -2.0);
    TCHECK_NEAR(one.valueAt(0), -2.0, 0.0);

    SegmentTree t(std::vector<double>{4, 2, 9, 2, 5});
    TCHECK_NEAR(t.rangeSum(0, 5), 22.0, kTimeEpsilon);
    TCHECK_NEAR(t.rangeSum(1, 4), 13.0, kTimeEpsilon);
    TCHECK_NEAR(t.rangeSum(2, 2), 0.0, 0.0);
    TCHECK_NEAR(t.rangeMin(0, 5), 2.0, 0.0);
    TCHECK(t.rangeMinIndex(0, 5) == 1U);  // tie with index 3: smaller index wins
    TCHECK(t.rangeMinIndex(2, 5) == 3U);
    TCHECK_THROWS(t.rangeSum(3, 2), std::out_of_range);
    TCHECK_THROWS(t.rangeSum(0, 6), std::out_of_range);
    TCHECK_THROWS(t.rangeMin(3, 3), std::invalid_argument);
    TCHECK_THROWS(t.update(5, 1.0), std::out_of_range);
    TCHECK_THROWS(t.update(0, std::numeric_limits<double>::quiet_NaN()), std::invalid_argument);
    TCHECK_THROWS(t.update(0, std::numeric_limits<double>::infinity()), std::invalid_argument);
    TCHECK_NEAR(t.valueAt(0), 4.0, 0.0);  // rejected updates changed nothing

    // A rejected rebuild keeps the old contents.
    TCHECK_THROWS(t.build(std::vector<double>{1.0, std::nan("")}), std::invalid_argument);
    TCHECK(t.size() == 5U);
    TCHECK_NEAR(t.rangeSum(0, 5), 22.0, kTimeEpsilon);
}

void testSegmentTreeAgainstBruteForce() {
    beginSection("Tire module: SegmentTree vs brute force, 60 seeded arrays + one of 20000");

    Lcg rng(20261104ULL);
    for (int trial = 0; trial < 60; ++trial) {
        const bool dupes = (trial % 2 == 0);
        const std::size_t n = static_cast<std::size_t>(rng.nextInt(1, 70));
        std::vector<double> v = randomValues(rng, n, dupes);
        SegmentTree t(v);

        for (int q = 0; q < 80; ++q) {
            if (q % 4 == 3) {  // interleave point updates
                const std::size_t i = rng.nextIndex(n);
                v[i] = dupes ? static_cast<double>(rng.nextInt(0, 5)) : rng.nextDouble(-50, 50);
                t.update(i, v[i]);
            }
            const std::size_t b = rng.nextIndex(n);
            const std::size_t e = b + 1U + rng.nextIndex(n - b);
            TCHECK_NEAR(t.rangeSum(b, e), bruteSum(v, b, e), 1e-7);
            TCHECK(t.rangeMinIndex(b, e) == bruteMinIndex(v, b, e));
            TCHECK_NEAR(t.rangeMin(b, e), v[bruteMinIndex(v, b, e)], 0.0);
        }
        for (std::size_t i = 0; i < n; ++i) {
            TCHECK_NEAR(t.rangeSum(i, i + 1U), v[i], 0.0);
        }
    }

    const std::size_t n = 20000;
    const std::vector<double> big = randomValues(rng, n, false);
    const SegmentTree t(big);
    for (int q = 0; q < 200; ++q) {
        const std::size_t b = rng.nextIndex(n);
        const std::size_t e = b + 1U + rng.nextIndex(n - b);
        TCHECK_NEAR(t.rangeSum(b, e), bruteSum(big, b, e), 1e-6);
        TCHECK(t.rangeMinIndex(b, e) == bruteMinIndex(big, b, e));
    }
}

void testSegmentTreeUpdateLocality() {
    beginSection("Tire module: SegmentTree point update touches only its ranges");

    const std::vector<double> v{3, 1, 4, 1, 5, 9, 2, 6};
    SegmentTree t(v);
    const std::size_t n = v.size();
    const std::size_t target = 5;

    std::vector<std::vector<double>> before(n + 1U, std::vector<double>(n + 1U, 0.0));
    for (std::size_t b = 0; b < n; ++b) {
        for (std::size_t e = b + 1U; e <= n; ++e) {
            before[b][e] = t.rangeSum(b, e);
        }
    }
    t.update(target, 100.0);  // 9 -> 100: +91 on every range that contains it
    for (std::size_t b = 0; b < n; ++b) {
        for (std::size_t e = b + 1U; e <= n; ++e) {
            const bool contains = b <= target && target < e;
            TCHECK_NEAR(t.rangeSum(b, e), before[b][e] + (contains ? 91.0 : 0.0), kTimeEpsilon);
        }
    }
    t.build(v);
    TCHECK_NEAR(t.rangeSum(0, n), 31.0, kTimeEpsilon);
}

// ---------------------------------------------------------------------------
// FenwickTree
// ---------------------------------------------------------------------------

void testFenwickBasics() {
    beginSection("Tire module: FenwickTree basics and invalid input");

    const FenwickTree empty;
    TCHECK(empty.empty());
    TCHECK_NEAR(empty.prefixSum(0), 0.0, 0.0);
    TCHECK_THROWS(empty.prefixSum(1), std::out_of_range);

    const FenwickTree zeros(4);
    TCHECK(zeros.size() == 4U);
    TCHECK_NEAR(zeros.prefixSum(4), 0.0, 0.0);

    FenwickTree f(std::vector<double>{1, 2, 3, 4, 5});
    TCHECK_NEAR(f.prefixSum(0), 0.0, 0.0);
    TCHECK_NEAR(f.prefixSum(3), 6.0, kTimeEpsilon);
    TCHECK_NEAR(f.prefixSum(5), 15.0, kTimeEpsilon);
    TCHECK_NEAR(f.rangeSum(1, 4), 9.0, kTimeEpsilon);
    TCHECK_NEAR(f.rangeSum(2, 2), 0.0, 0.0);
    f.add(2, 7.0);  // slot 2: 3 -> 10
    TCHECK_NEAR(f.rangeSum(2, 3), 10.0, kTimeEpsilon);
    TCHECK_NEAR(f.prefixSum(5), 22.0, kTimeEpsilon);
    f.add(4, -5.0);
    TCHECK_NEAR(f.rangeSum(4, 5), 0.0, kTimeEpsilon);
    TCHECK(f.size() == 5U);
    TCHECK_THROWS(f.rangeSum(3, 2), std::out_of_range);
    TCHECK_THROWS(f.rangeSum(0, 6), std::out_of_range);
    TCHECK_THROWS(f.prefixSum(6), std::out_of_range);
    TCHECK_THROWS(f.add(5, 1.0), std::out_of_range);
    TCHECK_THROWS(f.add(0, std::nan("")), std::invalid_argument);
    TCHECK_THROWS(f.add(0, std::numeric_limits<double>::infinity()), std::invalid_argument);
    TCHECK_NEAR(f.prefixSum(5), 17.0, kTimeEpsilon);  // rejected calls changed nothing
    TCHECK_THROWS(FenwickTree(std::vector<double>{std::numeric_limits<double>::infinity()}),
                  std::invalid_argument);
}

void testFenwickAgainstBruteForce() {
    beginSection("Tire module: FenwickTree vs direct summation, 60 seeded arrays");

    Lcg rng(424242ULL);
    for (int trial = 0; trial < 60; ++trial) {
        const std::size_t n = static_cast<std::size_t>(rng.nextInt(1, 90));
        std::vector<double> v = randomValues(rng, n, trial % 3 == 0);

        // The O(n) linear build must agree with n separate add() calls.
        FenwickTree linear(v);
        FenwickTree incremental(n);
        for (std::size_t i = 0; i < n; ++i) {
            incremental.add(i, v[i]);
        }
        for (std::size_t k = 0; k <= n; ++k) {
            TCHECK_NEAR(linear.prefixSum(k), incremental.prefixSum(k), 1e-7);
            TCHECK_NEAR(linear.prefixSum(k), bruteSum(v, 0, k), 1e-7);
        }

        for (int q = 0; q < 60; ++q) {
            const std::size_t i = rng.nextIndex(n);
            if (q % 2 == 0) {
                const double d = rng.nextDouble(-10, 10);
                v[i] += d;
                linear.add(i, d);
            } else {
                // Overwrite a slot: add the difference, the only update the
                // tree offers (a Fenwick tree stores sums, not the values).
                const double next = rng.nextDouble(-10, 10);
                linear.add(i, next - v[i]);
                v[i] = next;
            }
            std::size_t b = rng.nextIndex(n + 1U);
            std::size_t e = rng.nextIndex(n + 1U);
            if (b > e) {
                std::swap(b, e);
            }
            TCHECK_NEAR(linear.rangeSum(b, e), bruteSum(v, b, e), 1e-7);
        }
    }
}

// ---------------------------------------------------------------------------
// DegradationProfile
// ---------------------------------------------------------------------------

void testProfileHandWorked() {
    beginSection("Tire module: degradation profile, hand-worked compound");

    const DegradationProfile p = profileOf(handCompound());
    const std::vector<double> lap{90.5, 90.6, 90.7, 90.8, 91.4, 92.0};
    const std::vector<double> inc{0.0, 0.1, 0.1, 0.1, 0.6, 0.6};
    const std::vector<double> fresh{0.0, 0.1, 0.2, 0.3, 0.9, 1.5};
    const std::vector<double> cost{0.0, 90.5, 181.1, 271.8, 362.6, 454.0, 546.0};

    TCHECK(p.compound() == "hand");
    TCHECK(p.maxStint() == 6);
    TCHECK(p.lapTime().size() == 6U);
    TCHECK(p.stintCost().size() == 7U);
    TCHECK(p.averageLapTime().size() == 7U);
    for (std::size_t a = 0; a < 6U; ++a) {
        TCHECK_NEAR(p.lapTime()[a], lap[a], kTimeEpsilon);
        TCHECK_NEAR(p.incrementalDegradation()[a], inc[a], kTimeEpsilon);
        TCHECK_NEAR(p.degradationVsFresh()[a], fresh[a], kTimeEpsilon);
    }
    for (std::size_t k = 0; k <= 6U; ++k) {
        TCHECK_NEAR(p.stintCost()[k], cost[k], kTimeEpsilon);
    }
    for (std::size_t k = 1; k <= 6U; ++k) {
        TCHECK_NEAR(p.averageLapTime()[k], cost[k] / static_cast<double>(k), kTimeEpsilon);
    }
    TCHECK(p.lapTimeNonDecreasing());
}

void testCliffBoundaries() {
    beginSection("Tire module: cliff boundary, rate and edge cases");

    // Age 3 (= cliffLap) pays nothing; age 4 is the first that pays.
    const DegradationProfile p = profileOf(handCompound());
    TCHECK(p.cliffLap() == std::optional<int>(3));
    TCHECK(p.cliffActivationAge() == std::optional<int>(4));
    TCHECK_NEAR(p.cliffDegradationPerLap(), 0.5, 0.0);  // exact: read from TireCompound
    TCHECK_NEAR(p.lapTime()[3] - p.lapTime()[2], 0.1, kTimeEpsilon);
    TCHECK_NEAR(p.lapTime()[4] - p.lapTime()[3], 0.6, kTimeEpsilon);

    // A configured cliff with a 0 s/lap rate adds nothing and never activates.
    const DegradationProfile zeroRate = profileOf(TireCompound("zero", 0.0, 0.1, 10, 4, 0.0));
    TCHECK(zeroRate.cliffLap() == std::optional<int>(4));
    TCHECK(!zeroRate.cliffActivationAge().has_value());
    TCHECK_NEAR(zeroRate.cliffDegradationPerLap(), 0.0, 0.0);
    for (std::size_t a = 1; a < 10U; ++a) {
        TCHECK_NEAR(zeroRate.incrementalDegradation()[a], 0.1, kTimeEpsilon);
    }

    // Cliff from age 1: the rate is still reported, exactly.
    const DegradationProfile early = profileOf(TireCompound("early", 0.0, 0.1, 5, 0, 0.5));
    TCHECK(early.cliffActivationAge() == std::optional<int>(1));
    TCHECK_NEAR(early.cliffDegradationPerLap(), 0.5, 0.0);
    TCHECK_NEAR(early.lapTime()[1] - early.lapTime()[0], 0.6, kTimeEpsilon);

    // Activation on the last legal age, and just past it.
    const DegradationProfile edge = profileOf(TireCompound("edge", 0.0, 0.1, 5, 3, 1.0));
    TCHECK(edge.cliffActivationAge() == std::optional<int>(4));
    TCHECK_NEAR(edge.lapTime()[4], 91.4, kTimeEpsilon);
    const DegradationProfile late = profileOf(TireCompound("late", 0.0, 0.1, 5, 4, 1.0));
    TCHECK(late.cliffLap() == std::optional<int>(4));
    TCHECK(!late.cliffActivationAge().has_value());
    TCHECK_NEAR(late.lapTime()[4], 90.4, kTimeEpsilon);  // age 4 is not > cliffLap 4

    // No cliff at all, and zero wear.
    const DegradationProfile flat = profileOf(TireCompound("flat", 0.2, 0.0, 10));
    TCHECK(!flat.cliffLap().has_value());
    TCHECK(!flat.cliffActivationAge().has_value());
    TCHECK_NEAR(flat.cliffDegradationPerLap(), 0.0, 0.0);
    for (std::size_t a = 0; a < 10U; ++a) {
        TCHECK_NEAR(flat.lapTime()[a], 90.2, kTimeEpsilon);
        TCHECK_NEAR(flat.degradationVsFresh()[a], 0.0, 0.0);
    }
    TCHECK(flat.lapTimeNonDecreasing());

    // Shortest legal stint.
    const DegradationProfile one = profileOf(TireCompound("one", 0.0, 0.3, 1));
    TCHECK(one.maxStint() == 1);
    TCHECK_NEAR(one.stintCost()[1], 90.0, kTimeEpsilon);

    // The default set: cliffs activate at cliffLap + 1.
    const PitStrategy defaults(testTrack(50, 22.0), boxbox::defaultCompounds());
    const std::vector<DegradationProfile> profiles = boxbox::buildProfiles(defaults);
    TCHECK(profiles.size() == 3U);
    TCHECK(profiles[0].cliffActivationAge() == std::optional<int>(17));
    TCHECK(profiles[1].cliffActivationAge() == std::optional<int>(27));
    TCHECK(profiles[2].cliffActivationAge() == std::optional<int>(39));

    TCHECK_THROWS(DegradationProfile::fromStrategy(defaults, 3), std::out_of_range);
}

void testProfileAgainstSourcesOfTruth() {
    beginSection("Tire module: profiles vs lapDelta and PitStrategy, 300 seeded compounds");

    Lcg rng(99ULL);
    const Track track = testTrack();
    for (int trial = 0; trial < 300; ++trial) {
        const TireCompound c = randomCompound(rng);
        const PitStrategy strategy(track, {c});
        const DegradationProfile p = DegradationProfile::fromStrategy(strategy, 0);

        TCHECK(p.maxStint() == c.maxStintLaps());
        double running = 0.0;
        for (int a = 0; a < c.maxStintLaps(); ++a) {
            const auto ua = static_cast<std::size_t>(a);
            // Lap time: bit-identical to the canonical sum.
            TCHECK_NEAR(p.lapTime()[ua], track.baseLapTime() + c.lapDelta(a), 0.0);
            // Stint cost: bit-identical to the optimizer's prefix table...
            TCHECK_NEAR(p.stintCost()[ua + 1U], strategy.stintCost(0, a + 1), 0.0);
            // ...and, as an independent cross-check, equal to summing the laps.
            running += p.lapTime()[ua];
            TCHECK_NEAR(p.stintCost()[ua + 1U], running, 1e-7);
        }
        TCHECK(p.lapTimeNonDecreasing());  // TireCompound rejects negative wear

        // Cliff activation: the first age where lapDelta rises above its
        // linear part, found from TireCompound alone. Empty for no cliff, a
        // 0 s/lap rate, or a cliff past the legal stint.
        std::optional<int> firstPaying;
        for (int a = 0; a < c.maxStintLaps(); ++a) {
            const double linear = c.basePace() + c.degradationPerLap() * static_cast<double>(a);
            if (definitelyLess(linear, c.lapDelta(a))) {
                firstPaying = a;
                break;
            }
        }
        TCHECK(p.cliffActivationAge() == firstPaying);
        const double expectedRate = c.hasCliff() ? c.cliffDegradationPerLap() : 0.0;
        TCHECK_NEAR(p.cliffDegradationPerLap(), expectedRate, 0.0);

        for (std::size_t k = 1; k < p.averageLapTime().size(); ++k) {
            const double raw = p.stintCost()[k] / static_cast<double>(k);
            TCHECK_NEAR(p.averageLapTime()[k], raw, 1e-12);
            if (k > 1U) {
                TCHECK(!(p.averageLapTime()[k] < p.averageLapTime()[k - 1U]));
            }
        }
    }
}

void testProfileFromLapTimes() {
    beginSection("Tire module: profiles from recorded lap times");

    const DegradationProfile falling =
        DegradationProfile::fromLapTimes("rec", {90.0, 89.9, 89.8, 90.4});
    TCHECK(!falling.lapTimeNonDecreasing());
    TCHECK(!falling.cliffLap().has_value());
    TCHECK_NEAR(falling.stintCost()[4], 360.1, kTimeEpsilon);
    TCHECK_NEAR(falling.averageLapTime()[2], 89.95, kTimeEpsilon);  // raw, not smoothed

    const DegradationProfile rising = DegradationProfile::fromLapTimes("rec", {90.0, 90.0, 90.5});
    TCHECK(rising.lapTimeNonDecreasing());

    const DegradationProfile none = DegradationProfile::fromLapTimes("rec", {});
    TCHECK(none.maxStint() == 0);
    TCHECK(none.stintCost().size() == 1U);

    TCHECK_THROWS(DegradationProfile::fromLapTimes("bad", {90.0, std::nan("")}),
                  std::invalid_argument);
}

// ---------------------------------------------------------------------------
// TireRangeAnalyzer
// ---------------------------------------------------------------------------

void testRangeAnalyzer() {
    beginSection("Tire module: TireRangeAnalyzer hand-worked and what-if overrides");

    boxbox::TireRangeAnalyzer r(profileOf(handCompound()));
    TCHECK(r.ageCount() == 6U);
    TCHECK_NEAR(r.lapTimeSum(0, 6), 546.0, kTimeEpsilon);
    TCHECK_NEAR(r.lapTimeSum(2, 5), 90.7 + 90.8 + 91.4, kTimeEpsilon);
    TCHECK_NEAR(r.averageLapTime(0, 4), 362.6 / 4.0, kTimeEpsilon);
    TCHECK_NEAR(r.degradationSum(0, 6), 3.0, 1e-9);
    TCHECK_NEAR(r.degradationSum(4, 6), 2.4, 1e-9);
    TCHECK_NEAR(r.degradationSum(3, 3), 0.0, 0.0);
    TCHECK_NEAR(r.minLapTime(3, 6), 90.8, kTimeEpsilon);
    TCHECK(r.fastestAge(0, 6) == 0U);  // laps never fall, so the first age
    TCHECK_THROWS(r.averageLapTime(2, 2), std::invalid_argument);
    TCHECK_THROWS(r.averageLapTime(0, 7), std::out_of_range);
    TCHECK_THROWS(r.minLapTime(0, 7), std::out_of_range);
    TCHECK_THROWS(r.degradationSum(5, 4), std::out_of_range);

    // Age 1 gets a 2 s lock-up.
    r.overrideLapTime(1, 92.6);
    TCHECK_NEAR(r.lapTimeAt(1), 92.6, 0.0);
    TCHECK_NEAR(r.lapTimeSum(0, 6), 548.0, kTimeEpsilon);
    TCHECK_NEAR(r.degradationSum(0, 2), 2.1, 1e-9);
    TCHECK_NEAR(r.profile().lapTime()[1], 90.6, kTimeEpsilon);  // profile untouched

    // A quicker age moves the fastest lap: range-min doing real work.
    r.overrideLapTime(5, 89.0);
    TCHECK(r.fastestAge(0, 6) == 5U);
    TCHECK_NEAR(r.minLapTime(2, 6), 89.0, 0.0);

    // Overriding age 0 does not move the degradation reference.
    r.overrideLapTime(0, 95.5);
    TCHECK_NEAR(r.degradationSum(0, 1), 5.0, 1e-9);

    TCHECK_THROWS(r.overrideLapTime(6, 90.0), std::out_of_range);
    TCHECK_THROWS(r.overrideLapTime(2, std::nan("")), std::invalid_argument);
    TCHECK_NEAR(r.lapTimeAt(2), 90.7, kTimeEpsilon);  // rejected override changed nothing

    r.reset();
    TCHECK_NEAR(r.lapTimeSum(0, 6), 546.0, kTimeEpsilon);
    TCHECK(r.fastestAge(0, 6) == 0U);
}

void testRangeAnalyzerAgainstBruteForce() {
    beginSection("Tire module: TireRangeAnalyzer vs brute force, 40 seeded compounds");

    Lcg rng(31337ULL);
    for (int trial = 0; trial < 40; ++trial) {
        boxbox::TireRangeAnalyzer r(profileOf(randomCompound(rng)));
        std::vector<double> lap = r.profile().lapTime();
        const double fresh = lap.front();
        const std::size_t m = lap.size();
        for (int q = 0; q < 50; ++q) {
            if (q % 5 == 4) {
                const std::size_t a = rng.nextIndex(m);
                lap[a] = rng.nextDouble(85, 100);
                r.overrideLapTime(a, lap[a]);
            }
            const std::size_t b = rng.nextIndex(m);
            const std::size_t e = b + 1U + rng.nextIndex(m - b);
            TCHECK_NEAR(r.lapTimeSum(b, e), bruteSum(lap, b, e), 1e-7);
            TCHECK(r.fastestAge(b, e) == bruteMinIndex(lap, b, e));
            double deg = 0.0;
            for (std::size_t a = b; a < e; ++a) {
                deg += lap[a] - fresh;
            }
            TCHECK_NEAR(r.degradationSum(b, e), deg, 1e-7);
        }
    }
}

void testRangeAnalyzerMatchesPrefixTable() {
    beginSection("Tire module: unmodified analyzer matches the prefix table and lapDelta");

    // Agreed with Mantra: with no override, every range answer must equal
    // what the optimizer's prefix table and lapDelta already give - a sum is
    // a difference of two stint costs, and the minimum is the first age.
    Lcg rng(4242ULL);
    const Track track = testTrack();
    for (int trial = 0; trial < 300; ++trial) {
        const TireCompound c = randomCompound(rng);
        const PitStrategy strategy(track, {c});
        const boxbox::TireRangeAnalyzer r(DegradationProfile::fromStrategy(strategy, 0));
        const auto m = static_cast<std::size_t>(c.maxStintLaps());
        const double freshLap = track.baseLapTime() + c.lapDelta(0);
        for (int q = 0; q < 20; ++q) {
            const std::size_t b = rng.nextIndex(m);
            const std::size_t e = b + 1U + rng.nextIndex(m - b);
            const double prefixSum = strategy.stintCost(0, static_cast<int>(e)) -
                                     strategy.stintCost(0, static_cast<int>(b));
            TCHECK_NEAR(r.lapTimeSum(b, e), prefixSum, 1e-7);
            TCHECK(r.fastestAge(b, e) == b);
            TCHECK_NEAR(r.minLapTime(b, e), track.baseLapTime() + c.lapDelta(static_cast<int>(b)),
                        0.0);
            TCHECK_NEAR(r.degradationSum(b, e),
                        prefixSum - static_cast<double>(e - b) * freshLap, 1e-7);
        }
    }
}

void testOverridesStayWhatIf() {
    beginSection("Tire module: overrides never reach PitStrategy or RaceSimulator");

    // Agreed with Mantra: an override is a what-if on the analyzer's own copy
    // of the lap times. The optimizer and the simulator must keep using the
    // one lap-time model, so nothing they compute may change.
    const Track track = testTrack(40, 21.0);
    const std::vector<TireCompound> compounds = boxbox::defaultCompounds();
    const PitStrategy strategy(track, compounds);

    std::vector<std::vector<double>> costsBefore(compounds.size());
    for (std::size_t c = 0; c < compounds.size(); ++c) {
        for (int k = 0; k <= compounds[c].maxStintLaps(); ++k) {
            costsBefore[c].push_back(strategy.stintCost(c, k));
        }
    }
    const boxbox::StrategyResult planBefore = strategy.optimize();
    boxbox::RaceSimulator raceBefore(track, compounds);
    raceBefore.addDriver(1, "OPT", planBefore);
    raceBefore.runToFinish();

    // Override every age of every compound with a wildly different time.
    for (std::size_t c = 0; c < compounds.size(); ++c) {
        boxbox::TireRangeAnalyzer r(DegradationProfile::fromStrategy(strategy, c));
        for (std::size_t a = 0; a < r.ageCount(); ++a) {
            r.overrideLapTime(a, 60.0 + static_cast<double>(a));
        }
        TCHECK_NEAR(r.lapTimeAt(0), 60.0, 0.0);
        for (std::size_t a = 0; a < r.ageCount(); ++a) {  // the profile is untouched
            TCHECK_NEAR(r.profile().lapTime()[a],
                        track.baseLapTime() + compounds[c].lapDelta(static_cast<int>(a)), 0.0);
        }
    }

    for (std::size_t c = 0; c < compounds.size(); ++c) {
        for (int k = 0; k <= compounds[c].maxStintLaps(); ++k) {
            TCHECK_NEAR(strategy.stintCost(c, k), costsBefore[c][static_cast<std::size_t>(k)],
                        0.0);
        }
    }
    const boxbox::StrategyResult planAfter = strategy.optimize();
    TCHECK_NEAR(planAfter.totalTime, planBefore.totalTime, 0.0);
    TCHECK(planAfter.stints.size() == planBefore.stints.size());
    for (std::size_t i = 0; i < planAfter.stints.size() && i < planBefore.stints.size(); ++i) {
        TCHECK(planAfter.stints[i].compoundIndex == planBefore.stints[i].compoundIndex);
        TCHECK(planAfter.stints[i].laps == planBefore.stints[i].laps);
    }
    boxbox::RaceSimulator raceAfter(track, compounds);
    raceAfter.addDriver(1, "OPT", planAfter);
    raceAfter.runToFinish();
    TCHECK_NEAR(raceAfter.drivers().front().cumulativeTime(),
                raceBefore.drivers().front().cumulativeTime(), 0.0);
    TCHECK_NEAR(raceAfter.drivers().front().cumulativeTime(), planBefore.totalTime,
                boxbox::kClosureTolerance);
}

// ---------------------------------------------------------------------------
// Threshold queries
// ---------------------------------------------------------------------------

void testThresholdHandWorked() {
    beginSection("Tire module: threshold queries, hand-worked compound");
    const DegradationProfile p = profileOf(handCompound());

    boxbox::ThresholdResult r = boxbox::firstAgeDegradationExceeds(p, 0.3);
    TCHECK(r.usedBinarySearch);
    TCHECK(r.value == std::optional<int>(4));  // 0.3 at age 3 is not more than 0.3
    TCHECK(boxbox::firstAgeDegradationExceeds(p, 0.25).value == std::optional<int>(3));
    TCHECK(boxbox::firstAgeDegradationExceeds(p, -1.0).value == std::optional<int>(0));
    TCHECK(!boxbox::firstAgeDegradationExceeds(p, 1.5).value.has_value());

    r = boxbox::longestStintUnderAverage(p, 90.65);  // k=4 averages 90.65 exactly
    TCHECK(r.usedBinarySearch);
    TCHECK(r.value == std::optional<int>(4));
    TCHECK(!boxbox::longestStintUnderAverage(p, 90.4).value.has_value());
    TCHECK(boxbox::longestStintUnderAverage(p, 1000.0).value == std::optional<int>(6));

    TCHECK((boxbox::stintLengthsWithin(p, 90.7, 0.3) == std::vector<int>{1, 2, 3, 4}));
    TCHECK((boxbox::stintLengthsWithin(p, 90.7, 0.15) == std::vector<int>{1, 2}));

    TCHECK_THROWS(boxbox::firstAgeDegradationExceeds(p, std::nan("")), std::invalid_argument);
    TCHECK_THROWS(boxbox::longestStintUnderAverage(p, std::numeric_limits<double>::infinity()),
                  std::invalid_argument);
    TCHECK_THROWS(boxbox::stintLengthsWithin(p, 90.0, std::nan("")), std::invalid_argument);
}

void testThresholdScanPath() {
    beginSection("Tire module: threshold queries on non-monotonic recorded laps");

    // Laps that fall and then climb. TireCompound cannot produce this, but
    // recorded data can, and the scan path must stay correct.
    const DegradationProfile p = DegradationProfile::fromLapTimes(
        "rec", {90.0, 89.9, 89.8, 89.7, 89.6, 90.1, 90.6, 91.1, 91.6, 92.1});
    boxbox::ThresholdResult r = boxbox::firstAgeDegradationExceeds(p, 0.05);
    TCHECK(!r.usedBinarySearch);
    TCHECK(r.value == std::optional<int>(5));

    // Means: k=7 is 629.7 / 7 = 89.957, k=8 is 720.8 / 8 = 90.1.
    r = boxbox::longestStintUnderAverage(p, 90.0);
    TCHECK(!r.usedBinarySearch);
    TCHECK(r.value == std::optional<int>(7));

    // The fitting stint lengths are NOT a prefix here: k=1 fails, k=3 fits.
    // A binary search would get this wrong; the scan must not.
    const DegradationProfile odd =
        DegradationProfile::fromLapTimes("rec", {91.0, 89.0, 89.0, 92.0});
    r = boxbox::longestStintUnderAverage(odd, 89.8);  // means 91.0, 90.0, 89.667, 90.25
    TCHECK(!r.usedBinarySearch);
    TCHECK(r.value == std::optional<int>(3));
}

void testThresholdAgainstBruteForce() {
    beginSection("Tire module: threshold queries vs brute force, 300 seeded compounds");

    Lcg rng(5150ULL);
    for (int trial = 0; trial < 300; ++trial) {
        const DegradationProfile p = profileOf(randomCompound(rng));
        for (int q = 0; q < 6; ++q) {
            const double degLimit = rng.nextDouble(-0.5, 6.0);
            const double avgLimit = 90.0 + rng.nextDouble(-2.0, 4.0);
            const boxbox::ThresholdResult r1 = boxbox::firstAgeDegradationExceeds(p, degLimit);
            const boxbox::ThresholdResult r2 = boxbox::longestStintUnderAverage(p, avgLimit);
            TCHECK(r1.usedBinarySearch && r2.usedBinarySearch);
            TCHECK(r1.value == bruteFirstExceeds(p, degLimit));
            TCHECK(r2.value == bruteLongestUnder(p, avgLimit));
        }
    }
}

void testThresholdZeroWearEdges() {
    beginSection("Tire module: binary search at the tolerance edge, zero and tiny wear");

    // The case that broke the previous version: with zero or tiny wear the
    // raw mean stintCost[k] / k can dip by a few 1e-14 s, and a threshold
    // placed on that edge made binary search and scan disagree. Thresholds
    // here sit exactly on each tolerance edge and one rounding step either side.
    Lcg rng(777ULL);
    for (int trial = 0; trial < 400; ++trial) {
        const double wear = (trial % 2 == 0) ? 0.0 : 1e-13 * rng.nextDouble(0.0, 1.0);
        const TireCompound c("w", rng.nextDouble(-1.0, 1.0), wear, rng.nextInt(1, 60),
                             (trial % 3 == 0) ? -1 : rng.nextInt(0, 60), rng.nextDouble(0.0, 1.0));
        const DegradationProfile p = profileOf(c);
        for (std::size_t k = 1; k < p.averageLapTime().size(); ++k) {
            const double edge = p.averageLapTime()[k] - kTimeEpsilon;
            const std::vector<double> limits{edge, std::nextafter(edge, -1e300),
                                             std::nextafter(edge, 1e300)};
            for (const double limit : limits) {
                TCHECK(boxbox::longestStintUnderAverage(p, limit).value ==
                       bruteLongestUnder(p, limit));
            }
        }
        for (const double d : p.degradationVsFresh()) {
            const double edge = d - kTimeEpsilon;
            const std::vector<double> limits{edge, std::nextafter(edge, -1e300),
                                             std::nextafter(edge, 1e300)};
            for (const double limit : limits) {
                TCHECK(boxbox::firstAgeDegradationExceeds(p, limit).value ==
                       bruteFirstExceeds(p, limit));
            }
        }
    }
}

/// Independent reference for compareCompounds: first label every compared
/// age -1 (A faster), +1 (A slower) or 0 (tie), then keep only the non-tied
/// labels and report each position where the label differs from the one
/// before it.
boxbox::CompoundComparison bruteCompare(const DegradationProfile& a, const DegradationProfile& b) {
    boxbox::CompoundComparison r;
    const std::size_t n = std::min(a.lapTime().size(), b.lapTime().size());
    r.agesCompared = static_cast<int>(n);
    std::vector<std::pair<int, int>> nonTied;  // (age, label)
    for (std::size_t age = 0; age < n; ++age) {
        int label = 0;
        if (a.lapTime()[age] > b.lapTime()[age] + kTimeEpsilon) {
            label = 1;
        } else if (b.lapTime()[age] > a.lapTime()[age] + kTimeEpsilon) {
            label = -1;
        }
        if (label == 1 && !r.firstSlowerAge) {
            r.firstSlowerAge = static_cast<int>(age);
        }
        if (label != 0) {
            nonTied.emplace_back(static_cast<int>(age), label);
        }
    }
    for (std::size_t i = 1; i < nonTied.size(); ++i) {
        if (nonTied[i].second != nonTied[i - 1U].second) {
            r.switchAges.push_back(nonTied[i].first);
        }
    }
    return r;
}

bool sameComparison(const boxbox::CompoundComparison& x, const boxbox::CompoundComparison& y) {
    return x.agesCompared == y.agesCompared && x.firstSlowerAge == y.firstSlowerAge &&
           x.switchAges == y.switchAges;
}

DegradationProfile laps(const std::vector<double>& times) {
    return DegradationProfile::fromLapTimes("rec", times);
}

void testCompareCompoundsCases() {
    beginSection("Tire module: compareCompounds - switches, ties, no crossover, range edges");
    using boxbox::compareCompounds;
    using boxbox::CompoundComparison;

    // Several switches from a cliff: A is 0.5 s slower new, wears 0, cliff
    // after age 10 at 1.0 s/lap; B wears 0.2 s/lap. Slower, faster from 3,
    // slower again from 12.
    const DegradationProfile cliffA = profileOf(TireCompound("A", 0.5, 0.0, 20, 10, 1.0));
    const DegradationProfile linearB = profileOf(TireCompound("B", 0.0, 0.2, 20));
    CompoundComparison c = compareCompounds(cliffA, linearB);
    TCHECK(c.agesCompared == 20);
    TCHECK(c.firstSlowerAge == std::optional<int>(0));
    TCHECK((c.switchAges == std::vector<int>{3, 12}));

    // firstSlowerAge is the FIRST slower age even though A later leads.
    c = compareCompounds(laps({91, 89, 89, 91}), laps({90, 90, 90, 90}));
    TCHECK(c.firstSlowerAge == std::optional<int>(0));
    TCHECK((c.switchAges == std::vector<int>{1, 3}));

    // Many switches.
    c = compareCompounds(laps({91, 89, 91, 89, 91}), laps({90, 90, 90, 90, 90}));
    TCHECK((c.switchAges == std::vector<int>{1, 2, 3, 4}));

    // One switch each way, and the symmetric view.
    const DegradationProfile fast = profileOf(TireCompound("fast", -0.5, 0.3, 10));
    const DegradationProfile slow = profileOf(TireCompound("slow", 0.0, 0.1, 20));
    c = compareCompounds(fast, slow);  // gap -0.5 + 0.2 * age
    TCHECK(c.agesCompared == 10);
    TCHECK(c.firstSlowerAge == std::optional<int>(3));
    TCHECK((c.switchAges == std::vector<int>{3}));
    c = compareCompounds(slow, fast);
    TCHECK(c.firstSlowerAge == std::optional<int>(0));  // slower when new
    TCHECK((c.switchAges == std::vector<int>{3}));

    // A cliff causes the only switch.
    c = compareCompounds(profileOf(TireCompound("soft", -1.0, 0.05, 20, 5, 1.0)),
                         profileOf(TireCompound("hard", 0.0, 0.05, 20)));
    TCHECK(c.firstSlowerAge == std::optional<int>(7));
    TCHECK((c.switchAges == std::vector<int>{7}));

    // No crossover: one leads throughout, or the two are identical.
    c = compareCompounds(laps({89, 89.5, 89.9}), laps({90, 90, 90}));
    TCHECK(!c.firstSlowerAge.has_value());
    TCHECK(c.switchAges.empty());
    c = compareCompounds(slow, slow);
    TCHECK(!c.firstSlowerAge.has_value());
    TCHECK(c.switchAges.empty());

    // Ties.
    // slower, tie, slower: the tie does not create a switch
    c = compareCompounds(laps({91, 90, 91}), laps({90, 90, 90}));
    TCHECK(c.firstSlowerAge == std::optional<int>(0));
    TCHECK(c.switchAges.empty());
    // slower, tie, tie, faster: one switch, at the first non-tied age
    c = compareCompounds(laps({91, 90, 90, 89}), laps({90, 90, 90, 90}));
    TCHECK((c.switchAges == std::vector<int>{3}));
    // tied at the start: the first non-tied age only sets the leader
    c = compareCompounds(laps({90, 90, 91}), laps({90, 90, 90}));
    TCHECK(c.firstSlowerAge == std::optional<int>(2));
    TCHECK(c.switchAges.empty());
    // every age tied
    c = compareCompounds(laps({90, 90.5}), laps({90, 90.5}));
    TCHECK(!c.firstSlowerAge.has_value());
    TCHECK(c.switchAges.empty());
    // inside kTimeEpsilon is a tie; just past it is slower
    c = compareCompounds(laps({90.0 + 0.5 * kTimeEpsilon}), laps({90.0}));
    TCHECK(!c.firstSlowerAge.has_value());
    c = compareCompounds(laps({90.0 + 4.0 * kTimeEpsilon}), laps({90.0}));
    TCHECK(c.firstSlowerAge == std::optional<int>(0));
    // a tie between two compounds' real lap times (A: no wear, cliff after
    // age 2 at 0.5; B: 0.25 wear): 0.5 slower, 0.25 slower, tied, 0.25 slower
    c = compareCompounds(profileOf(TireCompound("A", 0.5, 0.0, 4, 2, 0.5)),
                         profileOf(TireCompound("B", 0.0, 0.25, 4)));
    TCHECK(c.firstSlowerAge == std::optional<int>(0));
    TCHECK(c.switchAges.empty());

    // Legal-range boundaries.
    // crossover on the first legal age
    c = compareCompounds(laps({91, 89}), laps({90, 90}));
    TCHECK(c.firstSlowerAge == std::optional<int>(0));
    TCHECK((c.switchAges == std::vector<int>{1}));
    // crossover on the last age legal on both
    c = compareCompounds(laps({89, 89, 91}), laps({90, 90, 90}));
    TCHECK(c.agesCompared == 3);
    TCHECK(c.firstSlowerAge == std::optional<int>(2));
    TCHECK((c.switchAges == std::vector<int>{2}));
    // a crossover just past the shorter stint is not reported, either way round
    c = compareCompounds(laps({89, 89, 89}), laps({90, 90, 90, 80, 80}));
    TCHECK(c.agesCompared == 3);
    TCHECK(!c.firstSlowerAge.has_value());
    TCHECK(c.switchAges.empty());
    c = compareCompounds(laps({90, 90, 90, 95}), laps({91, 91, 91}));
    TCHECK(c.agesCompared == 3);
    TCHECK(!c.firstSlowerAge.has_value());
    // the soft compound's 6-lap stint ends before its cliff makes it slower
    c = compareCompounds(profileOf(TireCompound("ss", -1.0, 0.05, 6, 5, 1.0)),
                         profileOf(TireCompound("hard", 0.0, 0.05, 20)));
    TCHECK(c.agesCompared == 6);
    TCHECK(!c.firstSlowerAge.has_value());
    // a one-lap compound
    c = compareCompounds(laps({90}), laps({91, 80}));
    TCHECK(c.agesCompared == 1);
    TCHECK(!c.firstSlowerAge.has_value());
    TCHECK(c.switchAges.empty());
}

void testCompareCompoundsAgainstBruteForce() {
    beginSection("Tire module: compareCompounds vs brute force, 400 seeded pairs");

    Lcg rng(8086ULL);
    for (int trial = 0; trial < 400; ++trial) {
        DegradationProfile a = profileOf(randomCompound(rng));
        DegradationProfile b = profileOf(randomCompound(rng));
        if (trial % 4 == 0) {
            // Coarse recorded laps, so ties and repeated switches are common.
            std::vector<double> la;
            std::vector<double> lb;
            const int n = rng.nextInt(1, 30);
            for (int i = 0; i < n; ++i) {
                la.push_back(90.0 + static_cast<double>(rng.nextInt(0, 2)));
                lb.push_back(90.0 + static_cast<double>(rng.nextInt(0, 2)));
            }
            a = laps(la);
            b = laps(lb);
        }
        if (trial % 4 == 1) {
            // Gaps right around the tolerance: tied at half of kTimeEpsilon,
            // different at twice it.
            const std::vector<double> offsets{0.0, 0.5 * kTimeEpsilon, -0.5 * kTimeEpsilon,
                                              2.0 * kTimeEpsilon, -2.0 * kTimeEpsilon};
            std::vector<double> la;
            std::vector<double> lb;
            const int n = rng.nextInt(1, 30);
            for (int i = 0; i < n; ++i) {
                const double base = 90.0 + 0.125 * static_cast<double>(i);
                la.push_back(base);
                lb.push_back(base + offsets[rng.nextIndex(offsets.size())]);
            }
            a = laps(la);
            b = laps(lb);
        }
        const boxbox::CompoundComparison ab = boxbox::compareCompounds(a, b);
        const boxbox::CompoundComparison ba = boxbox::compareCompounds(b, a);
        TCHECK(sameComparison(ab, bruteCompare(a, b)));
        TCHECK(ab.switchAges == ba.switchAges);  // a switch is a switch either way round
        TCHECK(sameComparison(ab, boxbox::compareCompounds(a, b)));  // deterministic
    }
}

void testFromCompoundMatchesFromStrategy() {
    beginSection("Tire module: fromCompound equals fromStrategy bit for bit, 300 compounds");

    Lcg rng(1357ULL);
    const Track track = testTrack();
    for (int trial = 0; trial < 300; ++trial) {
        const TireCompound c = randomCompound(rng);
        const DegradationProfile direct = DegradationProfile::fromCompound(track, c);
        const DegradationProfile adapted =
            DegradationProfile::fromStrategy(PitStrategy(track, {c}), 0);
        TCHECK(direct.compound() == adapted.compound());
        TCHECK(direct.maxStint() == adapted.maxStint());
        TCHECK(direct.maxStint() == c.maxStintLaps());
        TCHECK(direct.cliffLap() == adapted.cliffLap());
        TCHECK(direct.cliffActivationAge() == adapted.cliffActivationAge());
        TCHECK_NEAR(direct.cliffDegradationPerLap(), adapted.cliffDegradationPerLap(), 0.0);
        TCHECK(direct.lapTimeNonDecreasing() == adapted.lapTimeNonDecreasing());
        bool identical = direct.lapTime().size() == adapted.lapTime().size() &&
                         direct.stintCost().size() == adapted.stintCost().size();
        for (std::size_t i = 0; identical && i < direct.lapTime().size(); ++i) {
            identical = boxbox::nearlyEqual(direct.lapTime()[i], adapted.lapTime()[i], 0.0) &&
                        boxbox::nearlyEqual(direct.degradationVsFresh()[i],
                                            adapted.degradationVsFresh()[i], 0.0);
        }
        for (std::size_t k = 0; identical && k < direct.stintCost().size(); ++k) {
            identical = boxbox::nearlyEqual(direct.stintCost()[k], adapted.stintCost()[k], 0.0) &&
                        boxbox::nearlyEqual(direct.averageLapTime()[k],
                                            adapted.averageLapTime()[k], 0.0);
        }
        TCHECK(identical);

        // The documented age and stint mapping: a k-lap stint runs ages
        // 0 .. k-1, so its cost is the sum of lapTime()[0 .. k-1], and its
        // last lap is lapTime()[k - 1].
        TCHECK(direct.lapTime().size() == static_cast<std::size_t>(c.maxStintLaps()));
        TCHECK(direct.stintCost().size() == static_cast<std::size_t>(c.maxStintLaps()) + 1U);
        const auto k = static_cast<std::size_t>(c.maxStintLaps());
        TCHECK_NEAR(direct.stintCost()[k], bruteSum(direct.lapTime(), 0, k), 1e-7);
        TCHECK_NEAR(direct.lapTime()[k - 1U],
                    track.baseLapTime() + c.lapDelta(c.maxStintLaps() - 1), 0.0);
    }
}

void testDeterminism() {
    beginSection("Tire module: repeated runs are identical");

    const PitStrategy strategy(testTrack(50, 22.0), boxbox::defaultCompounds());
    const std::vector<DegradationProfile> a = boxbox::buildProfiles(strategy);
    const std::vector<DegradationProfile> b = boxbox::buildProfiles(strategy);
    TCHECK(a.size() == b.size());
    for (std::size_t c = 0; c < a.size() && c < b.size(); ++c) {
        TCHECK(a[c].lapTime().size() == b[c].lapTime().size());
        for (std::size_t i = 0; i < a[c].lapTime().size(); ++i) {
            // Exact: identical arithmetic in the same order (as test_main.cpp).
            TCHECK_NEAR(a[c].lapTime()[i], b[c].lapTime()[i], 0.0);
            TCHECK_NEAR(a[c].stintCost()[i + 1U], b[c].stintCost()[i + 1U], 0.0);
            TCHECK_NEAR(a[c].averageLapTime()[i + 1U], b[c].averageLapTime()[i + 1U], 0.0);
        }
    }
}

// ---------------------------------------------------------------------------
// RaceLapAnalytics: lap-time index, recorder, position swaps
// ---------------------------------------------------------------------------

/// Every pair compared directly: the O(D^2) reference for countOrderSwaps.
std::int64_t bruteSwaps(const std::vector<int>& before, const std::vector<int>& after) {
    std::int64_t swaps = 0;
    for (std::size_t i = 0; i < after.size(); ++i) {
        for (std::size_t j = i + 1U; j < after.size(); ++j) {
            const auto bi = std::find(before.begin(), before.end(), after[i]);
            const auto bj = std::find(before.begin(), before.end(), after[j]);
            if (bj < bi) {  // j now behind i, but j started ahead
                ++swaps;
            }
        }
    }
    return swaps;
}

/// Running order by sorting with the Leaderboard's own rule: a reference that
/// uses no heap.
std::vector<int> bruteRunningOrder(const boxbox::RaceSimulator& sim) {
    std::vector<boxbox::LeaderboardEntry> entries = sim.snapshotEntries();
    std::sort(entries.begin(), entries.end(), boxbox::Leaderboard::isAhead);
    std::vector<int> order;
    for (const boxbox::LeaderboardEntry& e : entries) {
        order.push_back(e.driverNumber);
    }
    return order;
}

void testLapTimeIndexBasics() {
    beginSection("Race laps: LapTimeIndex basics, ties and invalid input");

    TCHECK_THROWS(boxbox::LapTimeIndex(-1), std::invalid_argument);
    const boxbox::LapTimeIndex none(0);
    TCHECK(none.raceLaps() == 0 && none.recordedLaps() == 0);

    boxbox::LapTimeIndex idx(5);
    TCHECK_THROWS(idx.totalTime(1, 1), std::out_of_range);  // nothing recorded yet
    idx.recordLap(1, 91.0);
    idx.recordLap(2, 89.5);
    idx.recordLap(3, 89.5);  // tie with lap 2
    TCHECK(idx.recordedLaps() == 3);
    TCHECK_NEAR(idx.lapTime(2), 89.5, 0.0);
    TCHECK_NEAR(idx.totalTime(1, 3), 270.0, kTimeEpsilon);
    TCHECK_NEAR(idx.totalTime(3, 3), 89.5, 0.0);
    TCHECK(idx.fastestLap(1, 3).lap == 2);  // lower lap wins the tie
    TCHECK_NEAR(idx.fastestLap(1, 3).seconds, 89.5, 0.0);
    TCHECK(idx.fastestLap(3, 3).lap == 3);
    TCHECK(idx.fastestLap(1, 1).lap == 1);

    TCHECK_THROWS(idx.recordLap(5, 90.0), std::invalid_argument);  // lap 4 skipped
    TCHECK_THROWS(idx.recordLap(3, 90.0), std::invalid_argument);  // already recorded
    TCHECK_THROWS(idx.recordLap(4, std::nan("")), std::invalid_argument);
    TCHECK(idx.recordedLaps() == 3);  // rejected calls recorded nothing
    TCHECK_THROWS(idx.totalTime(0, 2), std::out_of_range);
    TCHECK_THROWS(idx.totalTime(3, 2), std::out_of_range);
    TCHECK_THROWS(idx.totalTime(2, 4), std::out_of_range);
    TCHECK_THROWS(idx.fastestLap(4, 4), std::out_of_range);
    TCHECK_THROWS(idx.lapTime(4), std::out_of_range);

    idx.recordLap(4, 88.0);
    idx.recordLap(5, 92.0);
    TCHECK(idx.fastestLap(1, 5).lap == 4);
    TCHECK_THROWS(idx.recordLap(6, 90.0), std::invalid_argument);  // past the race
}

void testLapTimeIndexAgainstBruteForce() {
    beginSection("Race laps: LapTimeIndex vs brute force, 60 seeded races");

    Lcg rng(6060ULL);
    for (int trial = 0; trial < 60; ++trial) {
        const int laps = rng.nextInt(1, 80);
        const bool dupes = (trial % 2 == 0);
        boxbox::LapTimeIndex idx(laps);
        std::vector<double> rec;
        for (int lap = 1; lap <= laps; ++lap) {
            const double t = dupes ? 88.0 + static_cast<double>(rng.nextInt(0, 3))
                                   : rng.nextDouble(85.0, 120.0);
            idx.recordLap(lap, t);
            rec.push_back(t);
            for (int q = 0; q < 3; ++q) {  // query the laps recorded so far
                const int first = rng.nextInt(1, lap);
                const int last = rng.nextInt(first, lap);
                const auto b = static_cast<std::size_t>(first - 1);
                const auto e = static_cast<std::size_t>(last);
                TCHECK_NEAR(idx.totalTime(first, last), bruteSum(rec, b, e), 1e-7);
                const std::size_t best = bruteMinIndex(rec, b, e);
                TCHECK(idx.fastestLap(first, last).lap == static_cast<int>(best) + 1);
                TCHECK_NEAR(idx.fastestLap(first, last).seconds, rec[best], 0.0);
            }
        }
    }
}

void testCountOrderSwaps() {
    beginSection("Race laps: countOrderSwaps hand cases, invalid input, vs brute force");

    TCHECK(boxbox::countOrderSwaps({}, {}) == 0);
    TCHECK(boxbox::countOrderSwaps({44}, {44}) == 0);
    TCHECK(boxbox::countOrderSwaps({1, 16, 44, 63, 81}, {1, 16, 44, 63, 81}) == 0);
    TCHECK(boxbox::countOrderSwaps({1, 16, 44, 63, 81}, {81, 63, 44, 16, 1}) == 10);  // 5*4/2
    TCHECK(boxbox::countOrderSwaps({1, 16, 44}, {16, 1, 44}) == 1);
    TCHECK(boxbox::countOrderSwaps({1, 16, 44}, {44, 1, 16}) == 2);  // 44 passed two cars

    TCHECK_THROWS(boxbox::countOrderSwaps({1, 2}, {1}), std::invalid_argument);
    TCHECK_THROWS(boxbox::countOrderSwaps({1, 1}, {1, 2}), std::invalid_argument);
    TCHECK_THROWS(boxbox::countOrderSwaps({1, 2}, {2, 2}), std::invalid_argument);
    TCHECK_THROWS(boxbox::countOrderSwaps({1, 2}, {1, 3}), std::invalid_argument);
    // A foreign car numbered between real ones: the sorted lookup lands on a
    // neighbour, which must be rejected rather than taken as the car.
    TCHECK_THROWS(boxbox::countOrderSwaps({1, 3}, {1, 2}), std::invalid_argument);
    TCHECK_THROWS(boxbox::countOrderSwaps({5, 9}, {7, 5}), std::invalid_argument);
    TCHECK_THROWS(boxbox::countOrderSwaps({10, 20, 30}, {10, 25, 30}), std::invalid_argument);

    // A duplicate in the first order would also trip a later check (lengths
    // match, so a car must then be missing or repeated), so the dedicated
    // check exists for its message. Test that the message names the problem.
    bool namedDuplicate = false;
    try {
        (void)boxbox::countOrderSwaps({4, 4, 8}, {4, 8, 4});
    } catch (const std::invalid_argument& error) {
        namedDuplicate = std::string(error.what()).find("twice in the first order") !=
                         std::string::npos;
    }
    TCHECK(namedDuplicate);

    Lcg rng(4433ULL);
    for (int trial = 0; trial < 200; ++trial) {
        const int d = rng.nextInt(0, 60);
        std::vector<int> before;
        for (int i = 0; i < d; ++i) {
            before.push_back(3 * i + 7);  // car numbers that are not 0 .. D-1
        }
        std::vector<int> after = before;
        for (std::size_t i = after.size(); i > 1U; --i) {  // seeded shuffle
            std::swap(after[i - 1U], after[rng.nextIndex(i)]);
        }
        const std::int64_t swaps = boxbox::countOrderSwaps(before, after);
        TCHECK(swaps == bruteSwaps(before, after));
        TCHECK(swaps == boxbox::countOrderSwaps(after, before));  // symmetric
    }
}

void testRaceLapRecorder() {
    beginSection("Race laps: RaceLapRecorder on 40 seeded races, with and without overtaking");

    Lcg rng(2026ULL);
    std::int64_t swapsSeen = 0;  // the overtaking races must actually reorder cars
    for (int trial = 0; trial < 40; ++trial) {
        const bool overtaking = (trial % 2 == 1);
        const int laps = rng.nextInt(1, 40);
        Track track("Race", laps, rng.nextDouble(0.0, 25.0));
        track.addSector("S1", 30.0);
        track.addSector("S2", 30.0);
        track.addSector("S3", 30.0);
        std::vector<TireCompound> compounds;
        const int compoundCount = rng.nextInt(1, 3);
        for (int c = 0; c < compoundCount; ++c) {
            compounds.push_back(randomCompound(rng));
        }
        const PitStrategy strategy(track, compounds);
        boxbox::RaceSimulator sim(track, compounds);
        if (overtaking) {
            sim.setOvertakeRule(boxbox::OvertakeRule{true, 0.5, 1.0});
        }
        const std::vector<int> numbers{44, 1, 16, 63, 81};
        sim.addDriver(numbers[0], "OPT", strategy.optimize());
        for (std::size_t c = 0; c < compounds.size(); ++c) {
            sim.addDriver(numbers[c + 1U], "ONE", strategy.singleCompoundPlan(c));
        }

        boxbox::RaceLapRecorder recorder(sim);
        TCHECK(recorder.lapsObserved() == 0);
        std::vector<int> grid;
        for (const boxbox::Driver& d : sim.drivers()) {
            grid.push_back(d.number());
        }
        std::sort(grid.begin(), grid.end());
        TCHECK(recorder.runningOrder(0) == grid);  // grid is in car-number order

        while (sim.advanceLap()) {
            recorder.observe(sim);
            TCHECK(recorder.runningOrder(recorder.lapsObserved()) == bruteRunningOrder(sim));
        }
        TCHECK(recorder.lapsObserved() == laps);

        for (std::size_t i = 0; i < sim.drivers().size(); ++i) {
            const boxbox::Driver& d = sim.drivers()[i];
            const boxbox::LapTimeIndex& rec = recorder.carLaps(i);
            TCHECK(rec.recordedLaps() == laps);
            if (laps == 0) {
                continue;
            }
            // Recorded laps plus pit stops add up to the race time (FR-8 style).
            const double rebuilt =
                rec.totalTime(1, laps) + static_cast<double>(d.stopsMade()) * track.pitLoss();
            TCHECK_NEAR(rebuilt, d.cumulativeTime(), boxbox::kClosureTolerance);

            // Without traffic, each lap equals the model, worked out from the plan alone.
            if (!overtaking) {
                int lap = 1;
                for (const boxbox::Stint& stint : d.plan()) {
                    for (int age = 0; age < stint.laps; ++age, ++lap) {
                        const double model =
                            track.baseLapTime() + compounds[stint.compoundIndex].lapDelta(age);
                        TCHECK_NEAR(rec.lapTime(lap), model, 0.0);
                    }
                }
            }

            // Traffic loss (Fenwick-backed) against a plain reference: each lap's
            // traffic is its recorded time minus the model lap worked out from
            // the plan alone.
            std::vector<double> perLap;
            for (const boxbox::Stint& stint : d.plan()) {
                for (int age = 0; age < stint.laps; ++age) {
                    const double model =
                        track.baseLapTime() + compounds[stint.compoundIndex].lapDelta(age);
                    perLap.push_back(rec.lapTime(static_cast<int>(perLap.size()) + 1) - model);
                }
            }
            TCHECK_NEAR(recorder.trafficLoss(i, 1, laps), d.trafficLoss(), 1e-9);
            for (int q = 0; q < 4; ++q) {
                const int first = rng.nextInt(1, laps);
                const int last = rng.nextInt(first, laps);
                double expected = 0.0;
                for (int lap = first; lap <= last; ++lap) {
                    expected += perLap[static_cast<std::size_t>(lap - 1)];
                }
                TCHECK_NEAR(recorder.trafficLoss(i, first, last), expected, 1e-9);
            }
            if (!overtaking) {
                TCHECK_NEAR(recorder.trafficLoss(i, 1, laps), 0.0, 0.0);
            }
        }

        if (laps > 0) {
            double fieldTotal = 0.0;
            for (const boxbox::Driver& d : sim.drivers()) {
                fieldTotal += d.trafficLoss();
            }
            TCHECK_NEAR(recorder.fieldTrafficLoss(1, laps), fieldTotal, 1e-9);
            TCHECK_THROWS(recorder.trafficLoss(0, 0, laps), std::out_of_range);
            TCHECK_THROWS(recorder.trafficLoss(0, 1, laps + 1), std::out_of_range);
            TCHECK_THROWS(recorder.trafficLoss(sim.drivers().size(), 1, 1), std::out_of_range);
            TCHECK_THROWS(recorder.fieldTrafficLoss(2, 1), std::out_of_range);
        }

        if (overtaking) {
            swapsSeen += recorder.swapsBetween(0, laps);
        }
        for (int q = 0; q < 5 && laps > 0; ++q) {
            const int a = rng.nextInt(0, laps);
            const int b = rng.nextInt(0, laps);
            TCHECK(recorder.swapsBetween(a, b) ==
                   bruteSwaps(recorder.runningOrder(a), recorder.runningOrder(b)));
        }
    }
    TCHECK(swapsSeen > 0);
}

void testRaceLapRecorderMisuse() {
    beginSection("Race laps: RaceLapRecorder rejects misuse");

    Track track("Race", 5, 20.0);
    track.addSector("S1", 90.0);
    const std::vector<TireCompound> compounds{TireCompound("M", 0.0, 0.1, 5)};
    const PitStrategy strategy(track, compounds);
    boxbox::RaceSimulator sim(track, compounds);
    sim.addDriver(7, "A", strategy.optimize());
    sim.addDriver(9, "B", strategy.optimize());

    boxbox::RaceLapRecorder recorder(sim);
    sim.advanceLap();
    sim.advanceLap();  // observe() skipped once
    TCHECK_THROWS(recorder.observe(sim), std::invalid_argument);
    TCHECK(recorder.lapsObserved() == 0);  // the rejected call recorded nothing
    TCHECK(recorder.carLaps(0).recordedLaps() == 0);

    TCHECK_THROWS(boxbox::RaceLapRecorder{sim}, std::invalid_argument);  // race already started
    TCHECK_THROWS(recorder.carLaps(2), std::out_of_range);
    TCHECK_THROWS(recorder.runningOrder(1), std::out_of_range);
    TCHECK_THROWS(recorder.runningOrder(-1), std::out_of_range);

    sim.addDriver(11, "C", strategy.optimize());  // field grows
    sim.reset();
    boxbox::RaceLapRecorder fresh(sim);
    TCHECK(fresh.carCount() == 3U);
    sim.advanceLap();
    fresh.observe(sim);
    TCHECK(fresh.lapsObserved() == 1);
    TCHECK(fresh.swapsBetween(0, 1) == 0);  // identical cars stay in grid order

    // A repeated observe() with no lap run between is harmless: it must not
    // push a second running order and put lapsObserved() ahead of the laps.
    fresh.observe(sim);
    TCHECK(fresh.lapsObserved() == 1);
    TCHECK(fresh.carLaps(0).recordedLaps() == 1);
    TCHECK_NEAR(fresh.carLaps(0).totalTime(1, fresh.lapsObserved()),
                fresh.carLaps(0).lapTime(1), 0.0);  // the report's window still works
    sim.advanceLap();
    fresh.observe(sim);
    TCHECK(fresh.lapsObserved() == 2);
}

}  // namespace

TireTestTally runTireModuleTests() {
    g_tally = TireTestTally{};

    testSegmentTreeBasics();
    testSegmentTreeAgainstBruteForce();
    testSegmentTreeUpdateLocality();
    testFenwickBasics();
    testFenwickAgainstBruteForce();
    testProfileHandWorked();
    testCliffBoundaries();
    testProfileAgainstSourcesOfTruth();
    testProfileFromLapTimes();
    testRangeAnalyzer();
    testRangeAnalyzerAgainstBruteForce();
    testRangeAnalyzerMatchesPrefixTable();
    testOverridesStayWhatIf();
    testThresholdHandWorked();
    testThresholdScanPath();
    testThresholdAgainstBruteForce();
    testThresholdZeroWearEdges();
    testCompareCompoundsCases();
    testCompareCompoundsAgainstBruteForce();
    testFromCompoundMatchesFromStrategy();
    testDeterminism();
    testLapTimeIndexBasics();
    testLapTimeIndexAgainstBruteForce();
    testCountOrderSwaps();
    testRaceLapRecorder();
    testRaceLapRecorderMisuse();

    return g_tally;
}
