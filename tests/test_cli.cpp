// Box Box: checks for the CLI command handlers and report printers.
//
// The handlers take std::ostream, so they run in-process with string streams:
// exit codes, key lines, error paths, "nothing printed before the input was
// accepted", determinism, and a seeded fuzz of random argument lists that must
// only ever come back with a documented exit status. Same harness style as
// test_main.cpp.

#include "CliTests.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "DriverReport.hpp"
#include "PitReport.hpp"
#include "RaceLapReport.hpp"
#include "StrategyCommands.hpp"
#include "TireCommands.hpp"
#include "boxbox/Common.hpp"
#include "boxbox/DriverAnalytics.hpp"
#include "boxbox/EventLog.hpp"
#include "boxbox/EventPriorityQueue.hpp"
#include "boxbox/PitStrategy.hpp"
#include "boxbox/RaceLapAnalytics.hpp"
#include "boxbox/RaceSimulator.hpp"
#include "boxbox/TireCompound.hpp"
#include "boxbox/Track.hpp"

namespace {

using boxbox::PitStrategy;
using boxbox::TireCompound;
using boxbox::Track;

// ---------------------------------------------------------------------------
// Harness
// ---------------------------------------------------------------------------

CliTestTally g_tally;
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

#define KCHECK(expr) reportCheck((expr), #expr, __FILE__, __LINE__, std::string())
#define KCHECK_MSG(expr, msg) reportCheck((expr), #expr, __FILE__, __LINE__, (msg))

#define KCHECK_THROWS(statement, ExType)                                                \
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

class Lcg {
public:
    explicit Lcg(std::uint64_t seed) noexcept : state_(seed) {}
    std::uint64_t next() noexcept {
        state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
        return state_ >> 33U;
    }
    std::size_t index(std::size_t n) noexcept {
        return static_cast<std::size_t>(next() % static_cast<std::uint64_t>(n));
    }

private:
    std::uint64_t state_;
};

bool has(const std::string& text, const std::string& fragment) {
    return text.find(fragment) != std::string::npos;
}

struct Run {
    int code = -1;
    std::string out;
    std::string err;
};

using Handler = int (*)(const std::vector<std::string>&, const PitStrategy&, std::ostream&,
                        std::ostream&);

Run run(Handler handler, const PitStrategy& strategy, const std::vector<std::string>& args) {
    std::ostringstream out;
    std::ostringstream err;
    Run r;
    r.code = handler(args, strategy, out, err);
    r.out = out.str();
    r.err = err.str();
    return r;
}

Run strategyRun(const PitStrategy& s, const std::vector<std::string>& args) {
    return run(boxbox::runStrategyCommand, s, args);
}
Run tiresRun(const PitStrategy& s, const std::vector<std::string>& args) {
    return run(boxbox::runTiresCommand, s, args);
}

/// The 4-lap hand race: one 10 s sector, 5 s pit loss, A (max 2 laps) and B (max 4).
struct Hand {
    Track track{"Hand", 4, 5.0};
    std::vector<TireCompound> compounds;
    Hand() {
        track.addSector("S1", 10.0);
        compounds = {TireCompound("A", 0.0, 1.0, 2), TireCompound("B", 2.0, 0.0, 4)};
    }
};

/// A 30-lap race on the default compounds and three sectors.
struct Race {
    Track track{"Race", 30, 21.0};
    std::vector<TireCompound> compounds = boxbox::defaultCompounds();
    Race() {
        track.addSector("S1", 28.4);
        track.addSector("S2", 31.2);
        track.addSector("S3", 30.4);
    }
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

void testStrategyCommands() {
    beginSection("strategy command group: output, exit codes and error paths");

    const Hand h;
    const PitStrategy hand(h.track, h.compounds);

    Run r = strategyRun(hand, {});
    KCHECK(r.code == boxbox::kStrategyOk && has(r.out, "Usage: boxbox"));
    KCHECK(strategyRun(hand, {"help"}).code == boxbox::kStrategyOk);
    KCHECK(strategyRun(hand, {"--help"}).code == boxbox::kStrategyOk);

    // evaluate: A2 + B2 = 10 + 11 + 12 + 12 + 5 = 50, 3 s over the 47 s optimum.
    r = strategyRun(hand, {"evaluate", "--plan", "A:2,B:2"});
    KCHECK(r.code == boxbox::kStrategyOk);
    KCHECK(has(r.out, "50.000") && has(r.out, "47.000") && has(r.out, "+3.000"));
    KCHECK(has(r.out, "simplified model"));
    KCHECK(r.err.empty());
    KCHECK(strategyRun(hand, {"evaluate", "--plan", "A:2,B:2"}).out == r.out);   // deterministic
    r = strategyRun(hand, {"evaluate", "--plan", "a:1-2,b:3-4"});                // case, ranges
    KCHECK(r.code == boxbox::kStrategyOk && has(r.out, "50.000"));

    // An infeasible plan the user entered: exit 2, the reason on stdout.
    r = strategyRun(hand, {"evaluate", "--plan", "A:3,B:1"});
    KCHECK(r.code == boxbox::kStrategyInfeasible && has(r.out, "INFEASIBLE") &&
           has(r.out, "maximum stint"));
    r = strategyRun(hand, {"evaluate", "--plan", "A:2"});
    KCHECK(r.code == boxbox::kStrategyInfeasible && has(r.out, "cover 2 laps"));

    // Usage errors: exit 1, a message on stderr, nothing on stdout.
    const std::vector<std::vector<std::string>> bad = {
        {"evaluate"},
        {"evaluate", "--plan", "A:2,B:2", "--plan", "A:4"},
        {"evaluate", "--plan", "Z:4"},
        {"evaluate", "--plan", "A:two"},
        {"evaluate", "--plan", "A"},
        {"evaluate", "--plan", ""},
        {"evaluate", "--plan", "A:1-2,B:4-4"},       // gap
        {"evaluate", "--plan", "A:1-2,B:2-4"},       // overlap
        {"evaluate", "--plan", "A:3-1"},              // reversed
        {"evaluate", "--plan", "A:1-2-3"},
        {"evaluate", "A:4"},                          // not an option
        {"evaluate", "--plan"},                       // missing value
        {"evaluate", "--bogus", "1", "--plan", "A:4"},
        {"alternatives", "--count", "0"},
        {"alternatives", "--count", "51"},
        {"alternatives", "--count", "x"},
        {"alternatives", "--count", "3", "--count", "4"},
        {"alternatives", "--plan", "A:4"},
        {"sensitivity"},
        {"sensitivity", "--pit-loss", "0:10"},
        {"sensitivity", "--pit-loss", "0:10:0"},
        {"sensitivity", "--pit-loss", "-5:10:1"},
        {"sensitivity", "--pit-loss", "0:nan:1"},
        {"sensitivity", "--pit-loss", "0:inf:1"},
        {"sensitivity", "--pit-loss", "10:0:1"},
        {"sensitivity", "--pit-loss", "0:100000:1"},
        {"sensitivity", "--pit-loss", "0:10:1", "--compound", "A"},
        {"sensitivity", "--pit-loss", "0:10:1", "--param", "pace"},
        {"sensitivity", "--compound", "A", "--param", "bogus", "--range", "0:1:1"},
        {"sensitivity", "--compound", "A", "--param", "pace"},
        {"sensitivity", "--compound", "Z", "--param", "pace", "--range", "0:1:1"},
        {"sensitivity", "--compound", "A", "--param", "max-stint", "--range", "1.5:3:1"},
        {"sensitivity", "--compound", "A", "--param", "degradation", "--range", "-1:1:1"},
        {"stops", "--max", "-1"},
        {"stops", "--max", "99"},
        {"stops", "--max", "x"},
        {"traffic", "--min-gap", "0"},
        {"traffic", "--min-gap", "nan"},
        {"traffic", "--pass-margin", "-1"},
        {"undercut"},
        {"undercut", "--earlier-lap", "2"},
        {"undercut", "--earlier-lap", "3", "--later-lap", "2"},
        {"undercut", "--earlier-lap", "1", "--later-lap", "4"},
        {"undercut", "--earlier-lap", "0", "--later-lap", "2"},
        {"nope"},
    };
    for (const std::vector<std::string>& args : bad) {
        r = strategyRun(hand, args);
        KCHECK_MSG(r.code == boxbox::kStrategyUsageError, args.front());
        KCHECK_MSG(r.out.empty(), "stdout must be empty for: " + args.front());
        KCHECK_MSG(has(r.err, "boxbox: strategy:"), args.front());
    }

    // alternatives: 47, 48, then two plans at 50.
    r = strategyRun(hand, {"alternatives", "--count", "4"});
    KCHECK(r.code == boxbox::kStrategyOk);
    KCHECK(has(r.out, "47.000") && has(r.out, "48.000") && has(r.out, "A:2 x2") && has(r.out, "B:4"));
    KCHECK(has(r.out, "exact up to that bound"));
    r = strategyRun(hand, {"alternatives", "--count", "50"});
    KCHECK(r.code == boxbox::kStrategyOk);

    // compare: standard set plus a user plan; an infeasible user plan gives exit 2 but is shown.
    r = strategyRun(hand, {"compare", "--plan", "A:2,B:2"});
    KCHECK(r.code == boxbox::kStrategyOk && has(r.out, "custom 1") && has(r.out, "optimal"));
    r = strategyRun(hand, {"compare", "--plan", "B:5"});
    KCHECK(r.code == boxbox::kStrategyInfeasible && has(r.out, "infeasible"));
    KCHECK(strategyRun(hand, {"compare", "--max-stops", "1"}).code == boxbox::kStrategyOk);

    // sensitivity.
    r = strategyRun(hand, {"sensitivity", "--pit-loss", "0:20:10"});
    KCHECK(r.code == boxbox::kStrategyOk && has(r.out, "Pit-loss sweep") && has(r.out, "pit loss (s)"));
    r = strategyRun(hand, {"sensitivity", "--compound", "a", "--param", "degradation", "--range", "0:2:1"});
    KCHECK(r.code == boxbox::kStrategyOk && has(r.out, "Compound sweep"));
    for (const char* param : {"pace", "max-stint", "cliff-lap", "cliff-deg"}) {
        const char* range = (std::string(param) == "max-stint" || std::string(param) == "cliff-lap")
                                ? "1:2:1" : "0:1:1";
        KCHECK_MSG(strategyRun(hand, {"sensitivity", "--compound", "B", "--param", param, "--range", range})
                           .code == boxbox::kStrategyOk, param);
    }

    // stops: includes the infeasible counts, with a reason.
    r = strategyRun(hand, {"stops", "--max", "4"});
    KCHECK(r.code == boxbox::kStrategyOk && has(r.out, "0 stop(s):") && has(r.out, "infeasible"));
    KCHECK(strategyRun(hand, {"stops"}).code == boxbox::kStrategyOk);

    // traffic and undercut.
    r = strategyRun(hand, {"traffic"});
    KCHECK(r.code == boxbox::kStrategyOk && has(r.out, "clean (s)"));
    r = strategyRun(hand, {"undercut", "--earlier-lap", "1", "--later-lap", "2"});
    KCHECK(r.code == boxbox::kStrategyOk && has(r.out, "Earlier stop") && has(r.out, "Later stop"));

    // The default race: byte-identical across runs, and every command exits 0.
    const Race race;
    const PitStrategy big(race.track, race.compounds);
    const std::vector<std::vector<std::string>> good = {
        {"evaluate", "--plan", "SOFT:10,MEDIUM:20"},
        {"compare"},
        {"alternatives", "--count", "6"},
        {"sensitivity", "--pit-loss", "0:30:10"},
        {"sensitivity", "--compound", "SOFT", "--param", "degradation", "--range", "0.05:0.25:0.1"},
        {"stops", "--max", "3"},
        {"traffic"},
        {"undercut", "--earlier-lap", "10", "--later-lap", "13"},
    };
    for (const std::vector<std::string>& args : good) {
        const Run a = strategyRun(big, args);
        const Run b = strategyRun(big, args);
        KCHECK_MSG(a.code == boxbox::kStrategyOk, args.front());
        KCHECK_MSG(a.out == b.out && a.err == b.err, "deterministic: " + args.front());
        KCHECK_MSG(!a.out.empty() && a.err.empty(), args.front());
    }

    // Degenerate races: 0 laps and 1 lap never crash.
    Track none("None", 0, 5.0);
    none.addSector("S1", 10.0);
    const PitStrategy zero(none, h.compounds);
    for (const std::vector<std::string>& args : std::vector<std::vector<std::string>>{
             {"compare"}, {"alternatives"}, {"stops"}, {"traffic"},
             {"sensitivity", "--pit-loss", "0:10:5"}}) {
        KCHECK_MSG(strategyRun(zero, args).code == boxbox::kStrategyOk, args.front());
    }
    Track one("One", 1, 5.0);
    one.addSector("S1", 10.0);
    const PitStrategy single(one, h.compounds);
    for (const std::vector<std::string>& args : std::vector<std::vector<std::string>>{
             {"compare"}, {"alternatives", "--count", "3"}, {"stops", "--max", "1"}, {"traffic"}}) {
        KCHECK_MSG(strategyRun(single, args).code == boxbox::kStrategyOk, args.front());
    }
}

void testTiresCommands() {
    beginSection("tires command group: output, exit codes and error paths");

    const Race race;
    const PitStrategy strategy(race.track, race.compounds);

    Run r = tiresRun(strategy, {"help"});
    KCHECK(r.code == boxbox::kTiresOk && has(r.out, "Usage: boxbox"));

    r = tiresRun(strategy, {"profile", "--compound", "SOFT"});
    KCHECK(r.code == boxbox::kTiresOk);
    KCHECK(has(r.out, "Compound: SOFT") && has(r.out, "cumulative (s)") && has(r.out, "<- cliff"));
    KCHECK(has(r.out, "Lap time jumps"));
    KCHECK(tiresRun(strategy, {"profile"}).code == boxbox::kTiresOk);     // every compound

    r = tiresRun(strategy, {"range", "--compound", "SOFT", "--from", "0", "--to", "4"});
    KCHECK(r.code == boxbox::kTiresOk && !r.out.empty());
    r = tiresRun(strategy, {"range", "--compound", "SOFT", "--from", "0", "--to", "4", "--override",
                            "2=95.5"});
    KCHECK(r.code == boxbox::kTiresOk);

    r = tiresRun(strategy, {"threshold", "--compound", "SOFT", "--max-degradation", "0.5"});
    KCHECK(r.code == boxbox::kTiresOk && has(r.out, "first tire age"));
    r = tiresRun(strategy, {"threshold", "--compound", "SOFT", "--max-average", "89.9"});
    KCHECK(r.code == boxbox::kTiresOk && has(r.out, "longest stint"));
    r = tiresRun(strategy, {"threshold", "--compound", "SOFT", "--slower-than", "HARD"});
    KCHECK(r.code == boxbox::kTiresOk && has(r.out, "first age"));
    r = tiresRun(strategy, {"threshold", "--compound", "SOFT", "--max-degradation", "1.0",
                            "--max-average", "90.0"});
    KCHECK(r.code == boxbox::kTiresOk && has(r.out, "stint lengths averaging") &&
           has(r.out, "legal lengths"));
    r = tiresRun(strategy, {"threshold", "--compound", "SOFT", "--max-degradation", "0.0",
                            "--max-average", "0.0"});
    KCHECK(r.code == boxbox::kTiresOk && has(r.out, "none"));

    const std::vector<std::vector<std::string>> bad = {
        {"nope"},
        {"profile", "--compound", "ZZZ"},
        {"range", "--compound", "SOFT"},
        {"range", "--compound", "SOFT", "--from", "5", "--to", "2"},
        {"range", "--compound", "SOFT", "--from", "0", "--to", "999"},
        {"range", "--compound", "SOFT", "--from", "x", "--to", "2"},
        {"range", "--compound", "SOFT", "--from", "0", "--to", "3", "--override", "99=1"},
        {"range", "--compound", "SOFT", "--from", "0", "--to", "3", "--override", "1=nan"},
        {"threshold", "--compound", "SOFT"},
        {"threshold", "--compound", "SOFT", "--max-degradation", "nan"},
        {"threshold", "--compound", "SOFT", "--max-degradation", "1", "--slower-than", "HARD"},
        {"threshold", "--compound", "SOFT", "--max-average", "inf"},
        {"threshold", "--compound", "SOFT", "--slower-than", "ZZZ"},
        {"threshold", "--max-average", "90"},
    };
    for (const std::vector<std::string>& args : bad) {
        r = tiresRun(strategy, args);
        KCHECK_MSG(r.code == boxbox::kTiresUsageError, args.front());
        KCHECK_MSG(r.out.empty() || args.front() == "nope", "stdout for: " + args.front());
        KCHECK_MSG(!r.err.empty(), args.front());
    }
}

/// A race with a recorder, an event log and analytics, for the report printers.
struct Observed {
    Track track{"Obs", 24, 21.0};
    std::vector<TireCompound> compounds;
    boxbox::EventPriorityQueue queue;
    boxbox::EventLog log;
    boxbox::RaceSimulator sim;

