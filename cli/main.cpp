// Undercut: command-line frontend.
//
// This file is a thin frontend over the core library: it parses arguments,
// constructs core objects, calls into them and formats the result. It contains
// no strategy or simulation logic of its own, and the core has no awareness
// that it exists.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "undercut/Common.hpp"
#include "undercut/Driver.hpp"
#include "undercut/PitStrategy.hpp"
#include "undercut/RaceSimulator.hpp"
#include "undercut/TireCompound.hpp"
#include "undercut/Track.hpp"

namespace {

using undercut::Driver;
using undercut::PitStrategy;
using undercut::RaceSimulator;
using undercut::Standing;
using undercut::StrategyResult;
using undercut::Stint;
using undercut::TireCompound;
using undercut::Track;

constexpr int kExitOk = 0;
constexpr int kExitUsage = 1;
constexpr int kExitInfeasible = 2;
constexpr int kExitClosureFailed = 3;

// Rows of the stint-cost table printed by --show-table. Enough for any race a
// hand check would use, without flooding the terminal on a full-length run.
constexpr int kMaxTableRows = 60;

// ---------------------------------------------------------------------------
// Argument parsing helpers
// ---------------------------------------------------------------------------

double parseDouble(const std::string& text, const std::string& what) {
    std::size_t consumed = 0;
    double value = 0.0;
    bool ok = true;

    try {
        value = std::stod(text, &consumed);
    } catch (const std::exception&) {
        ok = false;
    }
    if (ok && consumed != text.size()) {
        ok = false;
    }
    if (!ok) {
        throw std::invalid_argument(what + ": expected a number, got \"" + text + "\"");
    }
    return value;
}

int parseInt(const std::string& text, const std::string& what) {
    std::size_t consumed = 0;
    long value = 0;
    bool ok = true;

    try {
        value = std::stol(text, &consumed, 10);
    } catch (const std::exception&) {
        ok = false;
    }
    if (ok && consumed != text.size()) {
        ok = false;
    }
    if (ok && (value < -1000000L || value > 1000000L)) {
        ok = false;
    }
    if (!ok) {
        throw std::invalid_argument(what + ": expected a whole number, got \"" + text + "\"");
    }
    return static_cast<int>(value);
}

std::vector<std::string> split(const std::string& text, char separator) {
    std::vector<std::string> parts;
    std::string current;
    for (const char ch : text) {
        if (ch == separator) {
            parts.push_back(current);
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    parts.push_back(current);
    return parts;
}

/// NAME:BASEPACE:DEG:MAXSTINT[:CLIFFLAP:CLIFFDEG]
TireCompound parseCompound(const std::string& spec) {
    const std::vector<std::string> parts = split(spec, ':');

    if (parts.size() != 4U && parts.size() != 6U) {
        throw std::invalid_argument(
            "--compound \"" + spec +
            "\": expected NAME:BASEPACE:DEG:MAXSTINT or "
            "NAME:BASEPACE:DEG:MAXSTINT:CLIFFLAP:CLIFFDEG");
    }
    if (parts[0].empty()) {
        throw std::invalid_argument("--compound \"" + spec + "\": name must not be empty");
    }

    const double basePace = parseDouble(parts[1], "--compound base pace");
    const double degradation = parseDouble(parts[2], "--compound degradation");
    const int maxStint = parseInt(parts[3], "--compound max stint");

    int cliffLap = -1;
    double cliffDegradation = 0.0;
    if (parts.size() == 6U) {
        cliffLap = parseInt(parts[4], "--compound cliff lap");
        cliffDegradation = parseDouble(parts[5], "--compound cliff degradation");
    }

    return TireCompound(parts[0], basePace, degradation, maxStint, cliffLap, cliffDegradation);
}

// ---------------------------------------------------------------------------
// Output formatting
// ---------------------------------------------------------------------------

std::string formatSeconds(double seconds, int precision = 3) {
    std::ostringstream out;
    if (!std::isfinite(seconds)) {
        return "-";
    }
    out << std::fixed << std::setprecision(precision) << seconds;
    return out.str();
}

/// Race-clock rendering: H:MM:SS.mmm, or M:SS.mmm under an hour.
std::string formatClock(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0.0) {
        return "-";
    }

    long long millis = std::llround(seconds * 1000.0);
    const long long hours = millis / 3600000LL;
    millis %= 3600000LL;
    const long long minutes = millis / 60000LL;
    millis %= 60000LL;
    const long long secs = millis / 1000LL;
    millis %= 1000LL;

    std::ostringstream out;
    out << std::setfill('0');
    if (hours > 0) {
        out << hours << ':' << std::setw(2) << minutes << ':';
    } else {
        out << minutes << ':';
    }
    out << std::setw(2) << secs << '.' << std::setw(3) << millis;
    return out.str();
}

std::string formatDelta(double value) {
    std::ostringstream out;
    out << std::scientific << std::setprecision(2) << value;
    return out.str();
}

void printUsage(std::ostream& out) {
    out <<
        "Undercut - F1 Pit-Stop & Race Strategy Scheduler\n"
        "\n"
        "Usage: undercut [options]\n"
        "\n"
        "Race setup:\n"
        "  --laps N                Race distance in laps. Default 50.\n"
        "  --pit-loss SECONDS      Time lost per pit stop. Default 22.0.\n"
        "  --track NAME            Circuit name. Default \"Generic Circuit\".\n"
        "  --sector SPEC           Add a sector, as SECONDS or NAME:SECONDS.\n"
        "                          Repeatable. The first use replaces the default\n"
        "                          three sectors 28.4 / 31.2 / 30.4 (90.0 s lap).\n"
        "\n"
        "Tires:\n"
        "  --compound SPEC         Add a compound. Repeatable. The first use\n"
        "                          replaces the default SOFT/MEDIUM/HARD set.\n"
        "                          SPEC is NAME:BASEPACE:DEG:MAXSTINT\n"
        "                          or      NAME:BASEPACE:DEG:MAXSTINT:CLIFFLAP:CLIFFDEG\n"
        "                          BASEPACE is a signed offset on the base lap time:\n"
        "                          negative is faster than the track reference lap.\n"
        "                          A negative CLIFFLAP disables the cliff.\n"
        "\n"
        "Simulation:\n"
        "  --simulate              Replay the race lap by lap, driving one car\n"
        "                          from the optimizer's own plan, and run the\n"
        "                          FR-8 loop-closure check.\n"
        "  --rivals                Add one reference car per compound, each on a\n"
        "                          fewest-stop single-compound plan. Implies\n"
        "                          --simulate. Shows how compounds diverge.\n"
        "  --lap-log N             Print the standings every N laps. Implies\n"
        "                          --simulate. 0 disables. Default 0.\n"
        "\n"
        "Output:\n"
        "  --show-table            Print the per-compound stint-cost table.\n"
        "  -h, --help              This message.\n"
        "\n"
        "Exit status:\n"
        "  0 success   1 bad usage   2 no feasible strategy   3 FR-8 check failed\n"
        "\n"
        "Examples:\n"
        "  undercut --laps 50 --pit-loss 22.0\n"
        "  undercut --laps 50 --rivals --lap-log 10\n"
        "  undercut --laps 4 --pit-loss 5 --sector 10 \\\n"
        "           --compound A:0:1:3 --compound B:2:0:4 --show-table\n";
}

void printSetup(const Track& track, const std::vector<TireCompound>& compounds) {
    std::cout << "Race setup\n";
    std::cout << "  Track       " << track.name() << '\n';
    std::cout << "  Laps        " << track.totalLaps() << '\n';
    std::cout << "  Pit loss    " << formatSeconds(track.pitLoss()) << " s\n";

    std::cout << "  Sectors     " << track.sectorCount();
    if (track.sectorCount() > 0U) {
        std::cout << "   ";
        for (std::size_t i = 0; i < track.sectors().size(); ++i) {
            if (i > 0U) {
                std::cout << " + ";
            }
            std::cout << track.sectors()[i].name << ' ' << formatSeconds(track.sectors()[i].baseTime);
        }
    }
    std::cout << '\n';
    std::cout << "  Base lap    " << formatSeconds(track.baseLapTime()) << " s\n\n";

    std::cout << "Tire compounds\n";
    std::cout << "  " << std::left << std::setw(12) << "compound" << std::right
              << std::setw(11) << "base pace" << std::setw(10) << "deg/lap"
              << std::setw(11) << "cliff lap" << std::setw(11) << "cliff deg"
              << std::setw(11) << "max stint" << '\n';

    for (const TireCompound& compound : compounds) {
        std::cout << "  " << std::left << std::setw(12) << compound.name() << std::right
                  << std::setw(11) << formatSeconds(compound.basePace())
                  << std::setw(10) << formatSeconds(compound.degradationPerLap())
                  << std::setw(11)
                  << (compound.hasCliff() ? std::to_string(compound.cliffLap()) : std::string("-"))
                  << std::setw(11)
                  << (compound.hasCliff() ? formatSeconds(compound.cliffDegradationPerLap())
                                          : std::string("-"))
                  << std::setw(11) << compound.maxStintLaps() << '\n';
    }
    std::cout << '\n';
}

void printStintCostTable(const PitStrategy& strategy) {
    const std::vector<TireCompound>& compounds = strategy.compounds();

    int rows = 0;
    for (const TireCompound& compound : compounds) {
        rows = std::max(rows, compound.maxStintLaps());
    }
    if (strategy.track().totalLaps() > 0) {
        rows = std::min(rows, strategy.track().totalLaps());
    }
    const bool truncated = rows > kMaxTableRows;
    if (truncated) {
        rows = kMaxTableRows;
    }

    std::cout << "Stint cost table   green-flag seconds, pit loss excluded\n";
    std::cout << "  " << std::right << std::setw(5) << "laps";
    for (const TireCompound& compound : compounds) {
        std::cout << std::setw(14) << compound.name();
    }
    std::cout << '\n';

    for (int laps = 1; laps <= rows; ++laps) {
        std::cout << "  " << std::right << std::setw(5) << laps;
        for (std::size_t c = 0; c < compounds.size(); ++c) {
            std::cout << std::setw(14) << formatSeconds(strategy.stintCost(c, laps));
        }
        std::cout << '\n';
    }
    if (truncated) {
        std::cout << "  ... truncated at " << kMaxTableRows << " rows\n";
    }
    std::cout << '\n';
}

void printStrategy(const Track& track, const StrategyResult& result) {
    std::cout << "Optimal strategy\n";

    if (result.stints.empty()) {
        std::cout << "  (no racing laps)\n\n";
    } else {
        std::cout << "  " << std::right << std::setw(3) << "#" << "  " << std::left
                  << std::setw(12) << "compound" << std::right << std::setw(6) << "laps"
                  << std::setw(16) << "lap range" << std::setw(15) << "stint time" << '\n';

        for (std::size_t i = 0; i < result.stints.size(); ++i) {
            const Stint& stint = result.stints[i];
            std::ostringstream range;
            range << stint.startLap << " - " << (stint.startLap + stint.laps - 1);

            std::cout << "  " << std::right << std::setw(3) << (i + 1U) << "  " << std::left
                      << std::setw(12) << stint.compoundName << std::right << std::setw(6)
                      << stint.laps << std::setw(16) << range.str() << std::setw(15)
                      << formatSeconds(stint.time) << '\n';
        }
        std::cout << '\n';
    }

    std::cout << "  Stops        " << result.stopCount();
    if (!result.pitLaps.empty()) {
        std::cout << "   end of lap ";
        for (std::size_t i = 0; i < result.pitLaps.size(); ++i) {
            if (i > 0U) {
                std::cout << ", ";
            }
            std::cout << result.pitLaps[i];
        }
    }
    std::cout << '\n';

    std::cout << "  Green time   " << formatSeconds(result.greenTime) << " s\n";
    std::cout << "  Pit time     " << formatSeconds(result.pitTime) << " s\n";
    std::cout << "  Total time   " << formatSeconds(result.totalTime) << " s   = "
              << formatClock(result.totalTime) << '\n';

    // FR-1 acceptance evidence: dp[totalLaps] is finite, the plan reconstructed
    // from it totals the same time, and the stint lengths cover the race exactly.
    std::cout << "  DP optimum   " << formatSeconds(result.dpValue) << " s   delta "
              << formatDelta(result.totalTime - result.dpValue) << " s\n";

    const int planned = result.plannedLaps();
    std::cout << "  Laps planned " << planned << " / " << track.totalLaps() << "   "
              << ((planned == track.totalLaps()) ? "OK" : "MISMATCH") << '\n';
}

void printStandings(const RaceSimulator& simulator, const std::string& heading) {
    const std::vector<Standing> table = simulator.standings();
    const std::vector<Driver>& drivers = simulator.drivers();

    std::cout << heading << '\n';
    std::cout << "  " << std::right << std::setw(4) << "pos" << std::setw(4) << "#" << "  "
              << std::left << std::setw(16) << "driver" << std::setw(10) << "compound"
              << std::right << std::setw(5) << "age" << std::setw(7) << "stops"
              << std::setw(16) << "race time" << std::setw(13) << "gap" << '\n';

    for (const Standing& row : table) {
        const Driver& driver = drivers[row.driverIndex];
        std::cout << "  " << std::right << std::setw(4) << row.position << std::setw(4)
                  << driver.number() << "  " << std::left << std::setw(16) << driver.name()
                  << std::setw(10) << driver.currentCompoundName() << std::right << std::setw(5)
                  << driver.tireAge() << std::setw(7) << driver.stopsMade() << std::setw(16)
                  << formatSeconds(driver.cumulativeTime()) << std::setw(13)
                  << (row.position == 1 ? std::string("-") : formatSeconds(row.gapToLeader))
                  << '\n';
    }
    std::cout << '\n';
}

/// FR-8: the optimizer's total and the simulated total must be the same number.
/// Returns true on pass. The tolerance is read from the core, never widened here.
bool printClosureCheck(const StrategyResult& plan, const Driver& driver) {
    const double difference = std::fabs(driver.cumulativeTime() - plan.totalTime);
    const bool passed = difference <= undercut::kClosureTolerance;

    const std::string simulatedLabel =
        "Driver " + std::to_string(driver.number()) + " simulated total time";

    // One label width and one value width, so every row lines up whatever the
    // driver number is.
    constexpr int kLabelWidth = 34;
    constexpr int kValueWidth = 18;

    std::cout << "Loop closure check   FR-8\n";
    std::cout << "  " << std::left << std::setw(kLabelWidth) << "Optimizer total time"
              << std::right << std::setw(kValueWidth) << formatSeconds(plan.totalTime, 9)
              << " s\n";
    std::cout << "  " << std::left << std::setw(kLabelWidth) << simulatedLabel << std::right
              << std::setw(kValueWidth) << formatSeconds(driver.cumulativeTime(), 9) << " s\n";
    std::cout << "  " << std::left << std::setw(kLabelWidth) << "Absolute difference"
              << std::right << std::setw(kValueWidth) << formatDelta(difference) << " s\n";
    std::cout << "  " << std::left << std::setw(kLabelWidth) << "Tolerance kClosureTolerance"
              << std::right << std::setw(kValueWidth)
              << formatDelta(undercut::kClosureTolerance) << " s\n";
    std::cout << "  " << std::left << std::setw(kLabelWidth) << "RESULT" << std::right
              << std::setw(kValueWidth) << (passed ? "PASS" : "FAIL") << '\n';

    if (!passed) {
        std::cout << "\n  The optimizer and the simulator disagree about the same race.\n"
                     "  That is a real modelling bug, not a tolerance to widen.\n";
    }
    return passed;
}

// ---------------------------------------------------------------------------

struct Options {
    std::string trackName = "Generic Circuit";
    int laps = 50;
    double pitLoss = 22.0;
    std::vector<undercut::Sector> sectors;
    std::vector<TireCompound> compounds;
    bool sectorsOverridden = false;
    bool compoundsOverridden = false;
    bool showTable = false;
    bool simulate = false;
    bool rivals = false;
    int lapLog = 0;
    bool showHelp = false;
};

/// Returns the value that follows `--flag`, or throws if it is missing.
std::string requireValue(int argc, char** argv, int& index, const std::string& flag) {
    if (index + 1 >= argc) {
        throw std::invalid_argument(flag + ": missing value");
    }
    ++index;
    return std::string(argv[index]);
}

Options parseArguments(int argc, char** argv) {
    Options options;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            options.showHelp = true;
            return options;
        } else if (arg == "--laps") {
            options.laps = parseInt(requireValue(argc, argv, i, arg), "--laps");
        } else if (arg == "--pit-loss") {
            options.pitLoss = parseDouble(requireValue(argc, argv, i, arg), "--pit-loss");
        } else if (arg == "--track") {
            options.trackName = requireValue(argc, argv, i, arg);
        } else if (arg == "--sector") {
            const std::string spec = requireValue(argc, argv, i, arg);
            if (!options.sectorsOverridden) {
                options.sectors.clear();
                options.sectorsOverridden = true;
            }
            undercut::Sector sector;
            const std::vector<std::string> parts = split(spec, ':');
            if (parts.size() == 1U) {
                sector.name = "S" + std::to_string(options.sectors.size() + 1U);
                sector.baseTime = parseDouble(parts[0], "--sector");
            } else if (parts.size() == 2U) {
                sector.name = parts[0].empty()
                                  ? ("S" + std::to_string(options.sectors.size() + 1U))
                                  : parts[0];
                sector.baseTime = parseDouble(parts[1], "--sector");
            } else {
                throw std::invalid_argument("--sector \"" + spec +
                                            "\": expected SECONDS or NAME:SECONDS");
            }
            options.sectors.push_back(sector);
        } else if (arg == "--compound") {
            const std::string spec = requireValue(argc, argv, i, arg);
            if (!options.compoundsOverridden) {
                options.compounds.clear();
                options.compoundsOverridden = true;
            }
            options.compounds.push_back(parseCompound(spec));
        } else if (arg == "--show-table") {
            options.showTable = true;
        } else if (arg == "--simulate") {
            options.simulate = true;
        } else if (arg == "--rivals") {
            options.rivals = true;
            options.simulate = true;
        } else if (arg == "--lap-log") {
            options.lapLog = parseInt(requireValue(argc, argv, i, arg), "--lap-log");
            if (options.lapLog < 0) {
                throw std::invalid_argument("--lap-log: must not be negative");
            }
            options.simulate = true;
        } else {
            throw std::invalid_argument("unrecognised option \"" + arg +
                                        "\"; try --help");
        }
    }

