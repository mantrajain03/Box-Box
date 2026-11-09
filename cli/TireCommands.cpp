#include "TireCommands.hpp"

#include <cctype>
#include <cmath>
#include <cstddef>
#include <iomanip>
#include <map>
#include <ostream>
#include <stdexcept>
#include <utility>

#include "boxbox/Common.hpp"
#include "boxbox/TireAnalytics.hpp"

namespace boxbox {

namespace {


struct UsageError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// Parsed "--key value" pairs. --override may repeat; every other key may not.
struct ParsedOptions {
    std::map<std::string, std::string> single;
    std::vector<std::string> overrides;
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
        if (key == "--override") {
            parsed.overrides.push_back(args[i + 1U]);
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
               bool overridesAllowed) {
    for (const auto& entry : parsed.single) {
        bool ok = false;
        for (const std::string& name : allowed) {
            ok = ok || entry.first == name;
        }
        if (!ok) {
            throw UsageError("unknown option " + entry.first);
        }
    }
    if (!overridesAllowed && !parsed.overrides.empty()) {
        throw UsageError("--override only applies to 'tires range'");
    }
}

double parseSeconds(const std::string& text, const std::string& option) {
    std::size_t used = 0;
    double value = 0.0;
    try {
        value = std::stod(text, &used);
    } catch (const std::exception&) {
        throw UsageError(option + " expects a number of seconds, got '" + text + "'");
    }
    if (used != text.size() || !std::isfinite(value)) {
        throw UsageError(option + " expects a finite number of seconds, got '" + text + "'");
    }
    return value;
}

int parseAge(const std::string& text, const std::string& option) {
    std::size_t used = 0;
    long value = 0;
    try {
        value = std::stol(text, &used);
    } catch (const std::exception&) {
        throw UsageError(option + " expects a whole tire age, got '" + text + "'");
    }
    if (used != text.size() || value < 0 || value > 1000000) {
        throw UsageError(option + " expects a non-negative whole tire age, got '" + text + "'");
    }
    return static_cast<int>(value);
}

/// Case-insensitive, so `--compound soft` finds the default "SOFT".
bool sameName(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto ca = static_cast<unsigned char>(a[i]);
        const auto cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb)) {
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

void printCliff(const DegradationProfile& p, std::ostream& out) {
    if (p.cliffActivationAge()) {
        out << "Cliff: from tire age " << *p.cliffActivationAge() << ", each lap of age adds "
            << p.cliffDegradationPerLap() << " s on top of normal wear\n";
    } else if (p.cliffLap() && !(p.cliffDegradationPerLap() > 0.0)) {
        out << "Cliff: configured after age " << *p.cliffLap()
            << ", but its rate is 0 s/lap, so it adds nothing\n";
    } else if (p.cliffLap()) {
        out << "Cliff: configured after age " << *p.cliffLap()
            << ", never reached inside the legal stint\n";
    } else {
        out << "Cliff: none\n";
    }
}

void printProfile(const DegradationProfile& p, std::ostream& out) {
    out << "Compound: " << p.compound() << "\n";
    out << "Maximum legal stint: " << p.maxStint() << " laps\n";
    printCliff(p, out);
    out << "\n";
    out << "step = change from the previous lap; stint = total time of a stint\n"
           "that ends on this lap, with no pit loss (PitStrategy::stintCost).\n\n";
    out << std::setw(5) << "age" << std::setw(13) << "lap (s)" << std::setw(13) << "step (s)"
        << std::setw(15) << "vs fresh (s)" << std::setw(17) << "stint (s)" << "\n";
    for (std::size_t a = 0; a < p.lapTime().size(); ++a) {
        out << std::setw(5) << a << std::setw(13) << p.lapTime()[a] << std::setw(13)
            << p.incrementalDegradation()[a] << std::setw(15) << p.degradationVsFresh()[a]
            << std::setw(17) << p.stintCost()[a + 1U];
        if (p.cliffActivationAge() && static_cast<int>(a) == *p.cliffActivationAge()) {
            out << "   <- cliff";
        }
        out << "\n";
    }
}

int runProfile(const std::vector<std::string>& args, const PitStrategy& strategy,
               std::ostream& out) {
    const ParsedOptions parsed = parseOptions(args);
    allowOnly(parsed, {"--compound"}, false);

    out << std::fixed << std::setprecision(3);
    out << "Tire degradation profile, track " << strategy.track().name() << " (base lap "
        << strategy.track().baseLapTime() << " s)\n\n";

    const auto it = parsed.single.find("--compound");
    if (it != parsed.single.end()) {
        printProfile(DegradationProfile::fromStrategy(strategy, findCompound(strategy, it->second)),
                     out);
        return kTiresOk;
    }
    const std::vector<DegradationProfile> profiles = buildProfiles(strategy);
    for (std::size_t c = 0; c < profiles.size(); ++c) {
        if (c > 0U) {
            out << "\n";
        }
        printProfile(profiles[c], out);
    }
    return kTiresOk;
}

int runRange(const std::vector<std::string>& args, const PitStrategy& strategy,
             std::ostream& out) {
    const ParsedOptions parsed = parseOptions(args);
    allowOnly(parsed, {"--compound", "--from", "--to"}, true);

    const std::size_t c = findCompound(strategy, requireOpt(parsed, "--compound"));
    const int from = parseAge(requireOpt(parsed, "--from"), "--from");
    const int to = parseAge(requireOpt(parsed, "--to"), "--to");
    if (from > to) {
        throw UsageError("--from must not be greater than --to");
    }

    TireRangeAnalyzer analyzer(DegradationProfile::fromStrategy(strategy, c));
    const std::size_t lastAge = analyzer.ageCount() - 1U;  // every compound has >= 1 age
    if (static_cast<std::size_t>(to) > lastAge) {
        throw UsageError("--to " + std::to_string(to) + " is past the legal stint (last age " +
                         std::to_string(lastAge) + ")");
    }

    // AGE=SECONDS what-ifs, applied as segment-tree point updates.
    std::vector<std::pair<int, double>> applied;
    for (const std::string& spec : parsed.overrides) {
        const std::size_t eq = spec.find('=');
        if (eq == std::string::npos) {
            throw UsageError("--override expects AGE=SECONDS, got '" + spec + "'");
        }
        const int age = parseAge(spec.substr(0, eq), "--override age");
        const double seconds = parseSeconds(spec.substr(eq + 1U), "--override time");
        if (static_cast<std::size_t>(age) > lastAge) {
            throw UsageError("--override age " + std::to_string(age) +
                             " is past the legal stint (last age " + std::to_string(lastAge) +
                             ")");
        }
        analyzer.overrideLapTime(static_cast<std::size_t>(age), seconds);
        applied.emplace_back(age, seconds);
    }

    // User ranges are inclusive; the library is half-open.
    const auto b = static_cast<std::size_t>(from);
    const auto e = static_cast<std::size_t>(to) + 1U;

    out << std::fixed << std::setprecision(3);
    out << "Range query, compound " << strategy.compounds()[c].name() << ", tire ages " << from
        << " to " << to << " inclusive (" << (e - b) << " laps)\n";
    for (const auto& item : applied) {
        out << "  what-if: age " << item.first << " set to " << item.second << " s (model "
            << analyzer.profile().lapTime()[static_cast<std::size_t>(item.first)] << " s)\n";
    }
    out << "  total lap time:        " << analyzer.lapTimeSum(b, e) << " s\n";
    out << "  average lap time:      " << analyzer.averageLapTime(b, e) << " s\n";
    out << "  fastest lap:           " << analyzer.minLapTime(b, e) << " s at age "
        << analyzer.fastestAge(b, e) << "\n";
    out << "  total degradation:     " << analyzer.degradationSum(b, e)
        << " s (sum of each lap's loss vs the model's fresh lap)\n";
    return kTiresOk;
}

const char* methodText(bool binary) {
    return binary ? "binary search (stored lap times never fall)"
                  : "linear scan (stored lap times are not monotonic)";
}

int runThreshold(const std::vector<std::string>& args, const PitStrategy& strategy,
                 std::ostream& out) {
    const ParsedOptions parsed = parseOptions(args);
    allowOnly(parsed, {"--compound", "--max-degradation", "--max-average", "--slower-than"},
              false);

    const std::size_t c = findCompound(strategy, requireOpt(parsed, "--compound"));
    const std::size_t modes = parsed.single.count("--max-degradation") +
                              parsed.single.count("--max-average") +
                              parsed.single.count("--slower-than");
    if (modes != 1U) {
        throw UsageError("give exactly one of --max-degradation, --max-average, --slower-than");
    }

    const DegradationProfile profile = DegradationProfile::fromStrategy(strategy, c);
    const std::string& name = strategy.compounds()[c].name();
    out << std::fixed << std::setprecision(3);

    if (const auto it = parsed.single.find("--max-degradation"); it != parsed.single.end()) {
        const double limit = parseSeconds(it->second, "--max-degradation");
        const ThresholdResult r = firstAgeDegradationExceeds(profile, limit);
        out << name << ": first tire age losing more than " << limit << " s vs a fresh tire: ";
        if (r.value) {
            out << *r.value << "\n";
        } else {
            out << "none within the legal stint of " << profile.maxStint() << " laps\n";
        }
        out << "method: " << methodText(r.usedBinarySearch) << "\n";
        return kTiresOk;
    }
    if (const auto it = parsed.single.find("--max-average"); it != parsed.single.end()) {
        const double limit = parseSeconds(it->second, "--max-average");
        const ThresholdResult r = longestStintUnderAverage(profile, limit);
        out << name << ": longest stint averaging at most " << limit << " s/lap: ";
        if (r.value) {
            out << *r.value << " laps\n";
        } else {
            out << "none, even a 1-lap stint is slower\n";
        }
        out << "method: " << methodText(r.usedBinarySearch) << "\n";
        return kTiresOk;
    }
    const std::size_t other = findCompound(strategy, parsed.single.at("--slower-than"));
    const DegradationProfile otherProfile = DegradationProfile::fromStrategy(strategy, other);
    const CompoundComparison cmp = compareCompounds(profile, otherProfile);
    const std::string& otherName = strategy.compounds()[other].name();
    out << name << " vs " << otherName << ", tire ages 0 to " << (cmp.agesCompared - 1)
        << " (legal on both)\n";
    out << "  first age " << name << " is slower: ";
    if (cmp.firstSlowerAge) {
        out << *cmp.firstSlowerAge << "\n";
    } else {
        out << "never\n";
    }
    out << "  faster compound switches at age: ";
    if (cmp.switchAges.empty()) {
        out << "never (the gap does not change sign)\n";
    } else {
        for (std::size_t i = 0; i < cmp.switchAges.size(); ++i) {
            const auto age = static_cast<std::size_t>(cmp.switchAges[i]);
            const bool aSlower =
                definitelyLess(otherProfile.lapTime()[age], profile.lapTime()[age]);
            out << (i > 0U ? ", " : "") << cmp.switchAges[i] << " ("
                << (aSlower ? otherName : name) << " ahead)";
        }
        out << "\n";
    }
    out << "  ties (within " << std::scientific << std::setprecision(0) << kTimeEpsilon
        << std::fixed << std::setprecision(3)
        << " s) count as neither slower nor a switch\n";
    out << "method: linear scan (the gap can change direction at a cliff)\n";
    return kTiresOk;
}

}  // namespace

void printTiresHelp(std::ostream& out) {
    out << "Usage: boxbox [race options] tires <command> [options]\n"
           "\n"
           "Commands:\n"
           "  profile [--compound NAME]\n"
           "      Lap time, wear and stint cost for every legal tire age.\n"
           "  range --compound NAME --from AGE --to AGE [--override AGE=SECONDS ...]\n"
           "      Total, average and fastest lap plus total wear over a range of\n"
           "      tire ages. Both ends are included. Ages start at 0.\n"
           "      --override replaces one age's lap time as a what-if, for\n"
           "      example a lock-up; it may be repeated.\n"
           "  threshold --compound NAME --max-degradation SECONDS\n"
           "      First tire age losing more than SECONDS vs a fresh tire.\n"
           "  threshold --compound NAME --max-average SECONDS\n"
           "      Longest stint whose average lap stays at or under SECONDS.\n"
           "  threshold --compound NAME --slower-than OTHER\n"
           "      First tire age where NAME becomes slower than OTHER.\n"
           "  help\n"
           "      Show this text.\n"
           "\n"
           "Compound names ignore case. Race options such as --compound and\n"
           "--sector go before the word `tires`.\n";
}

int runTiresCommand(const std::vector<std::string>& args, const PitStrategy& strategy,
                    std::ostream& out, std::ostream& err) {
    try {
        if (args.empty()) {
            printTiresHelp(err);
            return kTiresUsageError;
        }
        if (args[0] == "help" || args[0] == "--help") {
            printTiresHelp(out);
            return kTiresOk;
        }
        const std::string& command = args[0];
        if (command == "profile") {
            return runProfile(args, strategy, out);
        }
        if (command == "range") {
            return runRange(args, strategy, out);
        }
        if (command == "threshold") {
            return runThreshold(args, strategy, out);
        }
        throw UsageError("unknown tires command '" + command + "'");
    } catch (const UsageError& error) {
        err << "boxbox: " << error.what() << "\n(run 'boxbox tires help' for usage)\n";
        return kTiresUsageError;
    } catch (const std::invalid_argument& error) {
        err << "boxbox: " << error.what() << "\n";
        return kTiresUsageError;
    } catch (const std::out_of_range& error) {
        err << "boxbox: " << error.what() << "\n";
        return kTiresUsageError;
    }
}

}  // namespace boxbox