    // A track with no sectors has a base lap time of 0, so its compounds must carry the whole lap
    // themselves (a negative lap time would be rejected by the event queue).
    explicit Observed(bool traffic, bool sectors = true)
        : track(makeTrack(sectors)),
          compounds(sectors ? boxbox::defaultCompounds() : bareCompounds()),
          sim(track, compounds) {
        const PitStrategy strategy(track, compounds);
        sim.setEventSink(&queue);
        if (traffic) {
            sim.setOvertakeRule(boxbox::OvertakeRule{true, 4.0, 10.0});
        }
        sim.addDriver(1, "OPTIMAL", strategy.optimize());
        for (std::size_t c = 0; c < compounds.size(); ++c) {
            sim.addDriver(static_cast<int>(c) + 2, "ALL-" + compounds[c].name(),
                          strategy.singleCompoundPlan(c));
        }
    }
    Observed(const Observed&) = delete;            // the simulator points at our own queue
    Observed& operator=(const Observed&) = delete;

    static std::vector<TireCompound> bareCompounds() {
        return {TireCompound("SOFT", 85.0, 0.09, 22, 16, 0.18), TireCompound("HARD", 86.0, 0.035, 45)};
    }

    static Track makeTrack(bool sectors) {
        Track t("Obs", 24, 21.0);
        if (sectors) {
            t.addSector("S1", 28.4);
            t.addSector("S2", 31.2);
            t.addSector("S3", 30.4);
        }
        return t;
    }
};

void testReportPrinters() {
    beginSection("report printers: pit, driver, sector and race-lap reports");

    Observed o(true);
    boxbox::LapRecorder recorder(o.sim);
    boxbox::RaceLapRecorder lapRecorder(o.sim);
    while (o.sim.advanceLap()) {
        recorder.observe(o.sim);
        lapRecorder.observe(o.sim);
    }
    while (!o.queue.empty()) o.log.record(o.queue.pop());
    const boxbox::DriverAnalytics analytics(recorder);

    // Pit report.
    boxbox::PitReportInput in;
    in.log = &o.log;
    in.compounds = &o.compounds;
    in.pitLoss = o.track.pitLoss();
    in.raceLaps = o.track.totalLaps();
    in.records = &recorder.records();
    std::ostringstream pit;
    boxbox::printPitReport(pit, in);
    const std::string report = pit.str();
    KCHECK(has(report, "=== Pit Report ===") && has(report, "Driver 1"));
    KCHECK(has(report, "Stint coverage: every car's stints tile laps 1-24"));
    KCHECK(has(report, "Stint length distribution") && has(report, "Stops in order:"));
    KCHECK(has(report, "Pit-lane time lost:") && has(report, "no separate entry, service or exit"));
    KCHECK(has(report, "Fewest pit calls") && has(report, "Traffic lost in the 3 lap(s)"));
    KCHECK(!has(report, "PROBLEM"));

    in.windowFrom = 5;
    in.windowTo = 15;
    std::ostringstream windowed;
    boxbox::printPitReport(windowed, in);
    KCHECK(has(windowed.str(), "Pit window laps 5-15") && has(windowed.str(), "prefix-sum count"));
    std::ostringstream again;
    boxbox::printPitReport(again, in);
    KCHECK(again.str() == windowed.str());                          // deterministic

    boxbox::PitReportInput noRecords = in;
    noRecords.records = nullptr;
    std::ostringstream plain;
    boxbox::printPitReport(plain, noRecords);
    KCHECK(!has(plain.str(), "Traffic lost in the"));

    boxbox::PitReportInput broken = in;
    broken.log = nullptr;
    std::ostringstream sink;
    KCHECK_THROWS(boxbox::printPitReport(sink, broken), std::invalid_argument);
    broken = in;
    broken.compounds = nullptr;
    KCHECK_THROWS(boxbox::printPitReport(sink, broken), std::invalid_argument);
    broken = in;
    broken.windowFrom = 9;
    broken.windowTo = 3;
    KCHECK_THROWS(boxbox::printPitReport(sink, broken), std::invalid_argument);
    boxbox::EventLog emptyLog;
    boxbox::PitReportInput empty = in;
    empty.log = &emptyLog;
    std::ostringstream none;
    boxbox::printPitReport(none, empty);
    KCHECK(has(none.str(), "No driver pit data recorded"));

    // A log whose stints do not tile the race: the coverage line says PROBLEM.
    boxbox::EventLog shortLog;
    boxbox::RaceEvent lap;
    lap.type = boxbox::RaceEventType::LapCompleted;
    lap.driverNumber = 7;
    lap.lap = 1;
    lap.timestamp = 90.0;
    shortLog.record(lap);
    boxbox::PitReportInput partial = in;
    partial.log = &shortLog;
    partial.windowFrom = 0;
    partial.records = nullptr;
    std::ostringstream partialOut;
    boxbox::printPitReport(partialOut, partial);
    KCHECK(has(partialOut.str(), "PROBLEM"));

    // Driver reports.
    std::ostringstream stats;
    boxbox::printDriverStats(stats, o.sim, analytics);
    KCHECK(has(stats.str(), "Driver statistics") && has(stats.str(), "OPTIMAL"));
    std::ostringstream top;
    boxbox::printTopLaps(top, o.sim, analytics, 3);
    KCHECK(has(top.str(), "Fastest 3 of") && has(top.str(), "Slowest 3 of") && has(top.str(), "Fastest lap on each compound"));
    boxbox::LapFilter car2;
    car2.driverNumber = 2;
    car2.firstLap = 3;
    car2.lastLap = 10;
    std::ostringstream filtered;
    boxbox::printTopLaps(filtered, o.sim, analytics, 2, car2);
    KCHECK(has(filtered.str(), "of car 2") && has(filtered.str(), "laps 3-10"));
    boxbox::LapFilter ghost;
    ghost.driverNumber = 99;
    KCHECK_THROWS(boxbox::printTopLaps(sink, o.sim, analytics, 2, ghost), std::invalid_argument);
    boxbox::LapFilter nobody;
    nobody.firstLap = 900;
    nobody.lastLap = 901;
    std::ostringstream noMatch;
    boxbox::printTopLaps(noMatch, o.sim, analytics, 2, nobody);
    KCHECK(has(noMatch.str(), "no laps match the filter"));
    for (const std::size_t window : {std::size_t{1}, std::size_t{5}, std::size_t{24}, std::size_t{99}}) {
        std::ostringstream windows;
        boxbox::printLapWindows(windows, analytics, window);
        KCHECK(has(windows.str(), "Rolling " + std::to_string(window) + "-lap windows"));
    }

    // Sector reports.
    std::ostringstream sectors;
    boxbox::printSectorStats(sectors, o.sim, analytics);
    KCHECK(has(sectors.str(), "MODELLED split") && has(sectors.str(), "Best sector in the field") &&
           has(sectors.str(), "Distribution over every lap"));
    std::ostringstream compare;
    boxbox::printSectorComparison(compare, o.sim, analytics, 1, 3);
    KCHECK(has(compare.str(), "car 1 against car 3") && has(compare.str(), "A - B"));
    KCHECK_THROWS(boxbox::printSectorComparison(sink, o.sim, analytics, 1, 99), std::invalid_argument);

    // A track with no sectors: the sector report says so rather than inventing data.
    Observed bare(false, false);
    boxbox::LapRecorder bareRecorder(bare.sim);
    while (bare.sim.advanceLap()) bareRecorder.observe(bare.sim);
    const boxbox::DriverAnalytics bareAnalytics(bareRecorder);
    std::ostringstream bareSectors;
    boxbox::printSectorStats(bareSectors, bare.sim, bareAnalytics);
    KCHECK(has(bareSectors.str(), "No sector data"));
    KCHECK_THROWS(boxbox::printSectorComparison(sink, bare.sim, bareAnalytics, 1, 2), std::invalid_argument);
    for (const boxbox::LapRecord& r : bareRecorder.records()) KCHECK(r.sectorTimes.empty());

    // Race-lap report.
    std::ostringstream laps;
    boxbox::printRaceLapReport(laps, o.sim, lapRecorder, 2, 12);
    KCHECK(has(laps.str(), "Race laps 2 - 12") && has(laps.str(), "traffic lost") &&
           has(laps.str(), "Position swaps"));
    KCHECK_THROWS(boxbox::printRaceLapReport(sink, o.sim, lapRecorder, 0, 5), std::out_of_range);
    KCHECK_THROWS(boxbox::printRaceLapReport(sink, o.sim, lapRecorder, 5, 2), std::out_of_range);
    KCHECK_THROWS(boxbox::printRaceLapReport(sink, o.sim, lapRecorder, 1, 25), std::out_of_range);
}

void testFuzz() {
    beginSection("Fuzz: random argument lists only ever return a documented exit status");

    const Race race;
    const PitStrategy strategy(race.track, race.compounds);

    const std::vector<std::string> strategyWords = {
        "evaluate", "compare", "alternatives", "sensitivity", "stops", "traffic", "undercut", "help",
        "nope", "--plan", "--count", "--max", "--max-stops", "--pit-loss", "--compound", "--param",
        "--range", "--min-gap", "--pass-margin", "--earlier-lap", "--later-lap", "SOFT:10,HARD:20",
        "SOFT:1-30", "SOFT:30", "MEDIUM:15,SOFT:15", "ZZZ:5", "a:b", ":", ",", "", "0", "1", "-1", "5",
        "12", "30", "31", "50", "0:30:10", "0:10:0", "-5:5:1", "0:nan:1", "0:inf:1", "nan", "inf", "-inf",
        "1e9", "abc", "SOFT", "HARD", "pace", "degradation", "max-stint", "cliff-lap", "cliff-deg",
        "0.05:0.25:0.1", "1:30:1", "--"};
    const std::vector<std::string> tiresWords = {
        "profile", "range", "threshold", "help", "nope", "--compound", "--from", "--to", "--override",
        "--max-degradation", "--max-average", "--slower-than", "SOFT", "HARD", "MEDIUM", "ZZZ", "0", "3",
        "10", "21", "22", "999", "-1", "nan", "inf", "1.5", "2=95.0", "3=nan", "99=1", "=", "abc", "",
        "90.0", "0.5", "--"};

    Lcg rng(20261110ULL);
    bool allDocumented = true;
    bool deterministic = true;
    int ok = 0;
    for (int trial = 0; trial < 1500; ++trial) {
        const bool useStrategy = (trial % 3 != 0);
        const std::vector<std::string>& words = useStrategy ? strategyWords : tiresWords;
        std::vector<std::string> args;
        const std::size_t count = rng.index(7);
        for (std::size_t i = 0; i < count; ++i) {
            args.push_back(words[rng.index(words.size())]);
        }
        Run a;
        try {
            a = useStrategy ? strategyRun(strategy, args) : tiresRun(strategy, args);
        } catch (...) {
            allDocumented = false;   // a handler must catch its own errors
            continue;
        }
        const bool documented = a.code == 0 || a.code == 1 || (useStrategy && a.code == 2);
        allDocumented = allDocumented && documented;
        if (a.code == 0) ++ok;
        if (a.code == 1) {
            // A rejected request prints its message to stderr and nothing to stdout.
            allDocumented = allDocumented && !a.err.empty() &&
                            (a.out.empty() || (!useStrategy && args.empty()));
        }
        if (trial % 10 == 0) {
            const Run b = useStrategy ? strategyRun(strategy, args) : tiresRun(strategy, args);
            deterministic = deterministic && a.code == b.code && a.out == b.out && a.err == b.err;
        }
    }
    KCHECK(allDocumented);
    KCHECK(deterministic);
    KCHECK(ok > 50);   // the fuzz is not just bouncing off the argument checker
}

}  // namespace

CliTestTally runCliTests() {
    g_tally = CliTestTally();

    testStrategyCommands();
    testTiresCommands();
    testReportPrinters();
    testFuzz();

    return g_tally;
}
