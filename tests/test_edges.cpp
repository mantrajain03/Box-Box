// Box Box: edge cases and guards found by the coverage review.
//
// Each section targets lines that the module tests never executed and that a
// caller can reach: invalid arguments, plans that do not cover the race, cars
// with no laps, partial sector data, and report inputs at their limits. Lines
// that cannot be reached through the public interface (defensive guards) are
// listed in docs/COMPLEXITY_AUDIT.md rather than tested.

#include "EdgeTests.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "PitReport.hpp"
#include "StrategyCommands.hpp"
#include "TireCommands.hpp"
#include "DriverReport.hpp"
#include "boxbox/Common.hpp"
#include "boxbox/Driver.hpp"
#include "boxbox/DriverAnalytics.hpp"
#include "boxbox/EventLog.hpp"
#include "boxbox/EventPriorityQueue.hpp"
#include "boxbox/PitAnalytics.hpp"
#include "boxbox/PitStrategy.hpp"
#include "boxbox/RaceLapAnalytics.hpp"
#include "boxbox/RaceSimulator.hpp"
#include "boxbox/TireCompound.hpp"
#include "boxbox/Track.hpp"

namespace {

using boxbox::PitStrategy;
using boxbox::PlannedStint;
using boxbox::TireCompound;
using boxbox::Track;

EdgeTestTally g_tally;
std::string g_section;

void beginSection(const std::string& name) {
    g_section = name;
    std::cout << "\n== " << name << '\n';
}

void reportCheck(bool ok, const char* expression, const char* file, int line) {
    ++g_tally.checks;
    if (ok) {
        return;
    }
    ++g_tally.failures;
    std::cout << "   FAIL  " << file << ':' << line << "  in " << g_section << '\n'
              << "         " << expression << '\n';
}

#define XCHECK(expr) reportCheck((expr), #expr, __FILE__, __LINE__)

#define XCHECK_THROWS(statement, ExType)                                                \
    do {                                                                                \
        bool threw = false;                                                             \
        try {                                                                           \
            statement;                                                                  \
        } catch (const ExType&) {                                                       \
            threw = true;                                                               \
        } catch (...) {                                                                 \
        }                                                                               \
        reportCheck(threw, "throws " #ExType ": " #statement, __FILE__, __LINE__);      \
    } while (false)

bool has(const std::string& text, const std::string& fragment) {
    return text.find(fragment) != std::string::npos;
}

const double kNaN = std::numeric_limits<double>::quiet_NaN();
const double kInf = std::numeric_limits<double>::infinity();

Track handTrack(int laps = 4) {
    Track t("Hand", laps, 5.0);
    t.addSector("S1", 10.0);
    return t;
}

void testNonFiniteArguments() {
    beginSection("Non-finite arguments are rejected by Track and TireCompound");

    XCHECK_THROWS(Track("T", 5, kNaN), std::invalid_argument);
    XCHECK_THROWS(Track("T", 5, kInf), std::invalid_argument);
    XCHECK_THROWS(Track("T", 5, -1.0), std::invalid_argument);
    XCHECK_THROWS(Track("T", -1, 1.0), std::invalid_argument);
    Track t("T", 5, 1.0);
    XCHECK_THROWS(t.addSector("S", kNaN), std::invalid_argument);
    XCHECK_THROWS(t.addSector("S", kInf), std::invalid_argument);
    XCHECK_THROWS(t.addSector("S", -2.0), std::invalid_argument);
    XCHECK_THROWS(t.setPitLoss(kNaN), std::invalid_argument);
    XCHECK_THROWS(t.setTotalLaps(-1), std::invalid_argument);
    XCHECK(t.sectorCount() == 0U);   // rejected sectors leave the track unchanged

    XCHECK_THROWS(TireCompound("C", kNaN, 0.1, 5), std::invalid_argument);
    XCHECK_THROWS(TireCompound("C", 0.0, kInf, 5), std::invalid_argument);
    XCHECK_THROWS(TireCompound("C", 0.0, 0.1, 5, 2, kNaN), std::invalid_argument);
    XCHECK_THROWS(TireCompound("C", 0.0, -0.1, 5), std::invalid_argument);
    XCHECK_THROWS(TireCompound("C", 0.0, 0.1, 0), std::invalid_argument);
    XCHECK_THROWS(TireCompound("C", 0.0, 0.1, 5, 2, -0.5), std::invalid_argument);
    XCHECK(TireCompound("", 0.0, 0.1, 5).code() == '?');          // no name, no code
    XCHECK(TireCompound("soft", 0.0, 0.1, 5).code() == 'S');
}

void testPlanCoverageAndGuards() {
    beginSection("evaluatePlan and the simulator reject plans that do not cover the race");

    const Track track = handTrack(4);
    const std::vector<TireCompound> compounds{TireCompound("A", 0.0, 1.0, 2), TireCompound("B", 2.0, 0.0, 4)};
    const PitStrategy strategy(track, compounds);

    // Every stint is legal, but the lap total is wrong.
    XCHECK(!strategy.evaluatePlan({PlannedStint{0, 2}}).feasible);                       // 2 of 4
    XCHECK(!strategy.evaluatePlan({PlannedStint{1, 4}, PlannedStint{0, 1}}).feasible);   // 5 of 4
    XCHECK(strategy.evaluatePlan({PlannedStint{0, 2}, PlannedStint{0, 2}}).feasible);
    XCHECK(!strategy.evaluatePlan({}).feasible);                                         // empty, 4 laps
    const Track none = handTrack(0);
    const PitStrategy zero(none, compounds);
    XCHECK(zero.evaluatePlan({}).feasible);
    XCHECK(!zero.evaluatePlan({PlannedStint{0, 1}}).feasible);

    // RaceSimulator guards.
    XCHECK_THROWS(boxbox::RaceSimulator(track, {}), std::invalid_argument);
    boxbox::RaceSimulator sim(track, compounds);
    const auto stint = [&](std::size_t compound, int laps) {
        boxbox::Stint s;
        s.compoundIndex = compound;
        s.compoundName = "x";
        s.laps = laps;
        return std::vector<boxbox::Stint>{s};
    };
    XCHECK_THROWS(sim.addDriver(1, "bad compound", stint(5, 4)), std::invalid_argument);
    XCHECK_THROWS(sim.addDriver(1, "too long", stint(0, 4)), std::invalid_argument);   // A allows 2
    XCHECK_THROWS(sim.addDriver(1, "zero laps", stint(0, 0)), std::invalid_argument);
    XCHECK_THROWS(sim.addDriver(1, "short", stint(1, 3)), std::invalid_argument);       // covers 3 of 4
    XCHECK_THROWS(sim.addDriver(1, "infeasible", boxbox::StrategyResult{}), std::invalid_argument);
    XCHECK(sim.drivers().empty());     // every rejected driver left the field untouched

    // A car with an empty plan on a 0-lap race: finished from the start, no tires fitted.
    boxbox::RaceSimulator idle(none, compounds);
    idle.addDriver(9, "IDLE", std::vector<boxbox::Stint>{});
    XCHECK(idle.finished());
    XCHECK(!idle.advanceLap());
    XCHECK(idle.drivers().front().currentCompoundName().empty());
    XCHECK(idle.drivers().front().currentCompoundIndex() == 0U);
    XCHECK(idle.lapTimeFor(idle.drivers().front()) == 0.0);     // nothing to run, so no time
}

/// Records for hand-built analytics.
boxbox::LapRecord record(int car, int lap, double seconds, std::size_t compound,
                         std::vector<double> sectors) {
    boxbox::LapRecord r;
    r.driverNumber = car;
    r.lap = lap;
    r.lapTime = seconds;
    r.compoundIndex = compound;
    r.position = 1;
    r.sectorTimes = std::move(sectors);
    return r;
}

void testPartialSectorsAndOddReports() {
    beginSection("Partial sector data, cars with no laps, and unknown compounds in reports");

    // Car 1 has sector times, car 2 has none: legal, and the distribution skips car 2's laps.
    std::vector<boxbox::LapRecord> mixed{record(1, 1, 30.0, 0, {10.0, 20.0}),
                                         record(2, 1, 31.0, 0, {}),
                                         record(1, 2, 29.0, 0, {9.0, 20.0}),
                                         record(2, 2, 32.0, 0, {})};
    for (boxbox::LapRecord& r : mixed) r.position = r.driverNumber;
    const boxbox::DriverAnalytics analytics({1, 2}, mixed);
    const boxbox::SectorDistribution d = boxbox::sectorDistribution(analytics, 1, 2);
    XCHECK(d.count == 2);                                        // car 2 contributed nothing
    XCHECK(std::fabs(d.mean - 20.0) <= 1e-12);
    const std::vector<boxbox::DriverSectorSummary> sums = boxbox::sectorSummaries(analytics);
    XCHECK(sums[0].available && !sums[1].available);
    XCHECK(boxbox::fieldBestSectors(analytics).size() == 2U);

    // Records naming a compound the race does not have: the report falls back to "compound N".
    const Track track = handTrack(3);
    const std::vector<TireCompound> compounds{TireCompound("A", 0.0, 1.0, 3)};
    boxbox::RaceSimulator sim(track, compounds);
    const std::vector<boxbox::LapRecord> odd{record(1, 1, 10.0, 9, {10.0})};
    const boxbox::DriverAnalytics oddAnalytics({1}, odd);
    std::ostringstream top;
    boxbox::printTopLaps(top, sim, oddAnalytics, 1);
    XCHECK(has(top.str(), "compound 9"));

    // A race nobody has run yet: every report prints a dash or says so, none throws.
    const PitStrategy strategy(track, compounds);
    boxbox::RaceSimulator fresh(track, compounds);
    fresh.addDriver(1, "ONE", strategy.optimize());
    fresh.addDriver(2, "TWO", strategy.optimize());
    const boxbox::LapRecorder recorder(fresh);                   // no observe(): no records
    const boxbox::DriverAnalytics empty(recorder);
    std::ostringstream stats;
    boxbox::printDriverStats(stats, fresh, empty);
    XCHECK(has(stats.str(), "Driver statistics"));
    std::ostringstream laps;
    boxbox::printTopLaps(laps, fresh, empty, 3);
    XCHECK(has(laps.str(), "no laps were run"));
    std::ostringstream windows;
    boxbox::printLapWindows(windows, empty, 4);
    XCHECK(has(windows.str(), "Rolling 4-lap windows"));
    std::ostringstream sectors;
    boxbox::printSectorStats(sectors, fresh, empty);
    XCHECK(has(sectors.str(), "No sector data"));
}

void testRecorderFieldChanges() {
    beginSection("Recorders refuse a field that changes size");

    const Track track = handTrack(3);
    const std::vector<TireCompound> compounds{TireCompound("A", 0.0, 1.0, 3)};
    const PitStrategy strategy(track, compounds);
    boxbox::RaceSimulator sim(track, compounds);
    sim.addDriver(1, "ONE", strategy.optimize());
    boxbox::RaceLapRecorder lapRecorder(sim);
    boxbox::LapRecorder recorder(sim);
    sim.addDriver(2, "LATE", strategy.optimize());               // a car joins after the recorders
    sim.advanceLap();
    XCHECK_THROWS(lapRecorder.observe(sim), std::invalid_argument);
    XCHECK_THROWS(recorder.observe(sim), std::invalid_argument);
    XCHECK(lapRecorder.lapsObserved() == 0);                     // nothing was recorded
    XCHECK(recorder.records().empty());
}

void testRecorderOnAnotherRace() {
    beginSection("LapRecorder refuses a simulator whose cars differ, and reports tolerate sector-less cars");

    const Track track = handTrack(3);
    const std::vector<TireCompound> compounds{TireCompound("A", 0.0, 1.0, 3)};
    const PitStrategy strategy(track, compounds);
    boxbox::RaceSimulator first(track, compounds);
    first.addDriver(1, "ONE", strategy.optimize());
    first.addDriver(2, "TWO", strategy.optimize());
    boxbox::RaceSimulator other(track, compounds);
    other.addDriver(7, "SEVEN", strategy.optimize());     // same field size, different numbers
    other.addDriver(8, "EIGHT", strategy.optimize());
    boxbox::LapRecorder recorder(first);
    other.advanceLap();
    XCHECK_THROWS(recorder.observe(other), std::invalid_argument);
    XCHECK(recorder.records().empty());

    // A sector report where one car has sector times and the other has none prints a dash for it.
    std::vector<boxbox::LapRecord> mixed{record(1, 1, 30.0, 0, {10.0, 20.0}), record(2, 1, 31.0, 0, {})};
    mixed[1].position = 2;
    const boxbox::DriverAnalytics analytics({1, 2}, mixed);
    std::ostringstream out;
    boxbox::printSectorStats(out, first, analytics);
    XCHECK(has(out.str(), "Best sector in the field"));
    XCHECK(has(out.str(), "  -\n"));
}

/// A 6-lap race, one 10 s sector, 5 s pit loss, compounds A (up to 3 laps) and B (up to 6).
struct ShortRace {
    Track track{"Short", 6, 5.0};
    std::vector<TireCompound> compounds;
    ShortRace() {
        track.addSector("S1", 10.0);
        compounds = {TireCompound("A", 0.0, 1.0, 3), TireCompound("B", 2.0, 0.0, 6)};
    }
};

std::vector<boxbox::RaceEvent> drainAll(boxbox::EventPriorityQueue& queue) {
    std::vector<boxbox::RaceEvent> out;
    while (!queue.empty()) out.push_back(queue.pop());
    return out;
}

void testReviewFindings() {
    beginSection("Regressions from the code review: one-lap stints, sink changes, sector counts, report input");

    // 1. A one-lap stint still gets its compound. Plan A1 B2 A1 B2: the stops after laps 1, 3 and 4
    //    close stints of 1, 2 and 1 laps, and a one-lap stint's LapCompleted arrives after the PitEntry
    //    that closes it.
    const ShortRace s;
    const PitStrategy strategy(s.track, s.compounds);
    boxbox::EventPriorityQueue queue;
    boxbox::RaceSimulator sim(s.track, s.compounds);
    sim.setEventSink(&queue);
    sim.addDriver(7, "ONES", strategy.evaluatePlan({PlannedStint{0, 1}, PlannedStint{1, 2},
                                                    PlannedStint{0, 1}, PlannedStint{1, 2}}));
    sim.runToFinish();
    boxbox::EventLog log;
    for (const boxbox::RaceEvent& e : drainAll(queue)) log.record(e);
    const boxbox::DriverPitSummary summary = boxbox::PitAnalytics(log).summaryFor(7);
    XCHECK((summary.stintLengthsLaps == std::vector<int>{1, 2, 1, 2}));
    XCHECK((summary.compoundIndicesUsed == std::vector<int>{0, 1, 0, 1}));
    XCHECK((summary.stopLaps == std::vector<int>{1, 3, 4}));

    // Free stops make every stint one lap; every stint must still carry its compound.
    Track free("Free", 12, 0.0);
    free.addSector("S1", 90.0);
    const PitStrategy freeStrategy(free, boxbox::defaultCompounds());
    boxbox::EventPriorityQueue freeQueue;
    boxbox::RaceSimulator freeSim(free, boxbox::defaultCompounds());
    freeSim.setEventSink(&freeQueue);
    freeSim.addDriver(1, "FREE", freeStrategy.optimize());
    freeSim.runToFinish();
    boxbox::EventLog freeLog;
    for (const boxbox::RaceEvent& e : drainAll(freeQueue)) freeLog.record(e);
    const boxbox::DriverPitSummary freeSummary = boxbox::PitAnalytics(freeLog).summaryFor(1);
    XCHECK(freeSummary.stintLengthsLaps.size() == freeSim.drivers().front().plan().size());
    XCHECK(freeSummary.compoundIndicesUsed.size() == freeSummary.stintLengthsLaps.size());
    bool compoundsRight = true;
    for (std::size_t i = 0; i < freeSummary.compoundIndicesUsed.size(); ++i) {
        compoundsRight = compoundsRight && freeSummary.compoundIndicesUsed[i] ==
                                               static_cast<int>(freeSim.drivers().front().plan()[i].compoundIndex);
    }
    XCHECK(compoundsRight);

    // 2. Changing the sink mid-race reseeds the position baseline: the first PositionChange after
    //    the change starts from the standings at that moment, not from an older round.
    boxbox::OvertakeRule busy;
    busy.enabled = true;
    busy.minGap = 5.0;
    busy.passMargin = 0.0;
    Track longRace("Long", 30, 21.0);
    longRace.addSector("S1", 90.0);
    const std::vector<TireCompound> three = boxbox::defaultCompounds();
    const PitStrategy longStrategy(longRace, three);
    boxbox::RaceSimulator moving(longRace, three);
    moving.setOvertakeRule(busy);
    moving.addDriver(1, "OPT", longStrategy.optimize());
    for (std::size_t c = 0; c < three.size(); ++c) {
        moving.addDriver(static_cast<int>(c) + 2, "ALL", longStrategy.singleCompoundPlan(c));
    }
    boxbox::EventPriorityQueue first;
    moving.setEventSink(&first);
    moving.advanceLap();
    moving.advanceLap();
    moving.setEventSink(nullptr);
    for (int i = 0; i < 12; ++i) moving.advanceLap();      // positions move on with no sink
    std::vector<int> before(moving.drivers().size(), 0);
    for (const boxbox::Standing& row : moving.standings()) before[row.driverIndex] = row.position;
    boxbox::EventPriorityQueue second;
    moving.setEventSink(&second);
    moving.runToFinish();
    std::vector<int> lastSeen = before;
    bool chained = true;
    for (const boxbox::RaceEvent& e : drainAll(second)) {
        if (e.type != boxbox::RaceEventType::PositionChange) continue;
        std::size_t index = 0;
        while (moving.drivers()[index].number() != e.driverNumber) ++index;
        chained = chained && e.oldPosition == lastSeen[index];
        lastSeen[index] = e.newPosition;
    }
    XCHECK(chained);

    // Adding a car after the baseline was seeded must not read past the baseline.
    boxbox::EventPriorityQueue grow;
    boxbox::RaceSimulator late(longRace, three);
    late.setEventSink(&grow);
    late.addDriver(1, "ONE", longStrategy.optimize());
    late.addDriver(2, "TWO", longStrategy.singleCompoundPlan(0));
    late.advanceLap();
    late.addDriver(3, "LATE", longStrategy.singleCompoundPlan(1));
    late.advanceLap();      // would index lastPositions_ out of range if it were not reseeded
    XCHECK(late.lapsRun() == 2);

    // 3. Records with different sector counts across cars are refused (the field-wide queries
    //    assume one count), even though each car on its own is consistent.
    std::vector<boxbox::LapRecord> uneven{record(1, 1, 30.0, 0, {10.0, 20.0}),
                                          record(2, 1, 60.0, 0, {10.0, 20.0, 30.0})};
    uneven[1].position = 2;
    XCHECK_THROWS(boxbox::DriverAnalytics({1, 2}, uneven), std::invalid_argument);

    // 4. A last sector of base time 0 leaves the remainder at 0, never a hair below it.
    Track zeroLast("ZL", 3, 5.0);
    zeroLast.addSector("S1", 30.0);
    zeroLast.addSector("S2", 40.0);
    zeroLast.addSector("S3", 0.0);
    bool nonNegative = true;
    for (int i = 0; i < 2000; ++i) {
        const double lap = 60.0 + 0.0137 * static_cast<double>(i);
        for (const double part : boxbox::modelledSectorTimes(zeroLast, lap)) {
            nonNegative = nonNegative && part >= 0.0;
        }
    }
    XCHECK(nonNegative);
    const std::vector<TireCompound> slow{TireCompound("A", 1.0, 0.1, 3)};
    const PitStrategy zlStrategy(zeroLast, slow);
    boxbox::RaceSimulator zlSim(zeroLast, slow);
    zlSim.addDriver(1, "ONE", zlStrategy.optimize());
    boxbox::LapRecorder zlRecorder(zlSim);
    while (zlSim.advanceLap()) zlRecorder.observe(zlSim);
    const boxbox::DriverAnalytics zlAnalytics(zlRecorder);     // the recorder's own records are accepted
    XCHECK(zlAnalytics.summaries().front().laps == 3);

    // 5. Pit-report input: bad options are refused, a stop past the race distance does not throw,
    //    and the caller's stream formatting survives.
    const std::vector<TireCompound> compounds{TireCompound("A", 0.0, 1.0, 5)};
    boxbox::EventLog damaged;
    boxbox::RaceEvent stop;
    stop.type = boxbox::RaceEventType::PitEntry;
    stop.driverNumber = 1;
    stop.lap = 40;                         // beyond the 10-lap race below
    stop.timestamp = 900.0;
    damaged.record(stop);
    boxbox::PitReportInput in;
    in.log = &damaged;
    in.compounds = &compounds;
    in.pitLoss = 5.0;
    in.raceLaps = 10;
    std::ostringstream out;
    out.precision(7);
    const std::ios_base::fmtflags flags = out.flags();
    boxbox::printPitReport(out, in);
    XCHECK(has(out.str(), "Fewest pit calls"));
    XCHECK(out.flags() == flags && out.precision() == 7);

    std::ostringstream sink;
    boxbox::PitReportInput bad = in;
    bad.callWindow = -1;
    XCHECK_THROWS(boxbox::printPitReport(sink, bad), std::invalid_argument);
    bad = in;
    bad.trafficLapsAfter = 0;
    XCHECK_THROWS(boxbox::printPitReport(sink, bad), std::invalid_argument);
    bad = in;
    bad.histogramBinLaps = 0;
    XCHECK_THROWS(boxbox::printPitReport(sink, bad), std::invalid_argument);

    // 6. Huge arguments do not overflow.
    XCHECK(boxbox::PitAnalytics(damaged).stintHistogram(2000000000).size() == 1U);
    std::vector<boxbox::LapRecord> laps{record(1, 1, 90.0, 0, {})};
    laps[0].pitStop = true;
    XCHECK(boxbox::PitAnalytics::trafficAfterStops(laps, 2147483647).size() == 1U);
}

void testPitReportInputs() {
    beginSection("Pit report: partial logs, no race distance, and no stops");

    const std::vector<TireCompound> compounds{TireCompound("A", 0.0, 1.0, 5)};

    // A stop with no compound information: the stint is listed, its compound is "unknown".
    boxbox::EventLog log;
    boxbox::RaceEvent stop;
    stop.type = boxbox::RaceEventType::PitEntry;
    stop.driverNumber = 3;
    stop.lap = 4;
    stop.timestamp = 120.0;
    log.record(stop);
    boxbox::PitReportInput in;
    in.log = &log;
    in.compounds = &compounds;
    in.pitLoss = 5.0;
    in.raceLaps = 0;            // no distance given: the call windows are not clamped to a race
    std::ostringstream out;
    boxbox::printPitReport(out, in);
    XCHECK(has(out.str(), "unknown compound"));
    XCHECK(has(out.str(), "Fewest pit calls"));
    XCHECK(has(out.str(), "Stops in order:"));

    // A compound change to an index the race does not have is named by its index.
    boxbox::EventLog foreign;
    stop.driverNumber = 3;
    foreign.record(stop);
    boxbox::RaceEvent change;
    change.type = boxbox::RaceEventType::CompoundChange;
    change.driverNumber = 3;
    change.lap = 4;
    change.timestamp = 120.0;
    change.oldCompoundIndex = 0;
    change.newCompoundIndex = 9;
    foreign.record(change);
    in.log = &foreign;
    std::ostringstream foreignOut;
    boxbox::printPitReport(foreignOut, in);
    XCHECK(has(foreignOut.str(), "A -> compound index 9"));

    // Events but no stops: "none" under the order of stops.
    boxbox::EventLog quiet;
    boxbox::RaceEvent lap;
    lap.type = boxbox::RaceEventType::LapCompleted;
    lap.driverNumber = 1;
    lap.lap = 1;
    lap.timestamp = 90.0;
    quiet.record(lap);
    in.log = &quiet;
    in.raceLaps = 1;
    std::ostringstream none;
    boxbox::printPitReport(none, in);
    XCHECK(has(none.str(), "    none\n"));
    XCHECK(has(none.str(), "no pit stops recorded"));

    // With lap records but no stop among them, the traffic-after-stop section says none too.
    std::vector<boxbox::LapRecord> noStops{record(1, 1, 90.0, 0, {})};
    in.records = &noStops;
    std::ostringstream withRecords;
    boxbox::printPitReport(withRecords, in);
    XCHECK(has(withRecords.str(), "Traffic lost in the 3 lap(s) after each stop:\n    none\n"));
}

void testPitSortTieBreaks() {
    beginSection("PitAnalytics sort tie-breaks: equal times and equal first laps");

    // Two stops by one car at the same instant: ordered by lap.
    boxbox::EventLog log;
    for (const int lap : {5, 3}) {
        boxbox::RaceEvent e;
        e.type = boxbox::RaceEventType::PitEntry;
        e.driverNumber = 2;
        e.lap = lap;
        e.timestamp = 100.0;
        log.record(e);
    }
    const boxbox::PitAnalytics analytics(log);
    const std::vector<boxbox::PitStopRecord> stops = analytics.chronologicalPitStops();
    XCHECK(stops.size() == 2U && stops[0].lap == 3 && stops[1].lap == 5);

    // Coverage check with two stints that begin on the same lap: an overlap, found either way round.
    const boxbox::CoverageReport report =
        boxbox::PitAnalytics::checkStintCoverage({{1, 4}, {1, 3}}, 4);
    XCHECK(!report.tiles && has(report.message, "overlaps"));
}

void testStrategyAndTireCliEdges() {
    beginSection("CLI edges: undercut with an illegal first stint, reversed ranges, odd tire profiles");

    const Track track = handTrack(4);
    const std::vector<TireCompound> compounds{TireCompound("A", 0.0, 1.0, 2), TireCompound("B", 2.0, 0.0, 4)};
    const PitStrategy hand(track, compounds);
    std::ostringstream out;
    std::ostringstream err;

    // Stopping after lap 3 needs a 3-lap first stint, but A allows 2: reported, not computed.
    int code = boxbox::runStrategyCommand({"undercut", "--earlier-lap", "1", "--later-lap", "3"}, hand, out, err);
    XCHECK(code == boxbox::kStrategyOk);
    XCHECK(has(out.str(), "infeasible (the first stint is longer than its compound allows)"));
    XCHECK(has(out.str(), "No comparison"));

    // A range that ends before it starts, on a lap that does follow the previous stint.
    out.str("");
    err.str("");
    code = boxbox::runStrategyCommand({"evaluate", "--plan", "A:1,B:2-1"}, hand, out, err);
    XCHECK(code == boxbox::kStrategyUsageError && has(err.str(), "ends before it starts"));

    // Tire profiles of every cliff shape.
    Track long50("Long", 50, 5.0);
    long50.addSector("S1", 90.0);
    const std::vector<TireCompound> shapes{
        TireCompound("NOCLIFF", 0.0, 0.05, 10),
        TireCompound("ZERORATE", 0.0, 0.05, 10, 3, 0.0),
        TireCompound("BEYOND", 0.0, 0.05, 10, 20, 0.5),
        TireCompound("REAL", 0.0, 0.05, 10, 3, 0.5)};
    const PitStrategy tires(long50, shapes);
    out.str("");
    err.str("");
    code = boxbox::runTiresCommand({"profile"}, tires, out, err);
    XCHECK(code == boxbox::kTiresOk);
    XCHECK(has(out.str(), "Cliff: none"));
    XCHECK(has(out.str(), "so it adds nothing"));
    XCHECK(has(out.str(), "never reached inside the legal stint"));
    XCHECK(has(out.str(), "Cliff: from tire age 4"));

    // Threshold outcomes with nothing to report, and a compound compared with itself.
    out.str("");
    code = boxbox::runTiresCommand({"threshold", "--compound", "REAL", "--max-degradation", "1000"}, tires, out, err);
    XCHECK(code == boxbox::kTiresOk && has(out.str(), "none within the legal stint of 10 laps"));
    out.str("");
    code = boxbox::runTiresCommand({"threshold", "--compound", "REAL", "--max-average", "1"}, tires, out, err);
    XCHECK(code == boxbox::kTiresOk && has(out.str(), "none, even a 1-lap stint is slower"));
    out.str("");
    code = boxbox::runTiresCommand({"threshold", "--compound", "REAL", "--slower-than", "REAL"}, tires, out, err);
    XCHECK(code == boxbox::kTiresOk && has(out.str(), "never"));

    // Argument errors only the tires parser can raise.
    const std::vector<std::vector<std::string>> bad = {
        {"profile", "--compound", "REAL", "--compound", "NOCLIFF"},          // an option given twice
        {"profile", "--override", "1=2"},                                    // --override outside range
        {"range", "--compound", "REAL", "--from", "-1", "--to", "3"},        // negative age
        {"range", "--compound", "REAL", "--from", "0", "--to", "3", "--override", "3"},  // no '='
    };
    for (const std::vector<std::string>& args : bad) {
        out.str("");
        err.str("");
        XCHECK(boxbox::runTiresCommand(args, tires, out, err) == boxbox::kTiresUsageError);
        XCHECK(!err.str().empty());
    }
}

}  // namespace

EdgeTestTally runEdgeTests() {
    g_tally = EdgeTestTally();

    testNonFiniteArguments();
    testPlanCoverageAndGuards();
    testPartialSectorsAndOddReports();
    testRecorderFieldChanges();
    testRecorderOnAnotherRace();
    testReviewFindings();
    testPitReportInputs();
    testPitSortTieBreaks();
    testStrategyAndTireCliEdges();

    return g_tally;
}