    return options;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;

    try {
        options = parseArguments(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "undercut: " << error.what() << '\n';
        return kExitUsage;
    }

    if (options.showHelp) {
        printUsage(std::cout);
        return kExitOk;
    }

    try {
        Track track(options.trackName, options.laps, options.pitLoss);

        if (options.sectorsOverridden) {
            for (const undercut::Sector& sector : options.sectors) {
                track.addSector(sector.name, sector.baseTime);
            }
        } else {
            track.addSector("S1", 28.4);
            track.addSector("S2", 31.2);
            track.addSector("S3", 30.4);
        }

        std::vector<TireCompound> compounds =
            options.compoundsOverridden ? options.compounds : undercut::defaultCompounds();

        const PitStrategy strategy(track, compounds);

        std::cout << "============================================================\n"
                  << " UNDERCUT - F1 Pit-Stop & Race Strategy Scheduler\n"
                  << "============================================================\n\n";

        printSetup(strategy.track(), strategy.compounds());

        if (options.showTable) {
            printStintCostTable(strategy);
        }

        const StrategyResult result = strategy.optimize();

        if (!result.feasible) {
            std::cerr << "undercut: no feasible strategy covers " << track.totalLaps()
                      << " laps with this compound set\n";
            return kExitInfeasible;
        }

        printStrategy(strategy.track(), result);

        if (options.simulate) {
            std::cout << '\n';

            RaceSimulator simulator(strategy.track(), strategy.compounds());

            // FR-7: the stop laps come from the optimizer's backtracked plan.
            // No lap number is written by hand anywhere in this file.
            simulator.addDriver(1, "OPTIMAL", result);

            if (options.rivals) {
                int nextNumber = 2;
                for (std::size_t c = 0; c < strategy.compounds().size(); ++c) {
                    const StrategyResult reference = strategy.singleCompoundPlan(c);
                    if (!reference.feasible) {
                        continue;
                    }
                    simulator.addDriver(nextNumber, "ALL-" + strategy.compounds()[c].name(),
                                        reference);
                    ++nextNumber;
                }
            }

            if (options.lapLog > 0) {
                while (simulator.advanceLap()) {
                    if (simulator.lapsRun() % options.lapLog == 0 || simulator.finished()) {
                        std::ostringstream heading;
                        heading << "After lap " << simulator.lapsRun();
                        printStandings(simulator, heading.str());
                    }
                }
            } else {
                simulator.runToFinish();
                printStandings(simulator, "Final classification");
            }

            if (!printClosureCheck(result, simulator.drivers().front())) {
                return kExitClosureFailed;
            }
        }
    } catch (const std::exception& error) {
        std::cerr << "undercut: " << error.what() << '\n';
        return kExitUsage;
    }

    return kExitOk;
}
