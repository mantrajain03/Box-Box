// Box Box: checks for driver analytics (plan sec. 4.3 and 8.4, owner Mantra).
//
// Covers LapRecorder, DriverAnalytics, MonotonicDeque, selectLaps,
// trailingWindows, summarizeWindows and rankDrivers. Every optimized query is
// checked against a plain brute-force reference on seeded data, alongside a
// race small enough to work out by hand. Same harness style as test_main.cpp:
// a failing check is recorded and the run carries on.
//
// The CLI report (cli/DriverReport.cpp) is not linked here, because make test
// builds against the core objects only (PRD 7). It is exercised through
// bin/boxbox.

#include "DriverAnalyticsTests.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "boxbox/Common.hpp"
#include "boxbox/Driver.hpp"
#include "boxbox/DriverAnalytics.hpp"
#include "boxbox/LapRecord.hpp"
#include "boxbox/Leaderboard.hpp"
#include "boxbox/MonotonicDeque.hpp"
#include "boxbox/PitStrategy.hpp"
#include "boxbox/RaceLapAnalytics.hpp"
#include "boxbox/RaceSimulator.hpp"
#include "boxbox/TireCompound.hpp"
#include "boxbox/Track.hpp"

namespace {

using boxbox::CompoundUsage;
using boxbox::definitelyLess;
using boxbox::Driver;
using boxbox::DriverAnalytics;
using boxbox::DriverSummary;
using boxbox::kClosureTolerance;
using boxbox::kTimeEpsilon;
using boxbox::LapFilter;
using boxbox::LapOrder;
using boxbox::LapRecord;
using boxbox::LapRecorder;
using boxbox::LapWindow;
using boxbox::MonotonicDeque;
using boxbox::PitStrategy;
using boxbox::PlannedStint;
using boxbox::RaceSimulator;
using boxbox::RankMetric;
using boxbox::Stint;
using boxbox::StintSummary;
using boxbox::TireCompound;
using boxbox::Track;
using boxbox::WindowSummary;

// ---------------------------------------------------------------------------
// Harness
// ---------------------------------------------------------------------------

DriverTestTally g_tally;
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

#define DCHECK(expr) reportCheck((expr), #expr, __FILE__, __LINE__, std::string())

#define DCHECK_MSG(expr, detail) reportCheck((expr), #expr, __FILE__, __LINE__, (detail))

#define DCHECK_NEAR(actual, expected, tolerance)                                        \
    reportCheck(std::fabs((actual) - (expected)) <= (tolerance),                        \
                #actual " ~= " #expected, __FILE__, __LINE__,                           \
                describe((actual), (expected), (tolerance)))

#define DCHECK_THROWS(statement, ExType)                                                \
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
// Fixtures and brute-force references
// ---------------------------------------------------------------------------

/// A record with only the fields a test cares about set. The rest keep their
/// defaults: no traffic, no stop, compound 0, age 0, stint 0, P1.
LapRecord lapOf(int car, int lap, double seconds) {
    LapRecord r;
    r.driverNumber = car;
    r.lap = lap;
    r.lapTime = seconds;
    r.position = 1;
    return r;
}

/// The hand-worked race. One 10 s sector, 4 laps, 5 s pit loss.
///   compound A: +0 s, 1 s/lap wear, stints of up to 2 laps
///   compound B: +2 s, no wear, stints of up to 4 laps
///   car 7 runs A for 2 laps, stops, then B for 2:  10  11 | 12  12   (+5 s)
///   car 9 runs B for all 4 laps:                   12  12   12  12
/// Race time after each lap - car 7: 10 26 38 50, car 9: 12 24 36 48 - so
/// car 7 leads after lap 1 and car 9 from lap 2 on.
RaceSimulator handRace() {
    Track track("Hand", 4, 5.0);
    track.addSector("S1", 10.0);
    const std::vector<TireCompound> compounds{TireCompound("A", 0.0, 1.0, 2),
                                              TireCompound("B", 2.0, 0.0, 4)};
    const PitStrategy strategy(track, compounds);
    RaceSimulator sim(track, compounds);
    sim.addDriver(7, "SEVEN", strategy.evaluatePlan({PlannedStint{0, 2}, PlannedStint{1, 2}}));
    sim.addDriver(9, "NINE", strategy.evaluatePlan({PlannedStint{1, 4}}));
    return sim;
}

/// Run `sim` from the grid to the flag with a LapRecorder watching.
LapRecorder recordRace(RaceSimulator& sim) {
    LapRecorder recorder(sim);
    while (sim.advanceLap()) {
        recorder.observe(sim);
    }
    return recorder;
}

TireCompound randomCompound(Lcg& rng) {
    const int maxStint = rng.nextInt(1, 40);
    const int cliffLap = (rng.nextInt(0, 3) == 0) ? -1 : rng.nextInt(0, 40);
    const double wear = (rng.nextInt(0, 4) == 0) ? 0.0 : rng.nextDouble(0.0, 0.3);
    const double cliffRate = (rng.nextInt(0, 4) == 0) ? 0.0 : rng.nextDouble(0.0, 0.9);
    return TireCompound("r", rng.nextDouble(-1.0, 1.0), wear, maxStint, cliffLap, cliffRate);
}

/// Running order by sorting with the Leaderboard's own rule: a reference
/// that uses no heap.
std::vector<int> bruteRunningOrder(const RaceSimulator& sim) {
    std::vector<boxbox::LeaderboardEntry> entries = sim.snapshotEntries();
    std::sort(entries.begin(), entries.end(), boxbox::Leaderboard::isAhead);
    std::vector<int> order;
    for (const boxbox::LeaderboardEntry& e : entries) {
        order.push_back(e.driverNumber);
    }
    return order;
}

/// A field of synthetic lap records for the analytics alone: up to six cars
/// (numbers 5, 8, 11, ...), each with its own lap count, lap times with many
/// exact ties when `ties` is set, some traffic, stops (one car in three with
/// a zero pit loss), stints, compounds and positions. Each car's records are
/// in lap order, and the cars are interleaved at random.
struct SyntheticField {
    std::vector<int> field;
    std::vector<LapRecord> records;
};

SyntheticField syntheticField(Lcg& rng, bool ties) {
    SyntheticField out;
    const int cars = rng.nextInt(1, 6);
    std::vector<std::vector<LapRecord>> perCar(static_cast<std::size_t>(cars));
    std::size_t total = 0;

    for (int c = 0; c < cars; ++c) {
        const int number = 5 + 3 * c;
        out.field.push_back(number);
        const double pitLoss = (c % 3 == 2) ? 0.0 : 21.5;
        const int laps = rng.nextInt(0, 30);
        int stint = 0;
        int age = 0;
        std::size_t compound = rng.nextIndex(3);
        for (int lap = 1; lap <= laps; ++lap) {
            LapRecord r;
            r.driverNumber = number;
            r.lap = lap;
            r.lapTime = ties ? 88.0 + static_cast<double>(rng.nextInt(0, 3))
                             : rng.nextDouble(85.0, 99.0);
            r.trafficTime = (rng.nextInt(0, 3) == 0) ? rng.nextDouble(0.0, 1.5) : 0.0;
            r.pitStop = (rng.nextInt(0, 5) == 0);
            r.pitTime = r.pitStop ? pitLoss : 0.0;
            r.compoundIndex = compound;
            r.tireAge = age;
            r.stint = stint;
            r.position = rng.nextInt(1, cars);
            perCar[static_cast<std::size_t>(c)].push_back(r);
            ++age;
            if (r.pitStop) {
                ++stint;
                age = 0;
                compound = rng.nextIndex(3);
            }
        }
        total += static_cast<std::size_t>(laps);
    }

    // Interleave: take the next lap of a randomly chosen car that has one left.
    std::vector<std::size_t> next(perCar.size(), 0);
    while (out.records.size() < total) {
        const std::size_t c = rng.nextIndex(perCar.size());
        if (next[c] < perCar[c].size()) {
            out.records.push_back(perCar[c][next[c]]);
            ++next[c];
        }
    }
    return out;
}

/// Every DriverSummary definition, worked out the plain way for one car:
/// filter the records, then walk them. No hash map and no shared code.
void checkAgainstDirect(const DriverSummary& got, const std::vector<LapRecord>& records, int car,
                        const std::vector<double>& lapTimes) {
    std::vector<LapRecord> mine;
    for (const LapRecord& r : records) {
        if (r.driverNumber == car) {
            mine.push_back(r);
        }
    }
    const std::size_t n = mine.size();
    DCHECK(got.driverNumber == car);
    DCHECK(got.laps == static_cast<int>(n));
    DCHECK(lapTimes.size() == n);

    double green = 0.0;
    double pit = 0.0;
    double traffic = 0.0;
    double total = 0.0;
    int stops = 0;
    for (const LapRecord& r : mine) {
        green += r.lapTime;
        pit += r.pitTime;
        traffic += r.trafficTime;
        total += r.lapTime;
        total += r.pitTime;
        stops += r.pitStop ? 1 : 0;
    }
    DCHECK_NEAR(got.greenTime, green, kTimeEpsilon);
    DCHECK_NEAR(got.pitTime, pit, kTimeEpsilon);
    DCHECK_NEAR(got.trafficTime, traffic, kTimeEpsilon);
    DCHECK_NEAR(got.totalTime, total, kTimeEpsilon);
    DCHECK(got.stops == stops);

    if (n == 0U) {
        DCHECK(got.bestLap.lap == 0 && got.worstLap.lap == 0 && got.stints.empty() &&
               got.compounds.empty() && got.positions.empty() && got.bestPosition == 0 &&
               got.worstPosition == 0);
        return;
    }

    // Fastest and slowest: a plain scan, the earliest lap on ties.
    std::size_t best = 0;
    std::size_t worst = 0;
    for (std::size_t i = 1; i < n; ++i) {
        if (mine[i].lapTime < mine[best].lapTime) {
            best = i;
        }
        if (mine[worst].lapTime < mine[i].lapTime) {
            worst = i;
        }
    }
    DCHECK(got.bestLap.lap == mine[best].lap && got.bestLap.driverNumber == car);
    DCHECK(got.worstLap.lap == mine[worst].lap && got.worstLap.driverNumber == car);
    const double average = green / static_cast<double>(n);
    DCHECK_NEAR(got.averageLap, average, kTimeEpsilon);
    DCHECK_NEAR(got.lapRange, mine[worst].lapTime - mine[best].lapTime, 0.0);
    double deviation = 0.0;
    for (const LapRecord& r : mine) {
        deviation += std::fabs(r.lapTime - average);
    }
    DCHECK_NEAR(got.meanAbsDeviation, deviation / static_cast<double>(n), kTimeEpsilon);

    bool timesOk = true;
    for (std::size_t i = 0; i < n && i < lapTimes.size(); ++i) {
        timesOk = timesOk && std::fabs(lapTimes[i] - mine[i].lapTime) <= 0.0;
    }
    DCHECK(timesOk);

    // Stints: a new one wherever the stint number changes.
    std::vector<StintSummary> stints;
    for (const LapRecord& r : mine) {
        if (stints.empty() || stints.back().stint != r.stint) {
            StintSummary s;
            s.stint = r.stint;
            s.compoundIndex = r.compoundIndex;
            s.firstLap = r.lap;
            stints.push_back(s);
        }
        stints.back().lastLap = r.lap;
        ++stints.back().laps;
        stints.back().time += r.lapTime;
    }
    bool stintsOk = got.stints.size() == stints.size();
    for (std::size_t i = 0; stintsOk && i < stints.size(); ++i) {
        const StintSummary& a = got.stints[i];
        const StintSummary& b = stints[i];
        stintsOk = a.stint == b.stint && a.compoundIndex == b.compoundIndex &&
                   a.firstLap == b.firstLap && a.lastLap == b.lastLap && a.laps == b.laps &&
                   std::fabs(a.time - b.time) <= kTimeEpsilon;
    }
    DCHECK(stintsOk);

    // Compound usage, counted lap by lap rather than from the stints.
    std::size_t highest = 0;
    for (const LapRecord& r : mine) {
        highest = std::max(highest, r.compoundIndex);
    }
    std::vector<int> perCompound(highest + 1U, 0);
    for (const LapRecord& r : mine) {
        ++perCompound[r.compoundIndex];
    }
    std::vector<CompoundUsage> usage;
    for (std::size_t c = 0; c < perCompound.size(); ++c) {
        if (perCompound[c] > 0) {
            usage.push_back(CompoundUsage{c, perCompound[c]});
        }
    }
    bool usageOk = got.compounds.size() == usage.size();
    for (std::size_t i = 0; usageOk && i < usage.size(); ++i) {
        usageOk = got.compounds[i].compoundIndex == usage[i].compoundIndex &&
                  got.compounds[i].laps == usage[i].laps;
    }
    DCHECK(usageOk);

    std::vector<int> positions;
    for (const LapRecord& r : mine) {
        positions.push_back(r.position);
    }
    DCHECK(got.positions == positions);
    DCHECK(got.bestPosition == *std::min_element(positions.begin(), positions.end()));
    DCHECK(got.worstPosition == *std::max_element(positions.begin(), positions.end()));
}

/// selectLaps' order, stated a second way for the reference sort.
bool referenceAhead(const LapRecord& x, const LapRecord& y, LapOrder order) {
    if (x.lapTime < y.lapTime) {
        return order == LapOrder::Fastest;
    }
    if (y.lapTime < x.lapTime) {
        return order == LapOrder::Slowest;
    }
    if (x.lap != y.lap) {
        return x.lap < y.lap;
    }
    return x.driverNumber < y.driverNumber;
}

bool referencePasses(const LapRecord& r, const LapFilter& f) {
    const bool car = !f.driverNumber.has_value() || r.driverNumber == f.driverNumber.value();
    const bool compound =
        !f.compoundIndex.has_value() || r.compoundIndex == f.compoundIndex.value();
    return car && compound && f.firstLap <= r.lap && r.lap <= f.lastLap;
}

/// rankDrivers' documented rule for one pair: true when `a` must come first.
bool documentedBefore(const DriverSummary& a, const DriverSummary& b, RankMetric metric) {
    if ((a.laps > 0) != (b.laps > 0)) {
        return a.laps > 0;
    }
    if (a.laps > 0) {
        const double va = boxbox::rankValue(a, metric);
        const double vb = boxbox::rankValue(b, metric);
        if (va < vb) {
            return true;
        }
        if (vb < va) {
            return false;
        }
    }
    return a.driverNumber < b.driverNumber;
}

DriverSummary rankable(int car, int laps, double average, double best, double range,
                       double deviation) {
    DriverSummary s;
    s.driverNumber = car;
    s.laps = laps;
    s.averageLap = average;
    s.bestLap.lapTime = best;
    s.bestLap.lap = (laps > 0) ? 1 : 0;
    s.lapRange = range;
    s.meanAbsDeviation = deviation;
    return s;
}

/// True when DriverAnalytics refuses these records with std::invalid_argument.
bool rejected(const std::vector<int>& field, const std::vector<LapRecord>& records) {
    try {
        const DriverAnalytics analytics(field, records);
        (void)analytics;
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// MonotonicDeque
// ---------------------------------------------------------------------------

void testMonotonicDequeHandCases() {
    beginSection("MonotonicDeque: hand cases, ties, window edges and invalid input");

    DCHECK_THROWS(MonotonicDeque(0, MonotonicDeque::Kind::Minimum), std::invalid_argument);

    // 3 1 2 1 5 through a 2-value window.
    //   minimum 3 1 1 1 1 at 0 1 1 3 3 - the second 1 takes over once the
    //   first has left the window
    //   maximum 3 3 2 2 5 at 0 0 2 2 4
    const std::vector<double> values{3.0, 1.0, 2.0, 1.0, 5.0};
    const std::vector<double> minValue{3.0, 1.0, 1.0, 1.0, 1.0};
    const std::vector<std::size_t> minIndex{0, 1, 1, 3, 3};
    const std::vector<double> maxValue{3.0, 3.0, 2.0, 2.0, 5.0};
    const std::vector<std::size_t> maxIndex{0, 0, 2, 2, 4};

    MonotonicDeque low(2, MonotonicDeque::Kind::Minimum);
    MonotonicDeque high(2, MonotonicDeque::Kind::Maximum);
    DCHECK(low.empty() && low.pushed() == 0U && low.window() == 2U);
    DCHECK(low.kind() == MonotonicDeque::Kind::Minimum);
    DCHECK_THROWS(low.extreme(), std::out_of_range);
    DCHECK_THROWS(low.extremeIndex(), std::out_of_range);
    for (std::size_t i = 0; i < values.size(); ++i) {
        low.push(values[i]);
        high.push(values[i]);
        DCHECK_NEAR(low.extreme(), minValue[i], 0.0);
        DCHECK(low.extremeIndex() == minIndex[i]);
        DCHECK_NEAR(high.extreme(), maxValue[i], 0.0);
        DCHECK(high.extremeIndex() == maxIndex[i]);
        DCHECK(low.size() <= 2U && high.size() <= 2U);
    }
    DCHECK(low.pushed() == 5U);

    // Equal values all stay, and the earliest of them answers.
    MonotonicDeque flat(3, MonotonicDeque::Kind::Minimum);
    flat.push(7.0);
    flat.push(7.0);
    flat.push(7.0);
    DCHECK(flat.extremeIndex() == 0U && flat.size() == 3U);
    flat.push(7.0);  // index 0 leaves; index 1 is now the earliest
    DCHECK(flat.extremeIndex() == 1U && flat.size() == 3U);

    // W = 1: each value on its own.
    MonotonicDeque one(1, MonotonicDeque::Kind::Maximum);
    one.push(4.0);
    one.push(9.0);
    one.push(2.0);
    DCHECK(one.extremeIndex() == 2U && one.size() == 1U);
    DCHECK_NEAR(one.extreme(), 2.0, 0.0);

    // A window longer than the stream: the running minimum since the start.
    MonotonicDeque wide(100, MonotonicDeque::Kind::Minimum);
    for (const double v : {5.0, 3.0, 4.0, 3.0, 6.0}) {
        wide.push(v);
    }
    DCHECK(wide.extremeIndex() == 1U);

    // A rejected push changes nothing.
    DCHECK_THROWS(wide.push(std::nan("")), std::invalid_argument);
    DCHECK_THROWS(wide.push(std::numeric_limits<double>::infinity()), std::invalid_argument);
    DCHECK(wide.pushed() == 5U && wide.extremeIndex() == 1U);

    wide.clear();
    DCHECK(wide.empty() && wide.pushed() == 0U && wide.window() == 100U);
    DCHECK_THROWS(wide.extreme(), std::out_of_range);
    wide.push(8.0);
    DCHECK(wide.extremeIndex() == 0U);
}

void testMonotonicDequeAgainstBruteForce() {
    beginSection("MonotonicDeque vs brute-force window scans, 150 seeded streams");

    Lcg rng(3131ULL);
    for (int trial = 0; trial < 150; ++trial) {
        const std::size_t n = rng.nextIndex(70);
        const bool ties = (trial % 2 == 0);
        std::vector<double> v(n);
        for (double& x : v) {
            x = ties ? static_cast<double>(rng.nextInt(0, 4)) : rng.nextDouble(80.0, 100.0);
        }
        const std::size_t w = 1U + rng.nextIndex(n + 3U);  // 1 .. n + 3, past the end too

        for (const MonotonicDeque::Kind kind :
             {MonotonicDeque::Kind::Minimum, MonotonicDeque::Kind::Maximum}) {
            MonotonicDeque dq(w, kind);
            bool ok = true;
            for (std::size_t i = 0; i < n; ++i) {
                dq.push(v[i]);
                const std::size_t first = (i + 1U > w) ? i + 1U - w : 0U;
                std::size_t pick = first;
                for (std::size_t j = first + 1U; j <= i; ++j) {
                    const bool better = (kind == MonotonicDeque::Kind::Minimum) ? v[j] < v[pick]
                                                                                : v[pick] < v[j];
                    if (better) {
                        pick = j;
                    }
                }
                ok = ok && dq.extremeIndex() == pick && std::fabs(dq.extreme() - v[pick]) <= 0.0 &&
                     dq.size() <= std::min(w, i + 1U) && dq.pushed() == i + 1U;
            }
            DCHECK(ok);
        }
    }

    // Every window size over one tie-heavy stream, including W = 1, W = n and W > n.
    const std::vector<double> stream{2, 0, 2, 1, 0, 0, 3, 1, 1, 2, 0, 3};
    for (std::size_t w = 1; w <= stream.size() + 2U; ++w) {
        MonotonicDeque lowest(w, MonotonicDeque::Kind::Minimum);
        MonotonicDeque highest(w, MonotonicDeque::Kind::Maximum);
        bool ok = true;
        for (std::size_t i = 0; i < stream.size(); ++i) {
            lowest.push(stream[i]);
            highest.push(stream[i]);
            const std::size_t first = (i + 1U > w) ? i + 1U - w : 0U;
            std::size_t lo = first;
            std::size_t hi = first;
            for (std::size_t j = first + 1U; j <= i; ++j) {
                lo = (stream[j] < stream[lo]) ? j : lo;
                hi = (stream[hi] < stream[j]) ? j : hi;
            }
            ok = ok && lowest.extremeIndex() == lo && highest.extremeIndex() == hi;
        }
        DCHECK(ok);
    }
}

// ---------------------------------------------------------------------------
// The hand-worked race
// ---------------------------------------------------------------------------

void testHandRaceRecords() {
    beginSection("Hand race: every LapRecord field, worked out by hand");

    RaceSimulator sim = handRace();
    const LapRecorder recorder = recordRace(sim);
    DCHECK((recorder.field() == std::vector<int>{7, 9}));
    DCHECK((recorder.names() == std::vector<std::string>{"SEVEN", "NINE"}));

    // Lap by lap, car 7 before car 9 within a lap (grid order).
    struct Expected {
        int car;
        int lap;
        double time;
        bool stop;
        double pit;
        std::size_t compound;
        int age;
        int stint;
        int position;
    };
    const std::vector<Expected> expected{
        {7, 1, 10.0, false, 0.0, 0, 0, 0, 1}, {9, 1, 12.0, false, 0.0, 1, 0, 0, 2},
        {7, 2, 11.0, true, 5.0, 0, 1, 0, 2},  {9, 2, 12.0, false, 0.0, 1, 1, 0, 1},
        {7, 3, 12.0, false, 0.0, 1, 0, 1, 2}, {9, 3, 12.0, false, 0.0, 1, 2, 0, 1},
        {7, 4, 12.0, false, 0.0, 1, 1, 1, 2}, {9, 4, 12.0, false, 0.0, 1, 3, 0, 1},
    };
    const std::vector<LapRecord>& rec = recorder.records();
    DCHECK(rec.size() == expected.size());
    for (std::size_t i = 0; i < expected.size() && i < rec.size(); ++i) {
        const LapRecord& r = rec[i];
        const Expected& e = expected[i];
        DCHECK(r.driverNumber == e.car && r.lap == e.lap);
        DCHECK_NEAR(r.lapTime, e.time, 0.0);
        DCHECK_NEAR(r.trafficTime, 0.0, 0.0);
        DCHECK(r.pitStop == e.stop);
        DCHECK_NEAR(r.pitTime, e.pit, 0.0);
        DCHECK(r.compoundIndex == e.compound && r.tireAge == e.age && r.stint == e.stint);
        DCHECK(r.position == e.position);
    }
}

void testHandRaceAnalytics() {
    beginSection("Hand race: summaries, top laps, rankings and windows, by hand");

    RaceSimulator sim = handRace();
    const LapRecorder recorder = recordRace(sim);
    const DriverAnalytics analytics(recorder);
    const PitStrategy strategy(sim.track(), sim.compounds());

    const DriverSummary& seven = analytics.summary(7);
    DCHECK(seven.name == "SEVEN" && seven.laps == 4 && seven.stops == 1);
    DCHECK_NEAR(seven.totalTime, 50.0, 0.0);
    DCHECK_NEAR(seven.totalTime, sim.drivers()[0].cumulativeTime(), 0.0);
    DCHECK_NEAR(seven.greenTime, 45.0, 0.0);
    DCHECK_NEAR(seven.pitTime, 5.0, 0.0);
    DCHECK_NEAR(seven.trafficTime, 0.0, 0.0);
    DCHECK_NEAR(seven.averageLap, 11.25, 0.0);
    DCHECK(seven.bestLap.lap == 1);
    DCHECK_NEAR(seven.bestLap.lapTime, 10.0, 0.0);
    DCHECK(seven.worstLap.lap == 3);  // laps 3 and 4 tie at 12 s: the earlier wins
    DCHECK_NEAR(seven.lapRange, 2.0, 0.0);
    // |10 - 11.25| + |11 - 11.25| + 2 * |12 - 11.25| = 1.25 + 0.25 + 1.5 = 3, over 4 laps
    DCHECK_NEAR(seven.meanAbsDeviation, 0.75, 0.0);
    DCHECK(seven.stints.size() == 2U);
    if (seven.stints.size() == 2U) {
        const StintSummary& a = seven.stints[0];
        const StintSummary& b = seven.stints[1];
        DCHECK(a.stint == 0 && a.compoundIndex == 0U && a.firstLap == 1 && a.lastLap == 2 &&
               a.laps == 2);
        DCHECK(b.stint == 1 && b.compoundIndex == 1U && b.firstLap == 3 && b.lastLap == 4 &&
               b.laps == 2);
        DCHECK_NEAR(a.time, 21.0, 0.0);
        DCHECK_NEAR(b.time, 24.0, 0.0);
        // Each stint's time is what the optimizer's prefix table says it costs.
        DCHECK_NEAR(a.time, strategy.stintCost(0, 2), kClosureTolerance);
        DCHECK_NEAR(b.time, strategy.stintCost(1, 2), kClosureTolerance);
    }
    DCHECK(seven.compounds.size() == 2U && seven.compounds[0].compoundIndex == 0U &&
           seven.compounds[0].laps == 2 && seven.compounds[1].compoundIndex == 1U &&
           seven.compounds[1].laps == 2);
    DCHECK((seven.positions == std::vector<int>{1, 2, 2, 2}));
    DCHECK(seven.bestPosition == 1 && seven.worstPosition == 2);

    const DriverSummary& nine = analytics.summary(9);
    DCHECK(nine.name == "NINE" && nine.laps == 4 && nine.stops == 0);
    DCHECK_NEAR(nine.totalTime, 48.0, 0.0);
    DCHECK_NEAR(nine.greenTime, 48.0, 0.0);
    DCHECK_NEAR(nine.pitTime, 0.0, 0.0);
    DCHECK_NEAR(nine.averageLap, 12.0, 0.0);
    DCHECK(nine.bestLap.lap == 1 && nine.worstLap.lap == 1);  // four equal laps
    DCHECK_NEAR(nine.lapRange, 0.0, 0.0);
    DCHECK_NEAR(nine.meanAbsDeviation, 0.0, 0.0);
    DCHECK(nine.stints.size() == 1U && nine.stints[0].compoundIndex == 1U &&
           nine.stints[0].laps == 4);
    DCHECK(nine.compounds.size() == 1U && nine.compounds[0].laps == 4);
    DCHECK((nine.positions == std::vector<int>{2, 1, 1, 1}));
    DCHECK(nine.bestPosition == 1 && nine.worstPosition == 2);

    // Fastest three: 10 and 11 (car 7), then the first 12 s lap, car 9's lap 1.
    const std::vector<LapRecord>& all = analytics.records();
    const std::vector<LapRecord> fastest = boxbox::selectLaps(all, 3, LapOrder::Fastest);
    DCHECK(fastest.size() == 3U);
    if (fastest.size() == 3U) {
        DCHECK(fastest[0].driverNumber == 7 && fastest[0].lap == 1);
        DCHECK(fastest[1].driverNumber == 7 && fastest[1].lap == 2);
        DCHECK(fastest[2].driverNumber == 9 && fastest[2].lap == 1);
    }
    // Slowest four: six laps tie at 12 s, so lap number, then car number decide.
    const std::vector<LapRecord> slowest = boxbox::selectLaps(all, 4, LapOrder::Slowest);
    DCHECK(slowest.size() == 4U);
    if (slowest.size() == 4U) {
        DCHECK(slowest[0].driverNumber == 9 && slowest[0].lap == 1);
        DCHECK(slowest[1].driverNumber == 9 && slowest[1].lap == 2);
        DCHECK(slowest[2].driverNumber == 7 && slowest[2].lap == 3);
        DCHECK(slowest[3].driverNumber == 9 && slowest[3].lap == 3);
    }
    LapFilter onB;
    onB.compoundIndex = 1U;
    const std::vector<LapRecord> bestOnB = boxbox::selectLaps(all, 1, LapOrder::Fastest, onB);
    DCHECK(bestOnB.size() == 1U && bestOnB[0].driverNumber == 9 && bestOnB[0].lap == 1);
    LapFilter sevenOnB = onB;
    sevenOnB.driverNumber = 7;
    const std::vector<LapRecord> sevensB = boxbox::selectLaps(all, 1, LapOrder::Fastest, sevenOnB);
    DCHECK(sevensB.size() == 1U && sevensB[0].lap == 3);
    LapFilter lateLaps;
    lateLaps.firstLap = 3;
    lateLaps.lastLap = 4;
    const std::vector<LapRecord> late = boxbox::selectLaps(all, 1, LapOrder::Fastest, lateLaps);
    DCHECK(late.size() == 1U && late[0].driverNumber == 7 && late[0].lap == 3);
    LapFilter nobody;
    nobody.driverNumber = 99;
    DCHECK(boxbox::selectLaps(all, 5, LapOrder::Fastest, nobody).empty());
    DCHECK(boxbox::selectLaps(all, 0, LapOrder::Fastest).empty());
    DCHECK(boxbox::selectLaps(all, 50, LapOrder::Slowest).size() == all.size());

    const std::vector<DriverSummary>& both = analytics.summaries();
    DCHECK((boxbox::rankDrivers(both, RankMetric::AverageLap) == std::vector<int>{7, 9}));
    DCHECK((boxbox::rankDrivers(both, RankMetric::BestLap) == std::vector<int>{7, 9}));
    DCHECK((boxbox::rankDrivers(both, RankMetric::LapRange) == std::vector<int>{9, 7}));
    DCHECK((boxbox::rankDrivers(both, RankMetric::MeanAbsDeviation) == std::vector<int>{9, 7}));

    // Car 7's 2-lap windows: [10] [10 11] [11 12] [12 12].
    const std::vector<LapWindow> w = boxbox::trailingWindows(analytics.lapTimes(7), 2);
    DCHECK(w.size() == 4U);
    if (w.size() == 4U) {
        DCHECK(w[0].firstLap == 1 && w[0].lastLap == 1 && w[0].laps == 1 &&
               w[0].fastestLap == 1 && w[0].slowestLap == 1);
        DCHECK(w[1].firstLap == 1 && w[1].lastLap == 2 && w[1].laps == 2 &&
               w[1].fastestLap == 1 && w[1].slowestLap == 2);
        DCHECK(w[2].firstLap == 2 && w[2].lastLap == 3 && w[2].fastestLap == 2 &&
               w[2].slowestLap == 3);
        DCHECK(w[3].firstLap == 3 && w[3].lastLap == 4 && w[3].fastestLap == 3 &&
               w[3].slowestLap == 3);  // 12 and 12: the earlier lap answers both
        DCHECK_NEAR(w[0].average, 10.0, 0.0);
        DCHECK_NEAR(w[1].average, 10.5, 0.0);
        DCHECK_NEAR(w[2].average, 11.5, 0.0);
        DCHECK_NEAR(w[3].average, 12.0, 0.0);
        DCHECK_NEAR(w[1].spread(), 1.0, 0.0);
        DCHECK_NEAR(w[3].spread(), 0.0, 0.0);
    }
    const WindowSummary two = boxbox::summarizeWindows(analytics.lapTimes(7), 2);
    DCHECK(two.complete && two.window == 2U);
    DCHECK(two.quickest.firstLap == 1 && two.quickest.lastLap == 2);  // 10.5
    DCHECK(two.slowest.firstLap == 3 && two.slowest.lastLap == 4);    // 12.0
    DCHECK_NEAR(two.dropOff, 1.5, 0.0);
    DCHECK(two.widest.lastLap == 2);  // spreads 1, 1, 0: the first 1 wins
    DCHECK_NEAR(two.widest.spread(), 1.0, 0.0);

    // A window longer than the race: the whole race is the one window.
    const WindowSummary five = boxbox::summarizeWindows(analytics.lapTimes(7), 5);
    DCHECK(!five.complete);
    DCHECK(five.quickest.firstLap == 1 && five.quickest.lastLap == 4);
    DCHECK_NEAR(five.quickest.average, 11.25, 0.0);
    DCHECK_NEAR(five.widest.spread(), 2.0, 0.0);
    DCHECK_NEAR(five.dropOff, 0.0, 0.0);
}

// ---------------------------------------------------------------------------
// LapRecorder on real races
// ---------------------------------------------------------------------------

void testRecorderOnSeededRaces() {
    beginSection("LapRecorder + DriverAnalytics on 40 seeded races, with and without overtaking");

    Lcg rng(8484ULL);
    bool sawTraffic = false;
    bool sawStop = false;
    bool sawFreeStop = false;  // a stop with a zero pit loss must still count
    for (int trial = 0; trial < 40; ++trial) {
        const bool overtaking = (trial % 2 == 1);
        const int laps = rng.nextInt(0, 45);
        const double pitLoss = (trial % 5 == 0) ? 0.0 : rng.nextDouble(5.0, 25.0);
        Track track("Race", laps, pitLoss);
        track.addSector("S1", 30.0);
        track.addSector("S2", 30.0);
        track.addSector("S3", 30.0);
        std::vector<TireCompound> compounds;
        const int compoundCount = rng.nextInt(1, 3);
        for (int c = 0; c < compoundCount; ++c) {
            compounds.push_back(randomCompound(rng));
        }
        const PitStrategy strategy(track, compounds);
        RaceSimulator sim(track, compounds);
        if (overtaking) {
            sim.setOvertakeRule(boxbox::OvertakeRule{true, 0.5, 1.0});
        }
        const std::vector<int> numbers{44, 1, 16, 63};
        sim.addDriver(numbers[0], "OPT", strategy.optimize());
        for (std::size_t c = 0; c < compounds.size(); ++c) {
            sim.addDriver(numbers[c + 1U], "ONE", strategy.singleCompoundPlan(c));
        }
        const std::size_t cars = sim.drivers().size();

        LapRecorder recorder(sim);
        boxbox::RaceLapRecorder tireModule(sim);  // the tire module's observer, as a cross-check
        while (sim.advanceLap()) {
            const std::size_t before = recorder.records().size();
            recorder.observe(sim);
            tireModule.observe(sim);
            DCHECK(recorder.records().size() == before + cars);

            // This lap's records: one per car, in grid order, positions as a sort says.
            const std::vector<int> order = bruteRunningOrder(sim);
            bool lapOk = true;
            for (std::size_t i = 0; i < cars && before + i < recorder.records().size(); ++i) {
                const LapRecord& r = recorder.records()[before + i];
                const auto at = std::find(order.begin(), order.end(), r.driverNumber);
                lapOk = lapOk && r.driverNumber == sim.drivers()[i].number() &&
                        r.lap == sim.lapsRun() && at != order.end() &&
                        r.position == static_cast<int>(at - order.begin()) + 1;
            }
            DCHECK(lapOk);
        }

        const DriverAnalytics analytics(recorder);
        for (std::size_t i = 0; i < cars; ++i) {
            const Driver& d = sim.drivers()[i];
            const DriverSummary& s = analytics.summary(d.number());
            DCHECK(s.laps == d.lapsCompleted() && s.laps == laps);
            // The same additions in the same order as Driver::completeLap: bit for bit.
            DCHECK_NEAR(s.totalTime, d.cumulativeTime(), 0.0);
            DCHECK_NEAR(s.greenTime + s.pitTime, s.totalTime, kTimeEpsilon);
            DCHECK_NEAR(s.trafficTime, d.trafficLoss(), kTimeEpsilon);
            DCHECK(s.stops == d.stopsMade());
            DCHECK_NEAR(s.pitTime, static_cast<double>(d.stopsMade()) * pitLoss, kTimeEpsilon);
            DCHECK(s.stints.size() == d.plan().size());
            sawTraffic = sawTraffic || s.trafficTime > 0.0;
            sawStop = sawStop || s.stops > 0;
            sawFreeStop = sawFreeStop || (s.stops > 0 && !(pitLoss > 0.0));

            // Walk the plan: each lap's tires, its time against the model, and
            // the tire module's record of the same lap.
            double planGreen = 0.0;
            bool lapsOk = true;
            int lap = 0;
            for (std::size_t st = 0; st < d.plan().size(); ++st) {
                const Stint& stint = d.plan()[st];
                double stintTraffic = 0.0;
                for (int age = 0; age < stint.laps; ++age) {
                    ++lap;
                    const LapRecord& r =
                        recorder.records()[static_cast<std::size_t>(lap - 1) * cars + i];
                    const double model =
                        track.baseLapTime() + compounds[stint.compoundIndex].lapDelta(age);
                    const bool endsStint = (age == stint.laps - 1) && (st + 1U < d.plan().size());
                    lapsOk = lapsOk && r.lap == lap && r.compoundIndex == stint.compoundIndex &&
                             r.tireAge == age && r.stint == static_cast<int>(st) &&
                             r.pitStop == endsStint &&
                             std::fabs(r.pitTime - (endsStint ? pitLoss : 0.0)) <= 0.0;
                    // Take the traffic out and the lap is the model's clean-air lap.
                    lapsOk = lapsOk && std::fabs((r.lapTime - r.trafficTime) - model) <= kTimeEpsilon;
                    if (!overtaking) {
                        lapsOk = lapsOk && std::fabs(r.lapTime - model) <= 0.0 &&
                                 std::fabs(r.trafficTime) <= 0.0;
                    }
                    lapsOk = lapsOk && std::fabs(tireModule.carLaps(i).lapTime(lap) - r.lapTime) <= 0.0;
                    stintTraffic += r.trafficTime;
                }
                const double cost = strategy.stintCost(stint.compoundIndex, stint.laps);
                planGreen += cost;
                if (st < s.stints.size()) {
                    const StintSummary& got = s.stints[st];
                    DCHECK(got.compoundIndex == stint.compoundIndex && got.laps == stint.laps &&
                           got.firstLap == lap - stint.laps + 1 && got.lastLap == lap);
                    // Per-stint contribution: clean-air time is the optimizer's stint cost.
                    DCHECK_NEAR(got.time - stintTraffic, cost, kClosureTolerance);
                }
            }
            DCHECK(lapsOk);
            // Traffic sits inside green-flag time and is counted once: take it
            // out and what is left is exactly the plan's green-flag time.
            DCHECK_NEAR(s.greenTime - s.trafficTime, planGreen, kClosureTolerance);
        }
    }
    DCHECK(sawTraffic);
    DCHECK(sawStop);
    DCHECK(sawFreeStop);
}

void testRecorderMisuse() {
    beginSection("LapRecorder rejects misuse and leaves its records untouched");

    Track track("Race", 5, 20.0);
    track.addSector("S1", 90.0);
    const std::vector<TireCompound> compounds{TireCompound("M", 0.0, 0.1, 5)};
    const PitStrategy strategy(track, compounds);
    const boxbox::StrategyResult plan = strategy.optimize();

    RaceSimulator sim(track, compounds);
    sim.addDriver(7, "A", plan);
    sim.addDriver(9, "B", plan);

    LapRecorder recorder(sim);
    recorder.observe(sim);  // no lap run yet: nothing to record, and no error
    DCHECK(recorder.records().empty());

    sim.advanceLap();
    sim.advanceLap();  // one observe() skipped
    DCHECK_THROWS(recorder.observe(sim), std::invalid_argument);
    DCHECK(recorder.records().empty());  // the rejected call recorded nothing

    DCHECK_THROWS(LapRecorder{sim}, std::invalid_argument);  // the race has started

    sim.reset();
    LapRecorder fresh(sim);
    sim.advanceLap();
    fresh.observe(sim);
    DCHECK(fresh.records().size() == 2U);
    sim.reset();  // the cars go back to the grid: laps went backwards
    DCHECK_THROWS(fresh.observe(sim), std::invalid_argument);
    DCHECK(fresh.records().size() == 2U);

    sim.addDriver(11, "C", plan);  // the field grows
    DCHECK_THROWS(fresh.observe(sim), std::invalid_argument);

    RaceSimulator twins(track, compounds);
    twins.addDriver(5, "A", plan);
    twins.addDriver(5, "B", plan);
    DCHECK_THROWS(LapRecorder{twins}, std::invalid_argument);  // cars are told apart by number

    // A race with no laps: nothing to record, and every summary is empty.
    Track none("None", 0, 20.0);
    none.addSector("S1", 90.0);
    const PitStrategy noneStrategy(none, compounds);
    RaceSimulator idle(none, compounds);
    idle.addDriver(3, "IDLE", noneStrategy.optimize());
    const LapRecorder nothing = recordRace(idle);
    DCHECK(nothing.records().empty());
    const DriverAnalytics empty(nothing);
    DCHECK(empty.summary(3).laps == 0 && empty.summary(3).bestLap.lap == 0);
    DCHECK(boxbox::rankDrivers(empty.summaries(), RankMetric::AverageLap) == std::vector<int>{3});
}

// ---------------------------------------------------------------------------
// DriverAnalytics on its own
// ---------------------------------------------------------------------------

void testAnalyticsValidation() {
    beginSection("DriverAnalytics: record checks, lookups and cars without laps");

    const std::vector<int> five{5};
    const std::vector<LapRecord> good{lapOf(5, 1, 90.0), lapOf(5, 2, 91.0)};
    DCHECK(!rejected(five, good));
    const auto broken = [&](auto&& change) {
        std::vector<LapRecord> records = good;
        change(records);
        return rejected(five, records);
    };

    DCHECK(rejected({5, 5}, {}));                                    // a number used twice
    DCHECK(rejected(five, {lapOf(6, 1, 90.0)}));                     // a car outside the field
    DCHECK(rejected(five, {lapOf(5, 2, 90.0)}));                     // starts on lap 2
    DCHECK(rejected(five, {lapOf(5, 1, 90.0), lapOf(5, 1, 90.5)}));  // lap 1 twice
    DCHECK(rejected(five, {lapOf(5, 1, 90.0), lapOf(5, 3, 90.5)}));  // lap 2 missing
    DCHECK(broken([](std::vector<LapRecord>& r) { r[1].lapTime = std::nan(""); }));
    DCHECK(broken([](std::vector<LapRecord>& r) { r[0].trafficTime = -0.1; }));
    DCHECK(broken([](std::vector<LapRecord>& r) {
        r[0].trafficTime = std::numeric_limits<double>::infinity();
    }));
    DCHECK(broken([](std::vector<LapRecord>& r) {
        r[0].pitStop = true;
        r[0].pitTime = -1.0;
        r[1].stint = 1;
    }));
    DCHECK(broken([](std::vector<LapRecord>& r) { r[0].pitTime = 20.0; }));  // no stop made
    DCHECK(broken([](std::vector<LapRecord>& r) { r[0].stint = 1; }));      // must start in 0
    DCHECK(broken([](std::vector<LapRecord>& r) { r[1].stint = 1; }));      // moved on, no stop
    DCHECK(broken([](std::vector<LapRecord>& r) { r[0].pitStop = true; }));  // stopped, lap 2 still stint 0
    DCHECK(broken([](std::vector<LapRecord>& r) { r[1].compoundIndex = 2; }));  // new tires, no stop
    DCHECK(broken([](std::vector<LapRecord>& r) { r[0].position = 0; }));
    DCHECK(broken([](std::vector<LapRecord>& r) { r[0].position = 2; }));  // one car in the field
    DCHECK(broken([](std::vector<LapRecord>& r) { r[0].tireAge = -1; }));
    // The lap time includes traffic, so traffic cannot be larger than the lap.
    DCHECK(broken([](std::vector<LapRecord>& r) { r[0].trafficTime = 95.0; }));
    DCHECK(!broken([](std::vector<LapRecord>& r) { r[0].trafficTime = 90.0; }));  // all of it
    // A window past MonotonicDeque::kMaxWindow is refused up front, not by the allocator.
    DCHECK_THROWS(MonotonicDeque(MonotonicDeque::kMaxWindow + 1U, MonotonicDeque::Kind::Minimum),
                  std::invalid_argument);
    // The legal version: a stop with a zero pit loss, then new tires in stint 1.
    DCHECK(!broken([](std::vector<LapRecord>& r) {
        r[0].pitStop = true;
        r[0].pitTime = 0.0;
        r[1].stint = 1;
        r[1].compoundIndex = 2;
    }));

    const DriverAnalytics two({3, 8}, {lapOf(8, 1, 90.0)});
    DCHECK(two.contains(3) && two.contains(8) && !two.contains(5));
    DCHECK_THROWS(two.summary(5), std::out_of_range);
    DCHECK_THROWS(two.lapTimes(5), std::out_of_range);
    DCHECK(two.summaries().size() == 2U && two.summaries()[0].driverNumber == 3 &&
           two.summaries()[1].driverNumber == 8);  // grid order
    const DriverSummary& idle = two.summary(3);
    DCHECK(idle.laps == 0 && idle.stops == 0 && idle.bestLap.lap == 0 && idle.worstLap.lap == 0 &&
           idle.stints.empty() && idle.compounds.empty() && idle.positions.empty() &&
           idle.bestPosition == 0 && idle.worstPosition == 0 && idle.name.empty());
    DCHECK_NEAR(idle.averageLap, 0.0, 0.0);
    DCHECK_NEAR(idle.totalTime, 0.0, 0.0);
    DCHECK(two.lapTimes(3).empty());
    const DriverAnalytics nobody({}, {});
    DCHECK(nobody.summaries().empty() && nobody.records().empty());

    // Cars' records may interleave; each car's own laps stay in order.
    std::vector<LapRecord> mixed{lapOf(2, 1, 91.0), lapOf(1, 1, 90.0), lapOf(1, 2, 90.5),
                                 lapOf(2, 2, 90.2)};
    for (LapRecord& r : mixed) {
        r.position = (r.driverNumber == 1) ? 1 : 2;
    }
    const DriverAnalytics interleaved({1, 2}, mixed);
    DCHECK(interleaved.lapTimes(2).size() == 2U);
    DCHECK_NEAR(interleaved.lapTimes(2)[0], 91.0, 0.0);
    DCHECK_NEAR(interleaved.lapTimes(2)[1], 90.2, 0.0);
    DCHECK(interleaved.summary(2).bestLap.lap == 2);
}

void testAnalyticsAgainstDirectCalculation() {
    beginSection("DriverAnalytics vs direct calculation, 80 seeded synthetic fields");

    Lcg rng(1212ULL);
    for (int trial = 0; trial < 80; ++trial) {
        const SyntheticField race = syntheticField(rng, trial % 2 == 0);
        const DriverAnalytics analytics(race.field, race.records);
        DCHECK(analytics.summaries().size() == race.field.size());
        DCHECK(analytics.records().size() == race.records.size());
        for (std::size_t c = 0; c < race.field.size(); ++c) {
            const int car = race.field[c];
            const DriverSummary& got = analytics.summary(car);
            DCHECK(&got == &analytics.summaries()[c]);  // the lookup lands on the grid slot
            checkAgainstDirect(got, race.records, car, analytics.lapTimes(car));
        }
    }
}

// ---------------------------------------------------------------------------
// Fastest and slowest laps
// ---------------------------------------------------------------------------

void testSelectLapsAgainstFullSort() {
    beginSection("selectLaps (bounded heap) vs a full sort, 60 seeded fields");

    using boxbox::lapRanksAhead;
    const LapRecord a = lapOf(1, 5, 90.0);
    const LapRecord b = lapOf(2, 5, 90.0);  // same time and lap: the car number decides
    const LapRecord c = lapOf(1, 6, 90.0);  // same time, a later lap
    const LapRecord d = lapOf(3, 1, 91.0);  // slower
    DCHECK(lapRanksAhead(a, d, LapOrder::Fastest) && !lapRanksAhead(d, a, LapOrder::Fastest));
    DCHECK(lapRanksAhead(d, a, LapOrder::Slowest) && !lapRanksAhead(a, d, LapOrder::Slowest));
    DCHECK(lapRanksAhead(a, b, LapOrder::Fastest) && lapRanksAhead(a, b, LapOrder::Slowest));
    DCHECK(lapRanksAhead(a, c, LapOrder::Fastest) && lapRanksAhead(a, c, LapOrder::Slowest));
    DCHECK(!lapRanksAhead(a, a, LapOrder::Fastest) && !lapRanksAhead(a, a, LapOrder::Slowest));

    const std::vector<LapRecord> bad{lapOf(1, 1, 90.0), lapOf(1, 2, std::nan(""))};
    DCHECK_THROWS(boxbox::selectLaps(bad, 1, LapOrder::Fastest), std::invalid_argument);
    LapFilter skipBad;
    skipBad.lastLap = 1;  // a filtered-out lap is never looked at
    DCHECK(boxbox::selectLaps(bad, 1, LapOrder::Fastest, skipBad).size() == 1U);

    Lcg rng(5151ULL);
    for (int trial = 0; trial < 60; ++trial) {
        const SyntheticField race = syntheticField(rng, trial % 2 == 0);
        const std::vector<LapRecord>& all = race.records;
        const std::size_t r = all.size();

        // lapRanksAhead agrees with the reference statement of the order.
        bool agrees = true;
        for (int q = 0; q < 50 && r > 0U; ++q) {
            const LapRecord& x = all[rng.nextIndex(r)];
            const LapRecord& y = all[rng.nextIndex(r)];
            for (const LapOrder o : {LapOrder::Fastest, LapOrder::Slowest}) {
                agrees = agrees && lapRanksAhead(x, y, o) == referenceAhead(x, y, o);
            }
        }
        DCHECK(agrees);

        std::vector<LapFilter> filters(1);  // the first considers every lap
        LapFilter oneCar;
        oneCar.driverNumber = race.field[rng.nextIndex(race.field.size())];
        filters.push_back(oneCar);
        LapFilter oneCompound;
        oneCompound.compoundIndex = rng.nextIndex(3);
        filters.push_back(oneCompound);
        LapFilter range;
        range.firstLap = rng.nextInt(1, 15);
        range.lastLap = range.firstLap + rng.nextInt(0, 15);
        filters.push_back(range);
        LapFilter allThree = range;
        allThree.driverNumber = oneCar.driverNumber;
        allThree.compoundIndex = oneCompound.compoundIndex;
        filters.push_back(allThree);
        LapFilter nobody;
        nobody.driverNumber = -1;
        filters.push_back(nobody);

        const std::vector<std::size_t> ks{0, 1, 2, 3, 7, r / 2U, (r > 0U) ? r - 1U : 0U, r, r + 5U};
        for (const LapOrder order : {LapOrder::Fastest, LapOrder::Slowest}) {
            for (const LapFilter& f : filters) {
                std::vector<LapRecord> expected;
                for (const LapRecord& rec : all) {
                    if (referencePasses(rec, f)) {
                        expected.push_back(rec);
                    }
                }
                std::sort(expected.begin(), expected.end(),
                          [order](const LapRecord& x, const LapRecord& y) {
                              return referenceAhead(x, y, order);
                          });
                for (const std::size_t k : ks) {
                    const std::vector<LapRecord> got = boxbox::selectLaps(all, k, order, f);
                    const std::size_t want = std::min(k, expected.size());
                    bool same = got.size() == want;
                    for (std::size_t i = 0; same && i < want; ++i) {
                        same = got[i].driverNumber == expected[i].driverNumber &&
                               got[i].lap == expected[i].lap;
                    }
                    DCHECK(same);
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Sliding windows
// ---------------------------------------------------------------------------

void testWindowsAgainstBruteForce() {
    beginSection("trailingWindows and summarizeWindows vs brute force, every window size");

    DCHECK_THROWS(boxbox::trailingWindows({90.0}, 0), std::invalid_argument);
    DCHECK_THROWS(boxbox::trailingWindows({90.0, std::nan("")}, 2), std::invalid_argument);
    DCHECK_THROWS(boxbox::summarizeWindows({90.0}, 0), std::invalid_argument);
    DCHECK(boxbox::trailingWindows({}, 3).empty());
    const WindowSummary none = boxbox::summarizeWindows({}, 3);
    DCHECK(!none.complete && none.window == 3U && none.quickest.lastLap == 0 &&
           none.widest.lastLap == 0);

    // W = 1 is every lap on its own; W = n and beyond start every window on lap 1.
    const std::vector<double> four{91.0, 90.0, 92.0, 90.0};
    const std::vector<LapWindow> single = boxbox::trailingWindows(four, 1);
    bool singleOk = single.size() == 4U;
    for (std::size_t i = 0; singleOk && i < 4U; ++i) {
        singleOk = single[i].laps == 1 && single[i].fastestLap == static_cast<int>(i) + 1 &&
                   single[i].slowestLap == static_cast<int>(i) + 1;
    }
    DCHECK(singleOk);
    for (const std::size_t w : {std::size_t{4}, std::size_t{9}}) {
        const std::vector<LapWindow> whole = boxbox::trailingWindows(four, w);
        DCHECK(whole.size() == 4U && whole[3].firstLap == 1 && whole[3].fastestLap == 2 &&
               whole[3].slowestLap == 3);  // 90.0 on laps 2 and 4: lap 2
    }

    Lcg rng(7272ULL);
    for (int trial = 0; trial < 120; ++trial) {
        const std::size_t n = rng.nextIndex(61);
        const bool ties = (trial % 2 == 0);
        std::vector<double> laps(n);
        for (double& t : laps) {
            t = ties ? 88.0 + static_cast<double>(rng.nextInt(0, 3)) : rng.nextDouble(85.0, 120.0);
        }

        for (std::size_t w = 1; w <= n + 2U; ++w) {
            const std::vector<LapWindow> got = boxbox::trailingWindows(laps, w);
            bool shapeOk = got.size() == n;
            bool extremesOk = true;
            bool averageOk = true;
            std::vector<double> average(n, 0.0);
            std::vector<double> spread(n, 0.0);
            for (std::size_t k = 0; k < n && shapeOk; ++k) {
                const std::size_t first = (k + 1U > w) ? k + 1U - w : 0U;
                std::size_t fast = first;
                std::size_t slow = first;
                double sum = 0.0;
                for (std::size_t j = first; j <= k; ++j) {
                    fast = (laps[j] < laps[fast]) ? j : fast;
                    slow = (laps[slow] < laps[j]) ? j : slow;
                    sum += laps[j];
                }
                average[k] = sum / static_cast<double>(k - first + 1U);
                spread[k] = laps[slow] - laps[fast];
                const LapWindow& g = got[k];
                shapeOk = g.firstLap == static_cast<int>(first) + 1 &&
                          g.lastLap == static_cast<int>(k) + 1 &&
                          g.laps == static_cast<int>(k - first) + 1;
                extremesOk = extremesOk && g.fastestLap == static_cast<int>(fast) + 1 &&
                             g.slowestLap == static_cast<int>(slow) + 1 &&
                             std::fabs(g.fastest - laps[fast]) <= 0.0 &&
                             std::fabs(g.slowest - laps[slow]) <= 0.0;
                averageOk = averageOk && std::fabs(g.average - average[k]) <= kTimeEpsilon;
            }
            DCHECK(shapeOk);
            DCHECK(extremesOk);
            DCHECK(averageOk);

            // The summary, by its documented rule, worked out on the brute-force numbers.
            const WindowSummary s = boxbox::summarizeWindows(laps, w);
            DCHECK(s.window == w && s.complete == (n >= w));
            if (n == 0U || !shapeOk) {
                continue;
            }
            const std::size_t start = (n >= w) ? w - 1U : n - 1U;
            std::size_t quick = start;
            std::size_t slowPick = start;
            std::size_t wide = start;
            for (std::size_t k = start + 1U; k < n; ++k) {
                quick = definitelyLess(average[k], average[quick]) ? k : quick;
                slowPick = definitelyLess(average[slowPick], average[k]) ? k : slowPick;
                wide = definitelyLess(spread[wide], spread[k]) ? k : wide;
            }
            DCHECK(s.quickest.lastLap == static_cast<int>(quick) + 1 &&
                   s.slowest.lastLap == static_cast<int>(slowPick) + 1 &&
                   s.widest.lastLap == static_cast<int>(wide) + 1);
            DCHECK_NEAR(s.dropOff, s.slowest.average - s.quickest.average, 0.0);
            DCHECK(!(s.dropOff < 0.0));
        }
    }
}

// ---------------------------------------------------------------------------
// Rankings
// ---------------------------------------------------------------------------

void testRankDrivers() {
    beginSection("rankDrivers: explicit tie-breaks, cars without laps, input order");

    const std::vector<DriverSummary> field{
        rankable(3, 10, 90.0, 89.0, 2.0, 0.5),
        rankable(1, 10, 90.0, 88.5, 3.0, 0.5),  // ties car 3 on average and mean dev
        rankable(8, 0, 0.0, 0.0, 0.0, 0.0),     // ran no laps
        rankable(2, 5, 89.0, 89.5, 1.0, 0.7),
        rankable(6, 0, 0.0, 0.0, 0.0, 0.0),
    };
    DCHECK((boxbox::rankDrivers(field, RankMetric::AverageLap) == std::vector<int>{2, 1, 3, 6, 8}));
    DCHECK((boxbox::rankDrivers(field, RankMetric::BestLap) == std::vector<int>{1, 3, 2, 6, 8}));
    DCHECK((boxbox::rankDrivers(field, RankMetric::LapRange) == std::vector<int>{2, 3, 1, 6, 8}));
    DCHECK((boxbox::rankDrivers(field, RankMetric::MeanAbsDeviation) ==
            std::vector<int>{1, 3, 2, 6, 8}));
    DCHECK(boxbox::rankDrivers({}, RankMetric::AverageLap).empty());
    DCHECK_NEAR(boxbox::rankValue(field[1], RankMetric::AverageLap), 90.0, 0.0);
    DCHECK_NEAR(boxbox::rankValue(field[1], RankMetric::BestLap), 88.5, 0.0);
    DCHECK_NEAR(boxbox::rankValue(field[1], RankMetric::LapRange), 3.0, 0.0);
    DCHECK_NEAR(boxbox::rankValue(field[1], RankMetric::MeanAbsDeviation), 0.5, 0.0);

    // Values from a small set, so exact ties are common. Any input order must
    // give the same ranking, and every pair in it must follow the rule.
    Lcg rng(9090ULL);
    for (int trial = 0; trial < 100; ++trial) {
        std::vector<DriverSummary> cars;
        const int count = rng.nextInt(0, 12);
        for (int i = 0; i < count; ++i) {
            const int laps = (rng.nextInt(0, 3) == 0) ? 0 : rng.nextInt(1, 50);
            cars.push_back(rankable(2 * i + 1, laps, 89.0 + 0.5 * rng.nextInt(0, 3),
                                    88.0 + 0.5 * rng.nextInt(0, 3), 0.5 * rng.nextInt(0, 3),
                                    0.25 * rng.nextInt(0, 3)));
        }
        std::vector<DriverSummary> shuffled = cars;
        for (std::size_t i = shuffled.size(); i > 1U; --i) {
            std::swap(shuffled[i - 1U], shuffled[rng.nextIndex(i)]);
        }

        for (const RankMetric metric : {RankMetric::AverageLap, RankMetric::BestLap,
                                        RankMetric::LapRange, RankMetric::MeanAbsDeviation}) {
            const std::vector<int> ranked = boxbox::rankDrivers(cars, metric);
            DCHECK(boxbox::rankDrivers(shuffled, metric) == ranked);
            bool ordered = ranked.size() == cars.size();
            for (std::size_t i = 0; ordered && i < ranked.size(); ++i) {
                for (std::size_t j = i + 1U; ordered && j < ranked.size(); ++j) {
                    const auto byNumber = [](int number) {
                        return [number](const DriverSummary& s) { return s.driverNumber == number; };
                    };
                    const auto x = std::find_if(cars.begin(), cars.end(), byNumber(ranked[i]));
                    const auto y = std::find_if(cars.begin(), cars.end(), byNumber(ranked[j]));
                    ordered = x != cars.end() && y != cars.end() && documentedBefore(*x, *y, metric);
                }
            }
            DCHECK(ordered);
        }
    }
}

// ---------------------------------------------------------------------------
// Determinism
// ---------------------------------------------------------------------------

void testDeterminism() {
    beginSection("Determinism: the same race recorded twice gives identical records and results");

    const auto run = [](std::uint64_t seed) {
        Lcg rng(seed);
        Track track("Race", 30, 21.0);
        track.addSector("S1", 30.0);
        track.addSector("S2", 30.0);
        track.addSector("S3", 30.0);
        const std::vector<TireCompound> compounds{randomCompound(rng), randomCompound(rng)};
        const PitStrategy strategy(track, compounds);
        RaceSimulator sim(track, compounds);
        sim.setOvertakeRule(boxbox::OvertakeRule{true, 0.5, 1.0});
        sim.addDriver(1, "OPT", strategy.optimize());
        sim.addDriver(2, "A", strategy.singleCompoundPlan(0));
        sim.addDriver(3, "B", strategy.singleCompoundPlan(1));
        return recordRace(sim);
    };

    const LapRecorder first = run(4242ULL);
    const LapRecorder second = run(4242ULL);
    bool same = first.records().size() == second.records().size();
    for (std::size_t i = 0; same && i < first.records().size(); ++i) {
        const LapRecord& x = first.records()[i];
        const LapRecord& y = second.records()[i];
        same = x.driverNumber == y.driverNumber && x.lap == y.lap &&
               std::fabs(x.lapTime - y.lapTime) <= 0.0 &&
               std::fabs(x.trafficTime - y.trafficTime) <= 0.0 &&
               std::fabs(x.pitTime - y.pitTime) <= 0.0 && x.pitStop == y.pitStop &&
               x.compoundIndex == y.compoundIndex && x.tireAge == y.tireAge &&
               x.stint == y.stint && x.position == y.position;
    }
    DCHECK(same);

    const DriverAnalytics a(first);
    const DriverAnalytics b(second);
    bool summariesSame = a.summaries().size() == b.summaries().size();
    for (std::size_t i = 0; summariesSame && i < a.summaries().size(); ++i) {
        const DriverSummary& x = a.summaries()[i];
        const DriverSummary& y = b.summaries()[i];
        summariesSame = std::fabs(x.totalTime - y.totalTime) <= 0.0 &&
                        std::fabs(x.meanAbsDeviation - y.meanAbsDeviation) <= 0.0 &&
                        x.bestLap.lap == y.bestLap.lap && x.worstLap.lap == y.worstLap.lap &&
                        x.positions == y.positions;
    }
    DCHECK(summariesSame);
    for (const RankMetric metric : {RankMetric::AverageLap, RankMetric::MeanAbsDeviation}) {
        DCHECK(boxbox::rankDrivers(a.summaries(), metric) ==
               boxbox::rankDrivers(b.summaries(), metric));
    }
    const std::vector<LapRecord> topA = boxbox::selectLaps(a.records(), 5, LapOrder::Fastest);
    const std::vector<LapRecord> topB = boxbox::selectLaps(b.records(), 5, LapOrder::Fastest);
    bool topSame = topA.size() == topB.size();
    for (std::size_t i = 0; topSame && i < topA.size(); ++i) {
        topSame = topA[i].driverNumber == topB[i].driverNumber && topA[i].lap == topB[i].lap;
    }
    DCHECK(topSame);
}

void testMonotonicDequeLarge() {
    beginSection("MonotonicDeque: 100,000-value streams against a brute-force scan, and clear()");

    Lcg rng(8642ULL);
    std::vector<double> values(100000);
    for (double& v : values) v = static_cast<double>(rng.nextInt(0, 4000));   // many exact ties

    for (const std::size_t window : {std::size_t{1}, std::size_t{2}, std::size_t{7}, std::size_t{300},
                                     std::size_t{100000}, std::size_t{150000}}) {
        MonotonicDeque low(window, MonotonicDeque::Kind::Minimum);
        MonotonicDeque high(window, MonotonicDeque::Kind::Maximum);
        // The O(L*W) reference is only run where it is affordable; the two big windows are
        // checked against a running extreme instead (W >= L means the window is the whole stream).
        const bool scan = window <= 300U;
        bool agree = true;
        double runningLow = values[0];
        double runningHigh = values[0];
        for (std::size_t i = 0; i < values.size(); ++i) {
            low.push(values[i]);
            high.push(values[i]);
            runningLow = std::min(runningLow, values[i]);
            runningHigh = std::max(runningHigh, values[i]);
            if (scan) {
                const std::size_t from = i + 1U > window ? i + 1U - window : 0U;
                double lo = values[from];
                double hi = values[from];
                std::size_t loIndex = from;
                std::size_t hiIndex = from;
                for (std::size_t j = from + 1U; j <= i; ++j) {
                    if (values[j] < lo) { lo = values[j]; loIndex = j; }
                    if (values[j] > hi) { hi = values[j]; hiIndex = j; }
                }
                agree = agree && low.extreme() == lo && high.extreme() == hi &&
                        low.extremeIndex() == loIndex && high.extremeIndex() == hiIndex;   // earliest on ties
            } else {
                agree = agree && low.extreme() == runningLow && high.extreme() == runningHigh;
            }
            agree = agree && low.size() >= 1U && low.size() <= std::min(window, i + 1U);
        }
        DCHECK_MSG(agree, "window " + std::to_string(window));
        DCHECK(low.pushed() == values.size());
    }

    // clear() forgets the stream but keeps the window and kind.
    MonotonicDeque dq(3, MonotonicDeque::Kind::Minimum);
    for (const double v : {5.0, 1.0, 4.0, 2.0}) dq.push(v);
    DCHECK_NEAR(dq.extreme(), 1.0, 0.0);
    dq.clear();
    DCHECK(dq.empty() && dq.pushed() == 0U && dq.window() == 3U);
    DCHECK_THROWS(dq.extreme(), std::out_of_range);
    dq.push(9.0);
    dq.push(8.0);
    DCHECK_NEAR(dq.extreme(), 8.0, 0.0);
    DCHECK(dq.extremeIndex() == 1U);
}

// ---------------------------------------------------------------------------
// Sector analytics: a MODELLED split of each lap
// ---------------------------------------------------------------------------

/// A record carrying explicit sector times, for the arithmetic checks.
LapRecord sectorLap(int car, int lap, std::initializer_list<double> sectors) {
    LapRecord r = lapOf(car, lap, 0.0);
    r.sectorTimes = sectors;
    for (const double s : r.sectorTimes) {
        r.lapTime += s;
    }
    return r;
}

void testModelledSplit() {
    beginSection("modelledSectorTimes and the recorder's sector split");

    Track track("T", 5, 10.0);
    track.addSector("S1", 20.0);
    track.addSector("S2", 30.0);
    track.addSector("S3", 50.0);
    const std::vector<double> split = boxbox::modelledSectorTimes(track, 90.0);  // 100 s of base
    DCHECK(split.size() == 3U);
    if (split.size() == 3U) {
        DCHECK_NEAR(split[0], 18.0, 1e-12);
        DCHECK_NEAR(split[1], 27.0, 1e-12);
        DCHECK_NEAR(split[2], 45.0, 1e-12);
        DCHECK_NEAR(split[0] + split[1] + split[2], 90.0, 1e-12);
    }
    // Any lap time, including a slow one: the parts always add back to it.
    for (const double lap : {0.0, 61.7, 100.0, 133.333}) {
        double sum = 0.0;
        for (const double part : boxbox::modelledSectorTimes(track, lap)) sum += part;
        DCHECK_NEAR(sum, lap, 1e-9);
    }

    Track none("N", 5, 10.0);
    DCHECK(boxbox::modelledSectorTimes(none, 90.0).empty());     // no sectors
    Track zero("Z", 5, 10.0);
    zero.addSector("S1", 0.0);
    DCHECK(boxbox::modelledSectorTimes(zero, 90.0).empty());     // base lap time 0

    // Through the recorder, on a real race: count, sign and sum.
    const std::vector<TireCompound> compounds{TireCompound("A", 0.0, 0.1, 5), TireCompound("B", 0.5, 0.0, 5)};
    const PitStrategy strategy(track, compounds);
    RaceSimulator sim(track, compounds);
    sim.setOvertakeRule(boxbox::OvertakeRule{true, 0.5, 1.0});
    sim.addDriver(1, "ONE", strategy.optimize());
    sim.addDriver(2, "TWO", strategy.singleCompoundPlan(1));
    const LapRecorder recorder = recordRace(sim);
    DCHECK(!recorder.records().empty());
    bool shaped = true;
    bool adds = true;
    for (const LapRecord& r : recorder.records()) {
        shaped = shaped && r.sectorTimes.size() == 3U;
        double sum = 0.0;
        for (const double s : r.sectorTimes) {
            shaped = shaped && s >= 0.0;
            sum += s;
        }
        adds = adds && std::fabs(sum - r.lapTime) <= 1e-9;
    }
    DCHECK(shaped);
    DCHECK(adds);
    const DriverAnalytics analytics(recorder);        // validation accepts what the recorder wrote
    DCHECK(analytics.summaries().size() == 2U);

    // A sector-less track records none, and the analytics says so.
    Track bare("Bare", 3, 5.0);
    const PitStrategy bareStrategy(bare, compounds);
    RaceSimulator bareSim(bare, compounds);
    bareSim.addDriver(1, "ONE", bareStrategy.optimize());
    const LapRecorder bareRecorder = recordRace(bareSim);
    for (const LapRecord& r : bareRecorder.records()) DCHECK(r.sectorTimes.empty());
}

void testSectorArithmetic() {
    beginSection("Sector analytics: hand case, validation, and missing data");

    const std::vector<LapRecord> records{
        sectorLap(1, 1, {10.0, 20.0, 30.0}), sectorLap(2, 1, {12.0, 18.0, 31.0}),
        sectorLap(1, 2, {9.0, 21.0, 30.0}),  sectorLap(2, 2, {12.0, 19.0, 30.0}),
        sectorLap(1, 3, {11.0, 19.0, 30.0}), sectorLap(2, 3, {13.0, 20.0, 33.0})};
    std::vector<LapRecord> placed = records;
    for (LapRecord& r : placed) r.position = r.driverNumber;
    const DriverAnalytics analytics({1, 2}, placed);

    const std::vector<boxbox::DriverSectorSummary> sums = boxbox::sectorSummaries(analytics);
    DCHECK(sums.size() == 2U && sums[0].available && sums[1].available);
    // Car 1: fastest 9 (lap 2), 19 (lap 3), 30 (lap 1, an exact tie keeps the earliest lap).
    DCHECK(sums[0].fastest.size() == 3U);
    DCHECK(sums[0].fastest[0].lap == 2 && std::fabs(sums[0].fastest[0].time - 9.0) <= 1e-12);
    DCHECK(sums[0].fastest[1].lap == 3 && std::fabs(sums[0].fastest[1].time - 19.0) <= 1e-12);
    DCHECK(sums[0].fastest[2].lap == 1 && std::fabs(sums[0].fastest[2].time - 30.0) <= 1e-12);
    DCHECK_NEAR(sums[0].average[0], 10.0, 1e-12);
    DCHECK_NEAR(sums[0].average[1], 20.0, 1e-12);
    DCHECK_NEAR(sums[0].average[2], 30.0, 1e-12);
    DCHECK_NEAR(sums[0].bestCombinedLap, 58.0, 1e-12);       // 9 + 19 + 30
    DCHECK_NEAR(sums[1].bestCombinedLap, 12.0 + 18.0 + 30.0, 1e-12);

    // Field: best 9 (car 1, lap 2), 18 (car 2, lap 1), 30 (car 1, lap 1: tie with car 2's lap 2).
    const std::vector<boxbox::FieldSectorBest> field = boxbox::fieldBestSectors(analytics);
    DCHECK(field.size() == 3U);
    if (field.size() == 3U) {
        DCHECK(field[0].driverNumber == 1 && field[0].lap == 2);
        DCHECK(field[1].driverNumber == 2 && field[1].lap == 1);
        DCHECK(field[2].driverNumber == 1 && field[2].lap == 1);
    }
    DCHECK_NEAR(boxbox::fieldBestCombinedLap(analytics), 9.0 + 18.0 + 30.0, 1e-12);

    // Distribution of the last sector: 30 30 30 31 30 33 -> min 30, max 33.
    const boxbox::SectorDistribution d = boxbox::sectorDistribution(analytics, 2, 3);
    DCHECK(d.count == 6);
    DCHECK_NEAR(d.minimum, 30.0, 1e-12);
    DCHECK_NEAR(d.maximum, 33.0, 1e-12);
    DCHECK_NEAR(d.mean, (30.0 + 31.0 + 30.0 + 30.0 + 30.0 + 33.0) / 6.0, 1e-12);
    DCHECK(d.histogram.size() == 3U);
    DCHECK(d.histogram.size() == 3U && d.histogram[0] == 4 && d.histogram[1] == 1 && d.histogram[2] == 1);
    double squares = 0.0;
    for (const double t : {30.0, 31.0, 30.0, 30.0, 30.0, 33.0}) squares += (t - d.mean) * (t - d.mean);
    DCHECK_NEAR(d.standardDeviation, std::sqrt(squares / 6.0), 1e-12);
    const boxbox::SectorDistribution one = boxbox::sectorDistribution(analytics, 0, 1);
    DCHECK(one.histogram.size() == 1U && one.histogram[0] == 6);
    DCHECK_THROWS(boxbox::sectorDistribution(analytics, 3, 3), std::out_of_range);
    DCHECK_THROWS(boxbox::sectorDistribution(analytics, 0, 0), std::invalid_argument);

    // Comparison, car 1 against car 2.
    const std::vector<boxbox::SectorComparisonRow> cmp = boxbox::compareSectors(analytics, 1, 2);
    DCHECK(cmp.size() == 3U);
    if (cmp.size() == 3U) {
        DCHECK_NEAR(cmp[0].averageA, 10.0, 1e-12);
        DCHECK_NEAR(cmp[0].averageB, 37.0 / 3.0, 1e-12);
        DCHECK_NEAR(cmp[0].averageGap, 10.0 - 37.0 / 3.0, 1e-12);
        DCHECK(cmp[0].averageGap < 0.0);                     // car 1 quicker in sector 1
        DCHECK_NEAR(cmp[1].fastestB, 18.0, 1e-12);
    }
    DCHECK_THROWS(boxbox::compareSectors(analytics, 1, 9), std::out_of_range);

    // Records the checks must refuse.
    const auto rejects = [&](auto&& change) {
        std::vector<LapRecord> bad = placed;
        change(bad);
        bool threw = false;
        try {
            const DriverAnalytics a({1, 2}, bad);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        return threw;
    };
    DCHECK(rejects([](std::vector<LapRecord>& r) { r[0].sectorTimes[1] = -1.0; r[0].lapTime -= 21.0; }));
    DCHECK(rejects([](std::vector<LapRecord>& r) { r[0].sectorTimes[0] = std::nan(""); }));
    DCHECK(rejects([](std::vector<LapRecord>& r) { r[0].lapTime += 5.0; }));          // parts no longer add up
    DCHECK(rejects([](std::vector<LapRecord>& r) { r[2].sectorTimes.pop_back(); r[2].lapTime -= 30.0; }));
    DCHECK(!rejects([](std::vector<LapRecord>&) {}));

    // Records with no sector times: nothing available, nothing invented.
    const DriverAnalytics plain({1}, {lapOf(1, 1, 90.0), lapOf(1, 2, 91.0)});
    DCHECK(!boxbox::sectorSummaries(plain)[0].available);
    DCHECK(boxbox::fieldBestSectors(plain).empty());
    DCHECK_NEAR(boxbox::fieldBestCombinedLap(plain), 0.0, 0.0);
    DCHECK_THROWS(boxbox::sectorDistribution(plain, 0, 2), std::invalid_argument);
    DCHECK_THROWS(boxbox::compareSectors(plain, 1, 1), std::invalid_argument);
}

void testSectorAgainstBruteForce() {
    beginSection("Sector analytics vs direct calculation, 60 seeded fields");

    Lcg rng(6060ULL);
    for (int trial = 0; trial < 60; ++trial) {
        const int cars = rng.nextInt(1, 5);
        const std::size_t sectors = static_cast<std::size_t>(rng.nextInt(1, 4));
        std::vector<int> field;
        std::vector<LapRecord> records;
        std::vector<int> laps(static_cast<std::size_t>(cars), 0);
        for (int c = 0; c < cars; ++c) field.push_back(10 + c);
        const int total = rng.nextInt(0, 24);
        for (int i = 0; i < total; ++i) {
            const std::size_t c = rng.nextIndex(static_cast<std::size_t>(cars));
            LapRecord r = lapOf(field[c], ++laps[c], 0.0);
            r.position = 1;
            for (std::size_t k = 0; k < sectors; ++k) {
                // Coarse values on purpose, so exact ties happen.
                r.sectorTimes.push_back(20.0 + static_cast<double>(rng.nextInt(0, 6)));
            }
            for (const double s : r.sectorTimes) r.lapTime += s;
            records.push_back(r);
        }
        const DriverAnalytics analytics(field, records);
        const std::vector<boxbox::DriverSectorSummary> sums = boxbox::sectorSummaries(analytics);
        const std::vector<boxbox::FieldSectorBest> best = boxbox::fieldBestSectors(analytics);

        // Direct passes.
        for (std::size_t c = 0; c < field.size(); ++c) {
            std::vector<double> fastest(sectors, 1.0e9);
            std::vector<int> fastLap(sectors, 0);
            std::vector<double> sum(sectors, 0.0);
            int n = 0;
            for (const LapRecord& r : records) {
                if (r.driverNumber != field[c]) continue;
                ++n;
                for (std::size_t k = 0; k < sectors; ++k) {
                    if (r.sectorTimes[k] < fastest[k]) {
                        fastest[k] = r.sectorTimes[k];
                        fastLap[k] = r.lap;
                    }
                    sum[k] += r.sectorTimes[k];
                }
            }
            DCHECK(sums[c].available == (n > 0));
            if (n == 0) continue;
            double combined = 0.0;
            for (std::size_t k = 0; k < sectors; ++k) {
                DCHECK_NEAR(sums[c].fastest[k].time, fastest[k], 0.0);
                DCHECK(sums[c].fastest[k].lap == fastLap[k]);
                DCHECK_NEAR(sums[c].average[k], sum[k] / n, 1e-9);
                combined += fastest[k];
            }
            DCHECK_NEAR(sums[c].bestCombinedLap, combined, 1e-9);
        }

        if (records.empty()) {
            DCHECK(best.empty());
            continue;
        }
        DCHECK(best.size() == sectors);
        for (std::size_t k = 0; k < sectors; ++k) {
            double lowest = 1.0e9;
            for (const LapRecord& r : records) lowest = std::min(lowest, r.sectorTimes[k]);
            DCHECK_NEAR(best[k].time, lowest, 0.0);
            // The tie-break: among records at the best time, the earliest lap, then the lowest car.
            int bestLap = 1 << 30;
            int bestCar = 1 << 30;
            for (const LapRecord& r : records) {
                if (r.sectorTimes[k] == lowest &&
                    (r.lap < bestLap || (r.lap == bestLap && r.driverNumber < bestCar))) {
                    bestLap = r.lap;
                    bestCar = r.driverNumber;
                }
            }
            DCHECK(best[k].lap == bestLap && best[k].driverNumber == bestCar);

            const boxbox::SectorDistribution d = boxbox::sectorDistribution(analytics, k, 4);
            int inBins = 0;
            for (const int count : d.histogram) inBins += count;
            DCHECK(d.count == static_cast<int>(records.size()) && inBins == d.count);
            DCHECK_NEAR(d.minimum, lowest, 0.0);
            DCHECK(d.minimum <= d.mean + 1e-9 && d.mean <= d.maximum + 1e-9);
            DCHECK(d.standardDeviation >= 0.0);
        }
    }
}

}  // namespace

DriverTestTally runDriverAnalyticsTests() {
    g_tally = DriverTestTally{};

    testMonotonicDequeHandCases();
    testMonotonicDequeAgainstBruteForce();
    testHandRaceRecords();
    testHandRaceAnalytics();
    testRecorderOnSeededRaces();
    testRecorderMisuse();
    testAnalyticsValidation();
    testAnalyticsAgainstDirectCalculation();
    testSelectLapsAgainstFullSort();
    testWindowsAgainstBruteForce();
    testRankDrivers();
    testDeterminism();
    testMonotonicDequeLarge();
    testModelledSplit();
    testSectorArithmetic();
    testSectorAgainstBruteForce();

    return g_tally;
}
