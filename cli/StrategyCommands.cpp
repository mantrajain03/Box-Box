#include "StrategyCommands.hpp"

#include <cctype>
#include <cmath>
#include <cstddef>
#include <iomanip>
#include <map>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "boxbox/Common.hpp"
#include "boxbox/StrategyAnalytics.hpp"

namespace boxbox {

namespace {

struct UsageError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// Parsed "--key value" pairs. --plan may repeat; every other key may not.
struct ParsedOptions {
    std::map<std::string, std::string> single;
    std::vector<std::string> plans;
};

ParsedOptions parseOptions(const std::vector<std::string>& args) {
    ParsedOptions parsed;
    for (std::size_t i = 1; i < args.size(); i += 2U) {
        const std::string& key = args[i];
        if (key.rfind("--", 0) != 0U) {
            throw UsageError("unexpected argument '" + key + "'");
        }
        if (i + 1U >= args.size()) {
            throw UsageError("option " + key + " needs a value");
        }
        if (key == "--plan") {
            parsed.plans.push_back(args[i + 1U]);
            continue;
        }
        if (parsed.single.count(key) != 0U) {
            throw UsageError("option " + key + " given twice");
        }
        parsed.single[key] = args[i + 1U];
    }
    return parsed;
}

void allowOnly(const ParsedOptions& parsed, const std::vector<std::string>& allowed,
               bool plansAllowed) {
    for (const auto& entry : parsed.single) {
        bool ok = false;
        for (const std::string& name : allowed) {
            ok = ok || entry.first == name;
        }
        if (!ok) {
            throw UsageError("unknown option " + entry.first);
        }
    }
    if (!plansAllowed && !parsed.plans.empty()) {
        throw UsageError("--plan does not apply to this command");
    }
}

double parseNumber(const std::string& text, const std::string& what) {
    std::size_t used = 0;
    double value = 0.0;
    try {
        value = std::stod(text, &used);
    } catch (const std::exception&) {
        throw UsageError(what + " expects a number, got '" + text + "'");
    }
    if (used != text.size() || !std::isfinite(value)) {
        throw UsageError(what + " expects a finite number, got '" + text + "'");
    }
    return value;
}

int parseWhole(const std::string& text, const std::string& what, int low, int high) {
    std::size_t used = 0;
    long value = 0;
    try {
        value = std::stol(text, &used);
    } catch (const std::exception&) {
        throw UsageError(what + " expects a whole number, got '" + text + "'");
    }
    if (used != text.size() || value < low || value > high) {
        throw UsageError(what + " expects a whole number from " + std::to_string(low) + " to " +
                         std::to_string(high) + ", got '" + text + "'");
    }
    return static_cast<int>(value);
}

bool sameName(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

std::size_t findCompound(const PitStrategy& strategy, const std::string& name) {
    for (std::size_t c = 0; c < strategy.compounds().size(); ++c) {
        if (sameName(strategy.compounds()[c].name(), name)) {
            return c;
        }
    }
    throw UsageError("no compound named '" + name + "'");
}

const std::string& requireOpt(const ParsedOptions& parsed, const std::string& key) {
    const auto it = parsed.single.find(key);
    if (it == parsed.single.end()) {
        throw UsageError("missing required option " + key);
    }
    return it->second;
}

std::vector<std::string> splitOn(const std::string& text, char separator) {
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

/// NAME:LAPS or NAME:FROM-TO, comma separated. A range must begin on the lap
/// after the previous stint, so a gap or an overlap is reported here instead
/// of being silently reshaped.
std::vector<PlannedStint> parsePlan(const std::string& spec, const PitStrategy& strategy) {
    std::vector<PlannedStint> plan;
    int nextLap = 1;
    for (const std::string& token : splitOn(spec, ',')) {
        const std::vector<std::string> halves = splitOn(token, ':');
        if (halves.size() != 2U || halves[0].empty() || halves[1].empty()) {
            throw UsageError("plan stint '" + token + "' should look like NAME:LAPS or NAME:FROM-TO");
        }
        PlannedStint stint;
        stint.compoundIndex = findCompound(strategy, halves[0]);
        const std::vector<std::string> range = splitOn(halves[1], '-');
        if (range.size() == 1U) {
            stint.laps = parseWhole(range[0], "plan stint '" + token + "'", 1, 1000000);
        } else if (range.size() == 2U) {
            const int from = parseWhole(range[0], "plan stint '" + token + "'", 1, 1000000);
            const int to = parseWhole(range[1], "plan stint '" + token + "'", 1, 1000000);
            if (from != nextLap) {
                throw UsageError("plan stint '" + token + "' starts on lap " + std::to_string(from) +
                                 " but the previous stint ended on lap " +
                                 std::to_string(nextLap - 1) + " (no gaps or overlaps)");
            }
            if (to < from) {
                throw UsageError("plan stint '" + token + "' ends before it starts");
            }
            stint.laps = to - from + 1;
        } else {
            throw UsageError("plan stint '" + token + "' has too many '-'");
        }
        nextLap += stint.laps;
        plan.push_back(stint);
    }
    return plan;
}

std::string planText(const PitStrategy& strategy, const std::vector<PlannedStint>& plan) {
    // Identical neighbouring stints are folded ("SOFT:1 x49") so a plan of
    // one-lap stints stays readable.
    std::ostringstream text;
    for (std::size_t i = 0; i < plan.size();) {
        std::size_t run = 1;
        while (i + run < plan.size() && plan[i + run].compoundIndex == plan[i].compoundIndex &&
               plan[i + run].laps == plan[i].laps) {
            ++run;
        }
        if (i > 0U) text << ' ';
        const std::string name = plan[i].compoundIndex < strategy.compounds().size()
                                     ? strategy.compounds()[plan[i].compoundIndex].name()
                                     : "?";
        text << name << ':' << plan[i].laps;
        if (run > 1U) text << " x" << run;
        i += run;
    }
    return plan.empty() ? "(no stints)" : text.str();
}

void printAssumptions(const PitStrategy& strategy, std::ostream& out) {
    out << "Track " << strategy.track().name() << ", " << strategy.track().totalLaps()
        << " laps, pit loss " << strategy.track().pitLoss() << " s, base lap "
        << strategy.track().baseLapTime() << " s. Every plan below uses this same track, tire model,\n"
        << "distance and pit loss, and the optimizer's own cost arithmetic (simplified model:\n"
        << "no traffic, tire warm-up or race-control effects).\n\n";
}

void printReportDetail(const PitStrategy& strategy, const StrategyReport& r, std::ostream& out) {
    out << r.name << "\n";
    if (!r.feasible) {
        out << "  INFEASIBLE: " << r.reason << "\n  plan: " << planText(strategy, r.plan) << "\n\n";
        return;
    }
    out << "  " << std::setw(4) << "#" << std::setw(10) << "compound" << std::setw(8) << "laps"
        << std::setw(12) << "lap range" << std::setw(14) << "time (s)" << std::setw(10) << "share"
        << "\n";
    for (std::size_t i = 0; i < r.stints.size(); ++i) {
        const StintReport& s = r.stints[i];
        std::ostringstream range;
        range << s.firstLap << "-" << s.lastLap;
        out << "  " << std::setw(4) << (i + 1U) << std::setw(10) << s.compoundName << std::setw(8)
            << s.laps << std::setw(12) << range.str() << std::setw(14) << s.time << std::setw(9)
            << 100.0 * s.share << "%\n";
    }
    out << "  stops " << r.stops << "   green " << r.greenTime << " s   pit " << r.pitTime
        << " s   total " << r.totalTime << " s\n";
    out << "  compound usage:";
    for (const CompoundLaps& c : r.compoundUsage) {
        out << ' ' << c.compoundName << ' ' << c.laps << " laps;";
    }
    out << "\n  vs optimum " << r.optimumTotal << " s: " << (r.deltaVsOptimum >= 0 ? "+" : "")
        << r.deltaVsOptimum << " s (" << (r.percentVsOptimum >= 0 ? "+" : "")
        << r.percentVsOptimum << " %)\n\n";
}

void printComparisonTable(const std::vector<StrategyReport>& reports, std::ostream& out) {
    out << std::left << std::setw(16) << "strategy" << std::right << std::setw(7) << "stops"
        << std::setw(13) << "green (s)" << std::setw(10) << "pit (s)" << std::setw(13)
        << "total (s)" << std::setw(12) << "delta (s)" << std::setw(10) << "delta %" << "\n";
    for (const StrategyReport& r : reports) {
        out << std::left << std::setw(16) << r.name << std::right;
        if (!r.feasible) {
            out << "  infeasible: " << r.reason << "\n";
            continue;
        }
        out << std::setw(7) << r.stops << std::setw(13) << r.greenTime << std::setw(10)
            << r.pitTime << std::setw(13) << r.totalTime << std::setw(12) << r.deltaVsOptimum
            << std::setw(10) << r.percentVsOptimum << "\n";
    }
}

struct Range {
    double from = 0.0;
    double to = 0.0;
    double step = 0.0;
};

Range parseRange(const std::string& text, const std::string& option) {
    const std::vector<std::string> parts = splitOn(text, ':');
    if (parts.size() != 3U) {
        throw UsageError(option + " expects FROM:TO:STEP, got '" + text + "'");
    }
    Range r;
    r.from = parseNumber(parts[0], option + " FROM");
    r.to = parseNumber(parts[1], option + " TO");
    r.step = parseNumber(parts[2], option + " STEP");
    return r;
}

CompoundParameter parseParameter(const std::string& text) {
    if (text == "pace") return CompoundParameter::BasePace;
    if (text == "degradation") return CompoundParameter::Degradation;
    if (text == "max-stint") return CompoundParameter::MaxStint;
    if (text == "cliff-lap") return CompoundParameter::CliffLap;
    if (text == "cliff-deg") return CompoundParameter::CliffDegradation;
    throw UsageError("--param must be pace, degradation, max-stint, cliff-lap or cliff-deg, got '" +
                     text + "'");
}

void printSweep(const PitStrategy& strategy, const SweepResult& sweep, const std::string& valueLabel,
                std::ostream& out) {
    out << std::setw(14) << valueLabel << std::setw(14) << "total (s)" << std::setw(7) << "stops"
        << std::setw(13) << "change (s)" << std::setw(9) << "plan?" << "  plan\n";
    for (std::size_t i = 0; i < sweep.rows.size(); ++i) {
        const SweepRow& row = sweep.rows[i];
        out << std::setw(14) << row.value << std::setw(14) << row.totalTime << std::setw(7)
            << row.stops << std::setw(13) << row.deltaFromPrevious << std::setw(9)
            << (i == 0U ? "-" : (row.planChanged ? "changed" : "same")) << "  "
            << planText(strategy, row.plan) << "\n";
    }
    if (sweep.firstChange) {
        out << "\nThe optimal plan first differs from the first row's at " << valueLabel << " = "
            << sweep.rows[*sweep.firstChange].value << ".\n";
    } else {
        out << "\nThe optimal plan does not change anywhere in this range.\n";
    }
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

int runEvaluate(const std::vector<std::string>& args, const PitStrategy& strategy,
                std::ostream& out) {
    const ParsedOptions parsed = parseOptions(args);
    allowOnly(parsed, {}, true);
    if (parsed.plans.size() != 1U) {
        throw UsageError("strategy evaluate needs exactly one --plan SPEC");
    }
    const std::vector<PlannedStint> plan = parsePlan(parsed.plans.front(), strategy);

    out << std::fixed << std::setprecision(3);
    printAssumptions(strategy, out);
    const StrategyReport report = evaluateCustom(strategy, "custom plan", plan);
    printReportDetail(strategy, report, out);
    return report.feasible ? kStrategyOk : kStrategyInfeasible;
}

int runCompare(const std::vector<std::string>& args, const PitStrategy& strategy,
               std::ostream& out) {
    const ParsedOptions parsed = parseOptions(args);
    allowOnly(parsed, {"--max-stops"}, true);
    const int cap = std::max(strategy.track().totalLaps(), 0);
    const int maxStops = parsed.single.count("--max-stops") != 0U
                             ? parseWhole(parsed.single.at("--max-stops"), "--max-stops", 0, 1000)
                             : std::min(3, cap);

    std::vector<NamedPlan> plans = standardCandidates(strategy, maxStops);
    int index = 1;
    bool anyInfeasible = false;
    for (const std::string& spec : parsed.plans) {
        plans.push_back(NamedPlan{"custom " + std::to_string(index++), parsePlan(spec, strategy)});
    }

    out << std::fixed << std::setprecision(3);
    printAssumptions(strategy, out);
    const std::vector<StrategyReport> reports = compareStrategies(strategy, plans);
    printComparisonTable(reports, out);
    out << "\n";
    for (const StrategyReport& r : reports) {
        printReportDetail(strategy, r, out);
        anyInfeasible = anyInfeasible || !r.feasible;
    }
    return anyInfeasible ? kStrategyInfeasible : kStrategyOk;
}

int runAlternatives(const std::vector<std::string>& args, const PitStrategy& strategy,
                    std::ostream& out) {
    const ParsedOptions parsed = parseOptions(args);
    allowOnly(parsed, {"--count"}, false);
    const int count = parsed.single.count("--count") != 0U
                          ? parseWhole(parsed.single.at("--count"), "--count", 1,
                                       static_cast<int>(kMaxAlternatives))
                          : 5;

    out << std::fixed << std::setprecision(3);
    printAssumptions(strategy, out);
    const std::vector<StrategyReport> best = kBestPlans(strategy, static_cast<std::size_t>(count));
    out << "The " << best.size() << " cheapest distinct plans (requested " << count << ", at most "
        << kMaxAlternatives << " allowed). The search is exact up to that bound: these are the\n"
        << "true cheapest plans, not a sample; there is no claim beyond the bound.\n";
    if (best.size() < static_cast<std::size_t>(count)) {
        out << "Only " << best.size() << " distinct plans exist for this race.\n";
    }
    out << "\n";
    printComparisonTable(best, out);
    out << "\n";
    for (const StrategyReport& r : best) {
        out << "  " << std::left << std::setw(16) << r.name << std::right
            << planText(strategy, r.plan) << "\n";
    }
    out << "\n";
    return kStrategyOk;
}

int runSensitivity(const std::vector<std::string>& args, const PitStrategy& strategy,
                   std::ostream& out) {
    const ParsedOptions parsed = parseOptions(args);
    allowOnly(parsed, {"--pit-loss", "--compound", "--param", "--range"}, false);
    const bool pit = parsed.single.count("--pit-loss") != 0U;
    const bool compound = parsed.single.count("--compound") != 0U;
    if (pit == compound) {
        throw UsageError("strategy sensitivity needs either --pit-loss FROM:TO:STEP or "
                         "--compound NAME --param P --range FROM:TO:STEP");
    }

    // Everything is parsed and computed before the first line is printed, so a
    // rejected request leaves nothing half-written on stdout.
    if (pit) {
        if (parsed.single.count("--param") != 0U || parsed.single.count("--range") != 0U) {
            throw UsageError("--param and --range belong with --compound");
        }
        const Range r = parseRange(parsed.single.at("--pit-loss"), "--pit-loss");
        const SweepResult sweep = pitLossSweep(strategy, r.from, r.to, r.step);
        out << std::fixed << std::setprecision(3);
        printAssumptions(strategy, out);
        out << "Pit-loss sweep: each row re-runs the optimizer at that pit loss.\n\n";
        printSweep(strategy, sweep, "pit loss (s)", out);
        return kStrategyOk;
    }

    const std::size_t index = findCompound(strategy, parsed.single.at("--compound"));
    const std::string param = requireOpt(parsed, "--param");
    const Range r = parseRange(requireOpt(parsed, "--range"), "--range");
    const SweepResult sweep =
        compoundSweep(strategy, index, parseParameter(param), r.from, r.to, r.step);
    out << std::fixed << std::setprecision(3);
    printAssumptions(strategy, out);
    out << "Compound sweep: " << strategy.compounds()[index].name() << " " << param
        << ", each row rebuilds that compound and re-runs the optimizer.\n\n";
    printSweep(strategy, sweep, param, out);
    return kStrategyOk;
}

int runStops(const std::vector<std::string>& args, const PitStrategy& strategy,
             std::ostream& out) {
    const ParsedOptions parsed = parseOptions(args);
    allowOnly(parsed, {"--max"}, false);
    const int cap = std::max(strategy.track().totalLaps(), 0);
    const int maxStops = parsed.single.count("--max") != 0U
                             ? parseWhole(parsed.single.at("--max"), "--max", 0, cap)
                             : std::min(4, cap);

    out << std::fixed << std::setprecision(3);
    printAssumptions(strategy, out);
    const StrategyResult optimum = strategy.optimize();
    out << "Best plan for each exact stop count (optimum overall: " << optimum.stopCount()
        << " stop(s), " << optimum.totalTime << " s)\n\n";
    for (const StopCountResult& row : bestForStopCount(strategy, maxStops)) {
        if (!row.feasible) {
            out << row.stops << " stop(s): infeasible - " << row.reason << "\n";
            continue;
        }
        out << row.stops << " stop(s): " << std::setw(10) << row.report.totalTime << " s  ("
            << (row.report.deltaVsOptimum >= 0 ? "+" : "") << row.report.deltaVsOptimum << " s)  "
            << planText(strategy, row.report.plan) << "\n";
    }
    out << "\n";
    return kStrategyOk;
}

int runTraffic(const std::vector<std::string>& args, const PitStrategy& strategy,
               std::ostream& out) {
    const ParsedOptions parsed = parseOptions(args);
    allowOnly(parsed, {"--min-gap", "--pass-margin"}, false);
    OvertakeRule rule;
    rule.enabled = true;
    if (parsed.single.count("--min-gap") != 0U) {
        rule.minGap = parseNumber(parsed.single.at("--min-gap"), "--min-gap");
    }
    if (parsed.single.count("--pass-margin") != 0U) {
        rule.passMargin = parseNumber(parsed.single.at("--pass-margin"), "--pass-margin");
    }
    const int cap = std::max(strategy.track().totalLaps(), 0);

    const std::vector<NamedPlan> plans = standardCandidates(strategy, std::min(2, cap));
    const std::vector<TrafficComparisonRow> rows = cleanAirVsTraffic(strategy, plans, rule);

    out << std::fixed << std::setprecision(3);
    printAssumptions(strategy, out);
    out << "Same field run twice: overtaking off (clean air), then on with min gap " << rule.minGap
        << " s and pass margin " << rule.passMargin << " s.\n\n";

    out << std::left << std::setw(16) << "car" << std::right << std::setw(14) << "clean (s)"
        << std::setw(14) << "traffic (s)" << std::setw(13) << "lost (s)" << std::setw(8) << "clean P"
        << std::setw(10) << "traffic P" << "\n";
    for (const TrafficComparisonRow& r : rows) {
        out << std::left << std::setw(16) << r.name << std::right << std::setw(14) << r.cleanTime
            << std::setw(14) << r.trafficTime << std::setw(13) << r.trafficLoss << std::setw(8)
            << r.cleanPosition << std::setw(10) << r.trafficPosition << "\n";
    }
    out << "\n";
    return kStrategyOk;
}

}  // namespace

void printStrategyHelp(std::ostream& out) {
    out << "Usage: boxbox [race options] strategy <command> [options]\n"
           "\n"
           "A plan SPEC is a comma-separated list of NAME:LAPS or NAME:FROM-TO stints,\n"
           "e.g. SOFT:15,HARD:35 or SOFT:1-15,HARD:16-50. Stints must cover the race exactly.\n"
           "\n"
           "Commands:\n"
           "  evaluate --plan SPEC\n"
           "      Cost your plan and compare it with the optimum. Exit 2 if it is infeasible.\n"
           "  compare [--plan SPEC ...] [--max-stops N]\n"
           "      The optimum, each compound alone, and the best plan for 0..N stops\n"
           "      (default 3), plus any plans of your own, all under one cost model.\n"
           "  alternatives [--count K]\n"
           "      The K cheapest distinct plans (default 5, at most 50). Exact up to that bound.\n"
           "  sensitivity --pit-loss FROM:TO:STEP\n"
           "      Re-run the optimizer across a range of pit-loss values (seconds).\n"
           "  sensitivity --compound NAME --param P --range FROM:TO:STEP\n"
           "      Vary one compound parameter and re-run the optimizer. P is pace,\n"
           "      degradation, max-stint, cliff-lap or cliff-deg.\n"
           "  stops [--max N]\n"
           "      The best plan for each exact stop count 0..N (default 4).\n"
           "  traffic [--min-gap SECONDS] [--pass-margin SECONDS]\n"
           "      Run the comparison set clean, then with the overtaking rule on.\n"
           "  help\n"
           "      This message.\n";
}

int runStrategyCommand(const std::vector<std::string>& args, const PitStrategy& strategy,
                       std::ostream& out, std::ostream& err) {
    if (args.empty() || args[0] == "help" || args[0] == "--help" || args[0] == "-h") {
        printStrategyHelp(out);
        return kStrategyOk;
    }

    try {
        const std::string& command = args[0];
        if (command == "evaluate") return runEvaluate(args, strategy, out);
        if (command == "compare") return runCompare(args, strategy, out);
        if (command == "alternatives") return runAlternatives(args, strategy, out);
        if (command == "sensitivity") return runSensitivity(args, strategy, out);
        if (command == "stops") return runStops(args, strategy, out);
        if (command == "traffic") return runTraffic(args, strategy, out);
        throw UsageError("unknown strategy command '" + command + "'; try 'strategy help'");
    } catch (const std::exception& error) {
        err << "boxbox: strategy: " << error.what() << "\n";
        return kStrategyUsageError;
    }
}

}  // namespace boxbox
