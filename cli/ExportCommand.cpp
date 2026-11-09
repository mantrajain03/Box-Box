#include "ExportCommand.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "boxbox/Common.hpp"
#include "boxbox/DriverAnalytics.hpp"
#include "boxbox/EventLog.hpp"
#include "boxbox/EventPriorityQueue.hpp"
#include "boxbox/JsonWriter.hpp"
#include "boxbox/PitAnalytics.hpp"
#include "boxbox/RaceEvent.hpp"
#include "boxbox/RaceLapAnalytics.hpp"
#include "boxbox/RaceState.hpp"
#include "boxbox/StrategyAnalytics.hpp"
#include "boxbox/TireAnalytics.hpp"
#include "boxbox/TireCompound.hpp"
#include "boxbox/Track.hpp"
#include "AlgorithmCatalog.hpp"
#include "TrackOutline.hpp"

namespace boxbox {

namespace {

/// Bundle layout version. Bump when a field is renamed or removed.
constexpr int kBundleVersion = 1;

/// Longest race the brute-force reference is run on. It enumerates every stint
/// decomposition, which grows exponentially, so it only ever sees a short race.
constexpr int kVerificationLaps = 8;

// ---------------------------------------------------------------------------
// Argument parsing
// ---------------------------------------------------------------------------

double parseNumber(const std::string& text, const std::string& what) {
    std::size_t consumed = 0;
    double value = 0.0;
    bool ok = true;
    try {
        value = std::stod(text, &consumed);
    } catch (const std::exception&) {
        ok = false;
    }
    if (!ok || consumed != text.size() || !std::isfinite(value)) {
        throw std::invalid_argument(what + ": expected a finite number, got \"" + text + "\"");
    }
    return value;
}

int parseWhole(const std::string& text, const std::string& what) {
    std::size_t consumed = 0;
    long value = 0;
    bool ok = true;
    try {
        value = std::stol(text, &consumed, 10);
    } catch (const std::exception&) {
        ok = false;
    }
    if (!ok || consumed != text.size() || value < 0L || value > 1000000L) {
        throw std::invalid_argument(what + ": expected a non-negative whole number, got \"" + text +
                                    "\"");
    }
    return static_cast<int>(value);
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

const char* eventTypeName(RaceEventType type) noexcept {
    switch (type) {
        case RaceEventType::PitEntry: return "PitEntry";
        case RaceEventType::CompoundChange: return "CompoundChange";
        case RaceEventType::PitExit: return "PitExit";
        case RaceEventType::LapCompleted: return "LapCompleted";
        case RaceEventType::PositionChange: return "PositionChange";
        case RaceEventType::OvertakeAttempt: return "OvertakeAttempt";
        case RaceEventType::Overtake: return "Overtake";
        case RaceEventType::TrafficHold: return "TrafficHold";
        case RaceEventType::RaceFinish: return "RaceFinish";
    }
    return "Unknown";
}

// ---------------------------------------------------------------------------
// Small writers shared by several sections
// ---------------------------------------------------------------------------

void writePlan(JsonWriter& json, const std::vector<PlannedStint>& plan) {
    json.beginArray();
    for (const PlannedStint& stint : plan) {
        json.beginObject()
            .key("compound").integer(static_cast<long long>(stint.compoundIndex))
            .key("laps").integer(stint.laps)
            .endObject();
    }
    json.endArray();
}

void writeResult(JsonWriter& json, const StrategyResult& result) {
    json.beginObject();
    json.key("feasible").boolean(result.feasible);
    json.key("stints").beginArray();
    for (const Stint& stint : result.stints) {
        json.beginObject()
            .key("compound").integer(static_cast<long long>(stint.compoundIndex))
            .key("name").string(stint.compoundName)
            .key("startLap").integer(stint.startLap)
            .key("laps").integer(stint.laps)
            .key("time").number(stint.time)
            .endObject();
    }
    json.endArray();
    json.key("pitLaps").beginArray();
    for (const int lap : result.pitLaps) {
        json.integer(lap);
    }
    json.endArray();
    json.key("stops").integer(result.stopCount());
    json.key("greenTime").number(result.greenTime);
    json.key("pitTime").number(result.pitTime);
    json.key("totalTime").number(result.totalTime);
    json.key("dpValue").number(result.dpValue);
    json.endObject();
}

void writeReport(JsonWriter& json, const StrategyReport& report) {
    json.beginObject();
    json.key("name").string(report.name);
    json.key("feasible").boolean(report.feasible);
    json.key("reason").string(report.reason);
    json.key("plan");
    writePlan(json, report.plan);
    json.key("stops").integer(report.stops);
    json.key("greenTime").number(report.greenTime);
    json.key("pitTime").number(report.pitTime);
    json.key("totalTime").number(report.totalTime);
    json.key("deltaVsOptimum").number(report.deltaVsOptimum);
    json.endObject();
}

// ---------------------------------------------------------------------------
// Static sections
// ---------------------------------------------------------------------------

void writeTrack(JsonWriter& json, const Track& track) {
    json.beginObject();
    json.key("name").string(track.name());
    json.key("laps").integer(track.totalLaps());
    json.key("pitLoss").number(track.pitLoss());
    json.key("baseLapTime").number(track.baseLapTime());
    json.key("sectors").beginArray();
    for (const Sector& sector : track.sectors()) {
        json.beginObject()
            .key("name").string(sector.name)
            .key("baseTime").number(sector.baseTime)
            .endObject();
    }
    json.endArray();

    // Stylised drawing for the Race Sim page. Not a real circuit, and nothing in
    // the engine depends on it (see TrackOutline.hpp).
    const TrackOutline outline = makeTrackOutline(track);
    const auto writePoints = [&json](const std::vector<OutlinePoint>& points) {
        json.beginArray();
        for (const OutlinePoint& p : points) {
            json.beginArray().number(p.x).number(p.y).endArray();
        }
        json.endArray();
    };
    json.key("outline").beginObject();
    json.key("width").number(outline.width);
    json.key("height").number(outline.height);
    json.key("points");
    writePoints(outline.points);
    json.key("sectorStart").beginArray();
    for (const std::size_t index : outline.sectorStart) {
        json.integer(static_cast<long long>(index));
    }
    json.endArray();
    json.key("pitLane");
    writePoints(outline.pitLane);
    json.key("startLine");
    writePoints({outline.lineA, outline.lineB});
    json.endObject();

    json.endObject();
}

void writeCompounds(JsonWriter& json, const std::vector<TireCompound>& compounds) {
    json.beginArray();
    for (const TireCompound& compound : compounds) {
        json.beginObject()
            .key("name").string(compound.name())
            .key("code").string(std::string(1U, compound.code()))
            .key("basePace").number(compound.basePace())
            .key("degradationPerLap").number(compound.degradationPerLap())
            .key("maxStintLaps").integer(compound.maxStintLaps())
            .key("cliffLap").integer(compound.hasCliff() ? compound.cliffLap() : -1)
            .key("cliffDegradationPerLap").number(compound.cliffDegradationPerLap())
            .endObject();
    }
    json.endArray();
}

/// stintCost(c, L) for every compound and every legal L: the prefix-sum table
/// the optimizer reads, so the frontend can show a stint's cost as a lookup.
void writeStintCostTable(JsonWriter& json, const PitStrategy& strategy) {
    json.beginArray();
    for (std::size_t c = 0; c < strategy.compounds().size(); ++c) {
        json.beginArray();
        for (int laps = 1; laps <= strategy.compounds()[c].maxStintLaps(); ++laps) {
            json.number(strategy.stintCost(c, laps));
        }
        json.endArray();
    }
    json.endArray();
}

// ---------------------------------------------------------------------------
// DP trace
// ---------------------------------------------------------------------------

/// Replays PitStrategy::optimize()'s recurrence, enumerating candidates in the
/// same order and with the same strict-improvement tie rule, but recording what
/// the optimizer does not expose: each lap's best candidate per compound.
///
/// Returns dp[totalLaps] and the final stints (compound, laps), so the caller can
/// check both against optimize() before the trace is trusted.
struct TraceOutcome {
    double finalValue = kUnreachable;
    std::vector<PlannedStint> plan;
};

TraceOutcome writeDpTrace(JsonWriter& json, const PitStrategy& strategy) {
    const int totalLaps = strategy.track().totalLaps();
    const double pitLoss = strategy.track().pitLoss();
    const std::size_t compoundCount = strategy.compounds().size();
    const std::size_t stateCount = static_cast<std::size_t>(totalLaps) + 1U;

    struct Choice {
        std::size_t compoundIndex = 0;
        int laps = 0;
    };
    std::vector<double> dp(stateCount, kUnreachable);
    std::vector<Choice> choice(stateCount);
    dp[0] = 0.0;

    json.beginArray();
    json.beginObject().key("lap").integer(0).key("cost").number(0.0).endObject();

    for (int lap = 1; lap <= totalLaps; ++lap) {
        const std::size_t lapIndex = static_cast<std::size_t>(lap);

        struct Best {
            bool any = false;
            int laps = 0;
            double cost = kUnreachable;
        };
        std::vector<Best> perCompound(compoundCount);

        for (std::size_t c = 0; c < compoundCount; ++c) {
            const int longest = std::min(lap, strategy.compounds()[c].maxStintLaps());
            for (int len = 1; len <= longest; ++len) {
                const std::size_t prev = static_cast<std::size_t>(lap - len);
                if (!isReachable(dp[prev])) {
                    continue;
                }
                const double stintCost = strategy.stintCost(c, len);
                if (!isReachable(stintCost)) {
                    continue;
                }
                const double candidate = dp[prev] + stintCost + ((lap - len > 0) ? pitLoss : 0.0);

                if (definitelyLess(candidate, perCompound[c].cost)) {
                    perCompound[c].any = true;
                    perCompound[c].laps = len;
                    perCompound[c].cost = candidate;
                }
                if (definitelyLess(candidate, dp[lapIndex])) {
                    dp[lapIndex] = candidate;
                    choice[lapIndex] = Choice{c, len};
                }
            }
        }

        json.beginObject();
        json.key("lap").integer(lap);
        if (isReachable(dp[lapIndex])) {
            json.key("cost").number(dp[lapIndex]);
            json.key("compound").integer(static_cast<long long>(choice[lapIndex].compoundIndex));
            json.key("laps").integer(choice[lapIndex].laps);
            json.key("from").integer(lap - choice[lapIndex].laps);
        } else {
            json.key("cost").null();
        }
        json.key("byCompound").beginArray();
        for (const Best& best : perCompound) {
            if (!best.any) {
                json.null();
                continue;
            }
            json.beginObject()
                .key("laps").integer(best.laps)
                .key("cost").number(best.cost)
                .endObject();
        }
        json.endArray();
        json.endObject();
    }
    json.endArray();

    TraceOutcome outcome;
    outcome.finalValue = dp[static_cast<std::size_t>(totalLaps)];
    for (int lap = totalLaps; lap > 0;) {
        const Choice step = choice[static_cast<std::size_t>(lap)];
        if (step.laps <= 0 || step.laps > lap) {
            outcome.plan.clear();
            return outcome;
        }
        outcome.plan.push_back(PlannedStint{step.compoundIndex, step.laps});
        lap -= step.laps;
    }
    std::reverse(outcome.plan.begin(), outcome.plan.end());
    return outcome;
}

/// Exhaustive reference for a short race. Written without reference to the DP,
/// so agreement between the two means something.
double bruteForceBest(const PitStrategy& strategy, int lapsLeft, bool isFirstStint,
                      long long* calls = nullptr) {
    if (calls != nullptr) {
        ++*calls;  // counted by the reference itself, for the Algorithms page's growth chart
    }
    if (lapsLeft == 0) {
        return 0.0;
    }
    double best = kUnreachable;
    for (std::size_t c = 0; c < strategy.compounds().size(); ++c) {
        const int longest = std::min(lapsLeft, strategy.compounds()[c].maxStintLaps());
        for (int length = 1; length <= longest; ++length) {
            const double rest = bruteForceBest(strategy, lapsLeft - length, false, calls);
            if (!isReachable(rest)) {
                continue;
            }
            const double candidate = (isFirstStint ? 0.0 : strategy.track().pitLoss()) +
                                     strategy.stintCost(c, length) + rest;
            if (candidate < best) {
                best = candidate;
            }
        }
    }
    return best;
}

// ---------------------------------------------------------------------------
// Tire intelligence
// ---------------------------------------------------------------------------

/// Points on the threshold slider, roughly.
constexpr int kThresholdPoints = 60;

/// Longest stint (in ages) for which the full range-query matrix is exported. The
/// matrix holds M(M+1)/2 answers per compound, so it is capped.
constexpr std::size_t kMaxRangeAges = 80;

/// Window sizes for the sliding-window view.
constexpr std::size_t kWindowSizes[] = {3U, 5U, 10U};

/// The smallest "round" step (1, 2, 2.5 or 5 times a power of ten) that is at least `raw`.
double niceStep(double raw) {
    if (!(raw > 0.0)) {
        return 0.25;
    }
    const double magnitude = std::pow(10.0, std::floor(std::log10(raw)));
    for (const double mantissa : {1.0, 2.0, 2.5, 5.0, 10.0}) {
        if (mantissa * magnitude >= raw) {
            return mantissa * magnitude;
        }
    }
    return 10.0 * magnitude;
}

void writeNumbers(JsonWriter& json, const std::vector<double>& values) {
    json.beginArray();
    for (const double v : values) {
        json.number(v);
    }
    json.endArray();
}

/// The `tires` section: each compound's degradation profile, every range query over
/// its ages, the bisection behind a "first age past a limit" query for a grid of
/// limits, and how each pair of compounds compares. Everything comes from
/// TireAnalytics; the bisection is replayed here (the core returns only the answer)
/// and checked against firstAgeDegradationExceeds before it is trusted.
bool writeTires(JsonWriter& json, const PitStrategy& strategy, std::string& failure) {
    const std::vector<DegradationProfile> profiles = buildProfiles(strategy);

    double maxDegradation = 0.0;
    for (const DegradationProfile& profile : profiles) {
        maxDegradation = std::max(maxDegradation, profile.degradationVsFresh().back());
    }
    const double step = niceStep(maxDegradation / static_cast<double>(kThresholdPoints));
    const std::size_t limitCount = static_cast<std::size_t>(std::floor(maxDegradation / step)) + 2U;
    std::vector<double> limits;
    for (std::size_t i = 0; i < limitCount; ++i) {
        limits.push_back(static_cast<double>(i) * step);
    }

    json.beginObject();
    json.key("baseLapTime").number(strategy.track().baseLapTime());

    json.key("limits");
    writeNumbers(json, limits);

    json.key("profiles").beginArray();
    for (std::size_t c = 0; c < profiles.size(); ++c) {
        const DegradationProfile& profile = profiles[c];
        const std::vector<double>& d = profile.degradationVsFresh();
        const std::size_t m = d.size();

        json.beginObject();
        json.key("compound").integer(static_cast<long long>(c));
        json.key("name").string(profile.compound());
        json.key("maxStint").integer(profile.maxStint());
        json.key("lapTime");              writeNumbers(json, profile.lapTime());
        json.key("degradationVsFresh");   writeNumbers(json, d);
        json.key("incremental");          writeNumbers(json, profile.incrementalDegradation());
        json.key("cumulative");           writeNumbers(json, profile.cumulativeDegradation());
        json.key("stintCost");            writeNumbers(json, profile.stintCost());
        json.key("averageLapTime");       writeNumbers(json, profile.averageLapTime());
        json.key("nonDecreasing").boolean(profile.lapTimeNonDecreasing());
        json.key("cliffLap").integer(profile.cliffLap() ? *profile.cliffLap() : -1);

        json.key("cliffActivationAge");
        if (profile.cliffActivationAge()) { json.integer(*profile.cliffActivationAge()); } else { json.null(); }
        json.key("cliffStepSeconds");
        if (profile.cliffStepSeconds()) { json.number(*profile.cliffStepSeconds()); } else { json.null(); }
        json.key("lapTimeChangeAfterCliff");
        if (profile.lapTimeChangeAfterCliff()) { json.number(*profile.lapTimeChangeAfterCliff()); } else { json.null(); }

        // Every range query over ages [b, e): rows by b, columns by e = b + 1 .. M.
        const TireRangeAnalyzer analyzer(profile);
        json.key("fastestAge").integer(static_cast<long long>(analyzer.fastestAge(0, m)));
        json.key("ranges");
        if (m > kMaxRangeAges) {
            json.null();
        } else {
            json.beginObject();
            const auto writeMatrix = [&](const char* name, auto query) {
                json.key(name).beginArray();
                for (std::size_t b = 0; b < m; ++b) {
                    json.beginArray();
                    for (std::size_t e = b + 1U; e <= m; ++e) {
                        json.number(query(b, e));
                    }
                    json.endArray();
                }
                json.endArray();
            };
            writeMatrix("sum", [&](std::size_t b, std::size_t e) { return analyzer.lapTimeSum(b, e); });
            writeMatrix("average", [&](std::size_t b, std::size_t e) { return analyzer.averageLapTime(b, e); });
            writeMatrix("min", [&](std::size_t b, std::size_t e) { return analyzer.minLapTime(b, e); });
            writeMatrix("degradation", [&](std::size_t b, std::size_t e) { return analyzer.degradationSum(b, e); });
            json.endObject();
        }

        // "First age whose loss against a fresh tire exceeds the limit", for each limit.
        json.key("thresholds").beginArray();
        for (const double limit : limits) {
            const ThresholdResult core = firstAgeDegradationExceeds(profile, limit);

            std::vector<std::vector<long long>> trace;
            std::optional<int> answer;
            if (profile.lapTimeNonDecreasing()) {
                std::size_t lo = 0;
                std::size_t hi = m;
                while (lo < hi) {
                    const std::size_t mid = lo + (hi - lo) / 2U;
                    const bool exceeds = definitelyLess(limit, d[mid]);
                    trace.push_back({static_cast<long long>(lo), static_cast<long long>(hi),
                                     static_cast<long long>(mid), exceeds ? 1LL : 0LL});
                    if (exceeds) { hi = mid; } else { lo = mid + 1U; }
                }
                if (lo < m) { answer = static_cast<int>(lo); }
            } else {
                answer = core.value;  // the core scans; there is no bisection to show
            }

            if (answer != core.value || core.usedBinarySearch != profile.lapTimeNonDecreasing()) {
                failure = "the bisection replay disagrees with firstAgeDegradationExceeds for " +
                          profile.compound() + " at limit " + std::to_string(limit);
                return false;
            }

            json.beginObject();
            json.key("age");
            if (answer) { json.integer(*answer); } else { json.null(); }
            json.key("binarySearch").boolean(core.usedBinarySearch);
            json.key("steps").beginArray();
            for (const std::vector<long long>& row : trace) {
                json.beginArray().integer(row[0]).integer(row[1]).integer(row[2]).integer(row[3]).endArray();
            }
            json.endArray();
            json.endObject();
        }
        json.endArray();

        json.endObject();
    }
    json.endArray();

    // How each pair of compounds compares age by age.
    json.key("comparisons").beginArray();
    for (std::size_t a = 0; a < profiles.size(); ++a) {
        for (std::size_t b = a + 1U; b < profiles.size(); ++b) {
            const CompoundComparison comparison = compareCompounds(profiles[a], profiles[b]);
            json.beginObject()
                .key("a").integer(static_cast<long long>(a))
                .key("b").integer(static_cast<long long>(b))
                .key("agesCompared").integer(comparison.agesCompared);
            json.key("firstSlowerAge");
            if (comparison.firstSlowerAge) { json.integer(*comparison.firstSlowerAge); } else { json.null(); }
            json.key("switchAges").beginArray();
            for (const int age : comparison.switchAges) {
                json.integer(age);
            }
            json.endArray();
            json.endObject();
        }
    }
    json.endArray();

    json.endObject();
    return true;
}

// ---------------------------------------------------------------------------
// Race overview
// ---------------------------------------------------------------------------

/// Number of bars in the pit-stop distribution.
constexpr int kPitBuckets = 10;

/// What the Command Center shows, taken from the core's analytics: the fastest
/// lap (selectLaps, a bounded heap), per-car summaries (DriverAnalytics), and
/// stops per lap bucket (PitAnalytics::buildStopIndex, prefix sums). The only
/// arithmetic done here is totalling and averaging the figures those return.
void writeOverview(JsonWriter& json, const PitStrategy& strategy, const LapRecorder& recorder,
                   const EventLog& log) {
    const int raceLaps = strategy.track().totalLaps();
    const DriverAnalytics analytics(recorder);

    int stops = 0;
    int laps = 0;
    double green = 0.0;
    std::vector<int> compoundLaps(strategy.compounds().size(), 0);
    for (const DriverSummary& summary : analytics.summaries()) {
        stops += summary.stops;
        laps += summary.laps;
        green += summary.greenTime;
        for (const CompoundUsage& usage : summary.compounds) {
            if (usage.compoundIndex < compoundLaps.size()) {
                compoundLaps[usage.compoundIndex] += usage.laps;
            }
        }
    }

    json.key("overview").beginObject();

    const std::vector<LapRecord> fastest =
        selectLaps(analytics.records(), 1U, LapOrder::Fastest);
    json.key("fastestLap");
    if (fastest.empty()) {
        json.null();
    } else {
        json.beginObject()
            .key("car").integer(fastest.front().driverNumber)
            .key("lap").integer(fastest.front().lap)
            .key("time").number(fastest.front().lapTime)
            .endObject();
    }

    json.key("averageLap").number(laps > 0 ? green / static_cast<double>(laps) : 0.0);
    json.key("totalStops").integer(stops);

    json.key("compoundLaps").beginArray();
    for (const int count : compoundLaps) {
        json.integer(count);
    }
    json.endArray();

    const PitAnalytics pit(log);
    const StopLapIndex stopIndex = pit.buildStopIndex(raceLaps);
    const int width = (raceLaps + kPitBuckets - 1) / kPitBuckets;
    json.key("pitBuckets").beginArray();
    for (int from = 1; from <= raceLaps; from += width) {
        const int to = std::min(from + width - 1, raceLaps);
        json.beginObject()
            .key("from").integer(from)
            .key("to").integer(to)
            .key("stops").integer(stopIndex.stopsInLaps(from, to))
            .endObject();
    }
    json.endArray();

    // Field mean and quickest lap time on each lap of the race.
    std::vector<double> lapSum(static_cast<std::size_t>(raceLaps), 0.0);
    std::vector<int> lapCount(static_cast<std::size_t>(raceLaps), 0);
    std::vector<double> lapBest(static_cast<std::size_t>(raceLaps), kUnreachable);
    for (const LapRecord& record : analytics.records()) {
        const std::size_t i = static_cast<std::size_t>(record.lap - 1);
        lapSum[i] += record.lapTime;
        ++lapCount[i];
        if (record.lapTime < lapBest[i]) {
            lapBest[i] = record.lapTime;
        }
    }
    json.key("lapTrend").beginArray();
    for (int lap = 1; lap <= raceLaps; ++lap) {
        const std::size_t i = static_cast<std::size_t>(lap - 1);
        json.beginObject()
            .key("lap").integer(lap)
            .key("average").number(lapCount[i] > 0 ? lapSum[i] / static_cast<double>(lapCount[i]) : 0.0)
            .key("fastest").number(lapBest[i])
            .endObject();
    }
    json.endArray();

    json.key("drivers").beginArray();
    for (const DriverSummary& summary : analytics.summaries()) {
        json.beginObject()
            .key("car").integer(summary.driverNumber)
            .key("name").string(summary.name)
            .key("laps").integer(summary.laps)
            .key("totalTime").number(summary.totalTime)
            .key("greenTime").number(summary.greenTime)
            .key("pitTime").number(summary.pitTime)
            .key("trafficTime").number(summary.trafficTime)
            .key("stops").integer(summary.stops)
            .key("averageLap").number(summary.averageLap)
            .key("bestLap").beginObject()
                .key("lap").integer(summary.bestLap.lap)
                .key("time").number(summary.bestLap.lapTime)
                .endObject()
            .key("worstLap").beginObject()
                .key("lap").integer(summary.worstLap.lap)
                .key("time").number(summary.worstLap.lapTime)
                .endObject()
            .key("lapRange").number(summary.lapRange)
            .key("meanAbsDeviation").number(summary.meanAbsDeviation)
            .endObject();
    }
    json.endArray();

    // Sliding windows over each car's lap times: quickest and slowest lap in the last
    // W laps from two monotonic deques, and the mean from prefix sums (trailingWindows).
    json.key("windows").beginObject();
    json.key("sizes").beginArray();
    for (const std::size_t size : kWindowSizes) {
        json.integer(static_cast<long long>(size));
    }
    json.endArray();
    json.key("cars").beginArray();
    for (const DriverSummary& summary : analytics.summaries()) {
        const std::vector<double>& lapTimes = analytics.lapTimes(summary.driverNumber);
        json.beginObject();
        json.key("car").integer(summary.driverNumber);
        json.key("lapTimes");
        writeNumbers(json, lapTimes);
        for (const std::size_t size : kWindowSizes) {
            const std::vector<LapWindow> windows = trailingWindows(lapTimes, size);
            std::vector<double> fastest, slowest, average, fastestLap, slowestLap;
            for (const LapWindow& w : windows) {
                fastest.push_back(w.fastest);
                slowest.push_back(w.slowest);
                average.push_back(w.average);
                fastestLap.push_back(static_cast<double>(w.fastestLap));
                slowestLap.push_back(static_cast<double>(w.slowestLap));
            }
            json.key("w" + std::to_string(size)).beginObject();
            json.key("fastest");     writeNumbers(json, fastest);
            json.key("slowest");     writeNumbers(json, slowest);
            json.key("average");     writeNumbers(json, average);
            json.key("fastestLap");  writeNumbers(json, fastestLap);
            json.key("slowestLap");  writeNumbers(json, slowestLap);
            json.endObject();
        }
        json.endObject();
    }
    json.endArray();
    json.endObject();

    json.endObject();
}

// ---------------------------------------------------------------------------
// Driver analytics: rankings, top laps, sectors, and range queries
// ---------------------------------------------------------------------------

/// Races longer than this export no range-query matrices or tree node lists: they hold
/// one answer per (first lap, last lap) pair, per car.
constexpr int kMaxRangeLaps = 100;

constexpr std::size_t kTopLapSizes[] = {3U, 5U, 10U};

/// A tree node, as the page draws it: its id and the laps (1-based, inclusive) it covers.
struct NodeRef {
    std::size_t id = 0;
    std::size_t firstLap = 0;
    std::size_t lastLap = 0;
};

/// Which segment-tree nodes a range query touches. SegmentTree does not expose its
/// nodes, but its layout is documented (iterative, bottom-up, leaves at n .. 2n-1,
/// node i combines 2i and 2i+1) and rangeSum is the standard walk. This replays that
/// walk; verifyTreeLayouts checks the result against the real tree's answers.
class SegmentLayout {
public:
    explicit SegmentLayout(std::size_t n) : n_(n), lo_(2U * n, 0U), hi_(2U * n, 0U) {
        for (std::size_t i = n; i < 2U * n; ++i) {
            lo_[i] = hi_[i] = i - n;
        }
        for (std::size_t i = n; i-- > 1U;) {
            lo_[i] = std::min(lo_[2U * i], lo_[2U * i + 1U]);
            hi_[i] = std::max(hi_[2U * i], hi_[2U * i + 1U]);
        }
    }

    /// The nodes whose sums add up to laps [begin, end), 0-based and half-open.
    std::vector<NodeRef> nodes(std::size_t begin, std::size_t end) const {
        std::vector<NodeRef> out;
        for (std::size_t lo = begin + n_, hi = end + n_; lo < hi; lo /= 2U, hi /= 2U) {
            if ((lo & 1U) != 0U) { out.push_back(ref(lo++)); }
            if ((hi & 1U) != 0U) { out.push_back(ref(--hi)); }
        }
        return out;
    }

private:
    NodeRef ref(std::size_t id) const { return NodeRef{id, lo_[id] + 1U, hi_[id] + 1U}; }

    std::size_t n_;
    std::vector<std::size_t> lo_;
    std::vector<std::size_t> hi_;
};

/// The Fenwick nodes prefixSum(count) adds: i = count, then i minus its lowest set bit.
/// Node i covers laps i - lowbit(i) + 1 .. i.
std::vector<NodeRef> fenwickNodes(std::size_t count) {
    std::vector<NodeRef> out;
    for (std::size_t i = count; i > 0U; i -= (i & (~i + 1U))) {
        const std::size_t low = i & (~i + 1U);
        out.push_back(NodeRef{i, i - low + 1U, i});
    }
    return out;
}

void writeNodes(JsonWriter& json, const std::vector<NodeRef>& nodes) {
    json.beginArray();
    for (const NodeRef& node : nodes) {
        json.beginArray()
            .integer(static_cast<long long>(node.id))
            .integer(static_cast<long long>(node.firstLap))
            .integer(static_cast<long long>(node.lastLap))
            .endArray();
    }
    json.endArray();
}

/// The `structures` section: which nodes of the segment tree and of the Fenwick tree a
/// query over any lap range touches. They depend only on the race length.
void writeStructures(JsonWriter& json, std::size_t n) {
    json.beginObject();
    json.key("raceLaps").integer(static_cast<long long>(n));

    const SegmentLayout layout(n);
    json.key("segmentNodes").beginArray();
    for (std::size_t b = 0; b < n; ++b) {
        json.beginArray();
        for (std::size_t e = b + 1U; e <= n; ++e) {
            writeNodes(json, layout.nodes(b, e));
        }
        json.endArray();
    }
    json.endArray();

    json.key("fenwickNodes").beginArray();
    for (std::size_t k = 0; k <= n; ++k) {
        writeNodes(json, fenwickNodes(k));
    }
    json.endArray();

    json.endObject();
}

/// Rebuild both trees from the same numbers the real ones were fed, add up the replayed
/// nodes, and compare with what the real trees answer, for every lap range of every car.
/// A node list that is wrong would give a different number, so agreement is evidence.
bool verifyTreeLayouts(const std::vector<std::vector<double>>& lapTimes,
                       const std::vector<std::vector<double>>& traffic,
                       const RaceLapRecorder& lapIndex, std::string& failure) {
    for (std::size_t car = 0; car < lapTimes.size(); ++car) {
        const std::size_t n = lapTimes[car].size();
        const SegmentLayout layout(n);

        std::vector<double> sums(2U * n, 0.0);
        for (std::size_t j = 0; j < n; ++j) { sums[n + j] = lapTimes[car][j]; }
        for (std::size_t i = n; i-- > 1U;) { sums[i] = sums[2U * i] + sums[2U * i + 1U]; }

        std::vector<double> fenwick(n + 1U, 0.0);
        for (std::size_t lap = 1; lap <= n; ++lap) {
            for (std::size_t i = lap; i <= n; i += (i & (~i + 1U))) { fenwick[i] += traffic[car][lap - 1U]; }
        }
        const auto prefix = [&](std::size_t count) {
            double total = 0.0;
            for (const NodeRef& node : fenwickNodes(count)) { total += fenwick[node.id]; }
            return total;
        };

        for (std::size_t b = 0; b < n; ++b) {
            for (std::size_t e = b + 1U; e <= n; ++e) {
                double viaNodes = 0.0;
                std::size_t covered = 0;
                for (const NodeRef& node : layout.nodes(b, e)) {
                    viaNodes += sums[node.id];
                    covered += node.lastLap - node.firstLap + 1U;
                }
                const double real = lapIndex.carLaps(car).totalTime(static_cast<int>(b) + 1, static_cast<int>(e));
                const double realTraffic = lapIndex.trafficLoss(car, static_cast<int>(b) + 1, static_cast<int>(e));
                if (covered != e - b || std::fabs(viaNodes - real) > kClosureTolerance ||
                    std::fabs(prefix(e) - prefix(b) - realTraffic) > kClosureTolerance) {
                    failure = "the replayed tree nodes disagree with the real segment or Fenwick tree for car index " +
                              std::to_string(car) + ", laps " + std::to_string(b + 1U) + " to " + std::to_string(e);
                    return false;
                }
            }
        }
    }
    return true;
}

void writeLapRef(JsonWriter& json, const LapRecord& lap) {
    json.beginObject()
        .key("car").integer(lap.driverNumber)
        .key("lap").integer(lap.lap)
        .key("time").number(lap.lapTime)
        .key("compound").integer(static_cast<long long>(lap.compoundIndex))
        .endObject();
}

/// The K fastest and K slowest laps from selectLaps, a bounded heap, optionally for one car.
void writeTopSet(JsonWriter& json, const std::vector<LapRecord>& records, std::size_t k,
                 const std::optional<int>& car) {
    LapFilter filter;
    filter.driverNumber = car;
    json.beginObject();
    json.key("fastest").beginArray();
    for (const LapRecord& lap : selectLaps(records, k, LapOrder::Fastest, filter)) { writeLapRef(json, lap); }
    json.endArray();
    json.key("slowest").beginArray();
    for (const LapRecord& lap : selectLaps(records, k, LapOrder::Slowest, filter)) { writeLapRef(json, lap); }
    json.endArray();
    json.endObject();
}

/// The `analytics` object of one run: rankings, top laps, modelled sector statistics and
/// the per-car range-query answers. Everything is the core's own result.
bool writeDriverAnalytics(JsonWriter& json, const PitStrategy& strategy, const LapRecorder& recorder,
                          const RaceLapRecorder& lapIndex, const OvertakeRule& rule, std::string& failure) {
    const DriverAnalytics analytics(recorder);
    const std::vector<DriverSummary>& summaries = analytics.summaries();
    const std::size_t raceLaps = static_cast<std::size_t>(strategy.track().totalLaps());

    json.key("analytics").beginObject();

    // Pace and consistency rankings: car numbers, best first, explicit tie-breaks.
    json.key("rankings").beginObject();
    const std::pair<const char*, RankMetric> metrics[] = {
        {"averageLap", RankMetric::AverageLap}, {"bestLap", RankMetric::BestLap},
        {"lapRange", RankMetric::LapRange}, {"meanAbsDeviation", RankMetric::MeanAbsDeviation}};
    for (const auto& metric : metrics) {
        json.key(metric.first).beginArray();
        for (const int car : rankDrivers(summaries, metric.second)) { json.integer(car); }
        json.endArray();
    }
    json.endObject();

    // Fastest and slowest laps, for the field and for each car.
    json.key("topLaps").beginObject();
    json.key("sizes").beginArray();
    for (const std::size_t k : kTopLapSizes) { json.integer(static_cast<long long>(k)); }
    json.endArray();
    json.key("field").beginObject();
    for (const std::size_t k : kTopLapSizes) {
        json.key("k" + std::to_string(k));
        writeTopSet(json, analytics.records(), k, std::nullopt);
    }
    json.endObject();
    json.key("cars").beginArray();
    for (const DriverSummary& summary : summaries) {
        json.beginObject().key("car").integer(summary.driverNumber);
        for (const std::size_t k : kTopLapSizes) {
            json.key("k" + std::to_string(k));
            writeTopSet(json, analytics.records(), k, summary.driverNumber);
        }
        json.endObject();
    }
    json.endArray();
    json.endObject();

    // Sector statistics. MODELLED: the simulator times whole laps, and a lap is split in
    // proportion to the track's sector times, so these add no information beyond lap times.
    const std::vector<DriverSectorSummary> sectors = sectorSummaries(analytics);
    bool anySectors = false;
    for (const DriverSectorSummary& s : sectors) { anySectors = anySectors || s.available; }
    json.key("sectors");
    if (!anySectors) {
        json.null();
    } else {
        json.beginObject();
        json.key("modelled").boolean(true);
        json.key("names").beginArray();
        for (const Sector& sector : strategy.track().sectors()) { json.string(sector.name); }
        json.endArray();
        json.key("cars").beginArray();
        for (std::size_t i = 0; i < sectors.size(); ++i) {
            const DriverSectorSummary& s = sectors[i];
            json.beginObject().key("car").integer(summaries[i].driverNumber).key("available").boolean(s.available);
            json.key("fastest").beginArray();
            for (const SectorBest& best : s.fastest) {
                json.beginObject().key("lap").integer(best.lap).key("time").number(best.time).endObject();
            }
            json.endArray();
            json.key("average");
            writeNumbers(json, s.average);
            json.key("bestCombinedLap").number(s.bestCombinedLap);
            json.endObject();
        }
        json.endArray();
        json.key("fieldBest").beginArray();
        for (const FieldSectorBest& best : fieldBestSectors(analytics)) {
            json.beginObject().key("car").integer(best.driverNumber).key("lap").integer(best.lap)
                .key("time").number(best.time).endObject();
        }
        json.endArray();
        json.key("fieldBestCombinedLap").number(fieldBestCombinedLap(analytics));
        json.key("pairs").beginArray();
        for (const DriverSummary& a : summaries) {
            for (const DriverSummary& b : summaries) {
                if (a.driverNumber == b.driverNumber) { continue; }
                json.beginObject().key("a").integer(a.driverNumber).key("b").integer(b.driverNumber);
                json.key("rows").beginArray();
                for (const SectorComparisonRow& row : compareSectors(analytics, a.driverNumber, b.driverNumber)) {
                    json.beginObject()
                        .key("sector").integer(static_cast<long long>(row.sector))
                        .key("fastestA").number(row.fastestA).key("fastestB").number(row.fastestB)
                        .key("averageA").number(row.averageA).key("averageB").number(row.averageB)
                        .key("averageGap").number(row.averageGap)
                        .endObject();
                }
                json.endArray();
                json.endObject();
            }
        }
        json.endArray();
        json.endObject();
    }

    // Range queries over laps, for every car: the segment tree's sum and quickest lap, and
    // the Fenwick tree's traffic loss, for every (first lap, last lap).
    json.key("ranges");
    if (raceLaps > static_cast<std::size_t>(kMaxRangeLaps)) {
        json.null();
    } else {
        std::vector<std::vector<double>> lapTimes;
        std::vector<std::vector<double>> traffic;
        for (const DriverSummary& summary : summaries) {
            lapTimes.push_back(analytics.lapTimes(summary.driverNumber));
            std::vector<double> perLap(raceLaps, 0.0);
            for (const LapRecord& record : analytics.records()) {
                if (record.driverNumber == summary.driverNumber) { perLap[static_cast<std::size_t>(record.lap - 1)] = record.trafficTime; }
            }
            traffic.push_back(perLap);
        }
        if (!verifyTreeLayouts(lapTimes, traffic, lapIndex, failure)) {
            return false;
        }

        json.beginObject();
        json.key("cars").beginArray();
        for (std::size_t car = 0; car < summaries.size(); ++car) {
            json.beginObject().key("car").integer(summaries[car].driverNumber);
            const auto writeMatrix = [&](const char* name, auto query) {
                json.key(name).beginArray();
                for (std::size_t b = 0; b < raceLaps; ++b) {
                    json.beginArray();
                    for (std::size_t e = b + 1U; e <= raceLaps; ++e) { query(b, e); }
                    json.endArray();
                }
                json.endArray();
            };
            writeMatrix("sum", [&](std::size_t b, std::size_t e) {
                json.number(lapIndex.carLaps(car).totalTime(static_cast<int>(b) + 1, static_cast<int>(e)));
            });
            writeMatrix("fastestLap", [&](std::size_t b, std::size_t e) {
                json.integer(lapIndex.carLaps(car).fastestLap(static_cast<int>(b) + 1, static_cast<int>(e)).lap);
            });
            json.key("traffic");
            if (!rule.enabled) {
                json.null();  // with overtaking off every loss is exactly zero
            } else {
                json.beginArray();
                for (std::size_t b = 0; b < raceLaps; ++b) {
                    json.beginArray();
                    for (std::size_t e = b + 1U; e <= raceLaps; ++e) {
                        json.number(lapIndex.trafficLoss(car, static_cast<int>(b) + 1, static_cast<int>(e)));
                    }
                    json.endArray();
                }
                json.endArray();
            }
            json.endObject();
        }
        json.endArray();
        json.endObject();
    }

    json.endObject();
    return true;
}

// ---------------------------------------------------------------------------
// Algorithms: the catalog, and operation counts (counted, never timed)
// ---------------------------------------------------------------------------

/// Longest race the brute-force reference is counted on. Its calls grow roughly 4x per lap
/// with three compounds, so this keeps the export fast.
constexpr int kGrowthLaps = 12;

/// How many candidates optimize() evaluates for an n-lap race: for each lap, one per legal
/// stint length per compound. The same loop bounds as the optimizer's inner loops.
long long dpTransitions(const PitStrategy& strategy, int laps) {
    long long count = 0;
    for (int lap = 1; lap <= laps; ++lap) {
        for (const TireCompound& compound : strategy.compounds()) {
            count += std::min(lap, compound.maxStintLaps());
        }
    }
    return count;
}

/// What a range query costs on average, over every range of an n-lap race: the segment-tree
/// nodes added, the Fenwick nodes added and subtracted, and the laps a plain scan would read.
struct RangeCostMeans {
    double segmentNodes = 0.0;
    double fenwickNodes = 0.0;
    double scanLaps = 0.0;
    std::size_t segmentMax = 0;
};

RangeCostMeans rangeCostMeans(std::size_t n) {
    const SegmentLayout layout(n);
    double segment = 0.0, fenwick = 0.0, scan = 0.0;
    std::size_t queries = 0, segmentMax = 0;
    for (std::size_t b = 0; b < n; ++b) {
        for (std::size_t e = b + 1U; e <= n; ++e) {
            const std::size_t nodes = layout.nodes(b, e).size();
            segment += static_cast<double>(nodes);
            segmentMax = std::max(segmentMax, nodes);
            fenwick += static_cast<double>(fenwickNodes(e).size() + fenwickNodes(b).size());
            scan += static_cast<double>(e - b);
            ++queries;
        }
    }
    RangeCostMeans means;
    const double q = static_cast<double>(queries);
    means.segmentNodes = segment / q;
    means.fenwickNodes = fenwick / q;
    means.scanLaps = scan / q;
    means.segmentMax = segmentMax;
    return means;
}

/// The `algorithms` section: the catalog (with the source files each entry cites), the
/// brute-force reference's calls against the DP's candidate evaluations for races of 1 to
/// kGrowthLaps laps, and the average work of a range query against a scan. Everything here is
/// a count, so it is the same on every machine; `make bench` times the same structures.
void writeAlgorithms(JsonWriter& json, const PitStrategy& strategy) {
    json.beginObject();

    json.key("catalog").beginArray();
    for (const AlgorithmInfo& info : algorithmCatalog()) {
        json.beginObject()
            .key("id").string(info.id)
            .key("name").string(info.name)
            .key("category").string(info.category)
            .key("purpose").string(info.purpose);
        json.key("files").beginArray();
        for (const std::string& file : info.files) { json.string(file); }
        json.endArray();
        json.key("costs").beginArray();
        for (const AlgorithmCost& cost : info.costs) {
            json.beginObject().key("operation").string(cost.operation).key("cost").string(cost.cost).endObject();
        }
        json.endArray();
        json.key("note").string(info.note);
        json.key("usedIn").beginArray();
        for (const std::string& page : info.usedIn) { json.string(page); }
        json.endArray();
        json.endObject();
    }
    json.endArray();

    json.key("growth").beginObject();

    json.key("dp").beginObject();
    json.key("laps").beginArray();
    for (int laps = 1; laps <= kGrowthLaps; ++laps) { json.integer(laps); }
    json.endArray();
    json.key("bruteForceCalls").beginArray();
    for (int laps = 1; laps <= kGrowthLaps; ++laps) {
        long long calls = 0;
        bruteForceBest(strategy, laps, true, &calls);
        json.integer(calls);
    }
    json.endArray();
    json.key("dpTransitions").beginArray();
    for (int laps = 1; laps <= kGrowthLaps; ++laps) { json.integer(dpTransitions(strategy, laps)); }
    json.endArray();
    json.key("raceLaps").integer(strategy.track().totalLaps());
    json.key("raceTransitions").integer(dpTransitions(strategy, strategy.track().totalLaps()));
    json.endObject();

    json.key("ranges");
    const int raceLaps = strategy.track().totalLaps();
    if (raceLaps > kMaxRangeLaps) {
        json.null();
    } else {
        std::vector<std::size_t> sizes;
        for (std::size_t n = 10U; n <= static_cast<std::size_t>(kMaxRangeLaps); n += 10U) { sizes.push_back(n); }
        sizes.push_back(static_cast<std::size_t>(raceLaps));
        std::sort(sizes.begin(), sizes.end());
        sizes.erase(std::unique(sizes.begin(), sizes.end()), sizes.end());

        json.beginObject().key("points").beginArray();
        for (const std::size_t n : sizes) {
            const RangeCostMeans means = rangeCostMeans(n);
            json.beginObject()
                .key("laps").integer(static_cast<long long>(n))
                .key("segmentMean").number(means.segmentNodes)
                .key("segmentMax").integer(static_cast<long long>(means.segmentMax))
                .key("fenwickMean").number(means.fenwickNodes)
                .key("scanMean").number(means.scanLaps)
                .endObject();
        }
        json.endArray().endObject();
    }

    json.endObject();  // growth
    json.endObject();
}

// ---------------------------------------------------------------------------
// Pit and event analytics
// ---------------------------------------------------------------------------

constexpr std::size_t kStopBucketSizes[] = {1U, 3U, 5U};
constexpr int kMaxGreedySlack = 10;        ///< "each stop could move this many laps either way"
constexpr int kTrafficLapsAfterStop = 3;   ///< laps counted after a stop for traffic
constexpr int kStintHistogramBin = 5;

/// The `pit` object of one run, all from PitAnalytics and the event log: per-car stops and
/// stints, stops per lap bucket (prefix sums), the field's stint statistics, the stops in
/// time order, a greedy "fewest pit calls" plan for each stop-window width, and how far out of
/// order the event queue received its events (an inversion count, from a Fenwick tree).
bool writePitAnalytics(JsonWriter& json, const PitStrategy& strategy, const LapRecorder& recorder,
                       const EventLog& log, std::string& failure) {
    const DriverAnalytics analytics(recorder);
    const PitAnalytics pit(log);
    const int raceLaps = strategy.track().totalLaps();
    const double pitLoss = strategy.track().pitLoss();

    json.key("pit").beginObject();

    // Per car: stops, stints, pit time, traffic in the laps after each stop.
    const std::vector<TrafficAfterStop> after =
        PitAnalytics::trafficAfterStops(analytics.records(), kTrafficLapsAfterStop);
    json.key("drivers").beginArray();
    for (const DriverPitSummary& summary : pit.allSummaries()) {
        const CoverageReport coverage =
            PitAnalytics::checkStintCoverage(PitAnalytics::stintIntervals(summary), raceLaps);
        if (!coverage.tiles) {
            failure = "car " + std::to_string(summary.driverNumber) + "'s stints do not tile the race: " + coverage.message;
            return false;
        }
        json.beginObject().key("car").integer(summary.driverNumber).key("stops").integer(summary.stopCount);
        json.key("stopLaps").beginArray();
        for (const int lap : summary.stopLaps) { json.integer(lap); }
        json.endArray();
        json.key("stintLengths").beginArray();
        for (const int laps : summary.stintLengthsLaps) { json.integer(laps); }
        json.endArray();
        json.key("compounds").beginArray();
        for (const int compound : summary.compoundIndicesUsed) { json.integer(compound); }
        json.endArray();
        json.key("pitTime").number(pit.totalPitLoss(summary.driverNumber, pitLoss));

        double trafficAfterStops = 0.0;
        json.key("trafficAfter").beginArray();
        for (const TrafficAfterStop& t : after) {
            if (t.driverNumber != summary.driverNumber) { continue; }
            json.beginObject().key("lap").integer(t.stopLap).key("laps").integer(t.lapsConsidered)
                .key("seconds").number(t.trafficSeconds).endObject();
            trafficAfterStops += t.trafficSeconds;
        }
        json.endArray();
        json.key("trafficAfterTotal").number(trafficAfterStops);
        json.endObject();
    }
    json.endArray();

    // The field as a whole.
    const StintStats stints = pit.fieldStintStats();
    json.key("field").beginObject();
    json.key("pitTime").number(pit.fieldPitLoss(pitLoss));
    json.key("stints").beginObject()
        .key("count").integer(stints.count).key("shortest").integer(stints.shortestLaps)
        .key("longest").integer(stints.longestLaps).key("mean").number(stints.meanLaps).endObject();
    json.key("stintHistogram").beginArray();
    for (const StintHistogramBin& bin : pit.stintHistogram(kStintHistogramBin)) {
        json.beginObject().key("from").integer(bin.firstLaps).key("to").integer(bin.lastLaps)
            .key("count").integer(bin.count).endObject();
    }
    json.endArray();
    json.endObject();

    // Stops per lap bucket, each answered in O(1) from the prefix-sum index.
    const StopLapIndex index = pit.buildStopIndex(raceLaps);
    json.key("stopBuckets").beginObject();
    for (const std::size_t size : kStopBucketSizes) {
        json.key("b" + std::to_string(size)).beginArray();
        for (int from = 1; from <= raceLaps; from += static_cast<int>(size)) {
            const int to = std::min(from + static_cast<int>(size) - 1, raceLaps);
            json.beginObject().key("from").integer(from).key("to").integer(to)
                .key("stops").integer(index.stopsInLaps(from, to)).endObject();
        }
        json.endArray();
    }
    json.endObject();

    // Every stop, in time order.
    const std::vector<PitStopRecord> stops = pit.chronologicalPitStops();
    json.key("stops").beginArray();
    for (const PitStopRecord& stop : stops) {
        json.beginObject().key("car").integer(stop.driverNumber).key("lap").integer(stop.lap)
            .key("time").number(stop.time).key("from").integer(stop.fromCompound)
            .key("to").integer(stop.toCompound).endObject();
    }
    json.endArray();

    const PitAnalytics::PitWindow window = pit.pitWindow();
    json.key("window").beginObject().key("hasStops").boolean(window.hasStops)
        .key("earliest").integer(window.earliestStopLap).key("latest").integer(window.latestStopLap).endObject();

    // Greedy interval stabbing: if every stop could move `slack` laps either way, the fewest
    // laps that put a pit call inside every window.
    json.key("greedy").beginArray();
    for (int slack = 0; slack <= kMaxGreedySlack; ++slack) {
        std::vector<StopWindow> windows;
        for (const PitStopRecord& stop : stops) {
            StopWindow w;
            w.driverNumber = stop.driverNumber;
            w.earliestLap = std::max(1, stop.lap - slack);
            w.latestLap = std::max(w.earliestLap, std::min(raceLaps - 1, stop.lap + slack));
            windows.push_back(w);
        }
        json.beginObject().key("slack").integer(slack).key("windows").beginArray();
        for (const StopWindow& w : windows) {
            json.beginObject().key("car").integer(w.driverNumber).key("earliest").integer(w.earliestLap)
                .key("latest").integer(w.latestLap).endObject();
        }
        json.endArray().key("calls").beginArray();
        for (const int lap : PitAnalytics::minimumStopLaps(windows)) { json.integer(lap); }
        json.endArray().endObject();
    }
    json.endArray();

    // How out of order the queue's input was. Events get a sequence number as they are
    // pushed, and the log is in the order they left; the inversions are the pairs the heap
    // had to put right.
    std::vector<int> pushedOrder, poppedOrder;
    for (const RaceEvent& event : log.entries()) { poppedOrder.push_back(static_cast<int>(event.sequenceId)); }
    pushedOrder = poppedOrder;
    std::sort(pushedOrder.begin(), pushedOrder.end());
    json.key("queue").beginObject()
        .key("events").integer(static_cast<long long>(log.size()))
        .key("inversions").integer(static_cast<long long>(countOrderSwaps(pushedOrder, poppedOrder)))
        .endObject();

    json.endObject();
    return true;
}

// ---------------------------------------------------------------------------
// Race replay
// ---------------------------------------------------------------------------

/// Replay the race for the optimal car plus one single-compound reference car
/// per compound (the CLI's --rivals field), writing one lap-by-lap run.
/// Returns false, with the reason in `failure`, when the event log is illegal.
bool writeRun(JsonWriter& json, const PitStrategy& strategy, const OvertakeRule& rule,
              const StrategyResult& optimal, std::string& failure) {
    RaceSimulator simulator(strategy.track(), strategy.compounds());
    simulator.setOvertakeRule(rule);

    EventPriorityQueue queue;
    simulator.setEventSink(&queue);

    simulator.addDriver(1, "OPTIMAL", optimal);
    int nextNumber = 2;
    for (std::size_t c = 0; c < strategy.compounds().size(); ++c) {
        const StrategyResult reference = strategy.singleCompoundPlan(c);
        if (reference.feasible) {
            simulator.addDriver(nextNumber, "ALL-" + strategy.compounds()[c].name(), reference);
            ++nextNumber;
        }
    }

    LapRecorder recorder(simulator);  // watches from the grid, before lap 1
    RaceLapRecorder lapIndex(simulator);  // the segment and Fenwick trees over each car's laps

    json.beginObject();

    json.key("overtake").beginObject()
        .key("enabled").boolean(rule.enabled)
        .key("minGap").number(rule.minGap)
        .key("passMargin").number(rule.passMargin)
        .endObject();

    json.key("drivers").beginArray();
    for (const Driver& driver : simulator.drivers()) {
        json.beginObject();
        json.key("car").integer(driver.number());
        json.key("name").string(driver.name());
        json.key("plan").beginArray();
        for (const Stint& stint : driver.plan()) {
            json.beginObject()
                .key("compound").integer(static_cast<long long>(stint.compoundIndex))
                .key("startLap").integer(stint.startLap)
                .key("laps").integer(stint.laps)
                .endObject();
        }
        json.endArray();
        json.endObject();
    }
    json.endArray();

    // One entry per lap round, cars in running order. Compound and tire age are
    // those the lap was RUN on; a car that stops is switched to its next set
    // only after the lap, so they are read before advanceLap().
    json.key("laps").beginArray();
    while (!simulator.finished()) {
        std::vector<std::size_t> compoundBefore;
        std::vector<int> ageBefore;
        std::vector<int> stopsBefore;
        for (const Driver& driver : simulator.drivers()) {
            compoundBefore.push_back(driver.currentCompoundIndex());
            ageBefore.push_back(driver.tireAge());
            stopsBefore.push_back(driver.stopsMade());
        }

        if (!simulator.advanceLap()) {
            break;
        }
        recorder.observe(simulator);
        lapIndex.observe(simulator);

        json.beginObject();
        json.key("lap").integer(simulator.lapsRun());
        json.key("order").beginArray();
        for (const Standing& row : simulator.standings()) {
            const Driver& driver = simulator.drivers()[row.driverIndex];
            const std::size_t i = row.driverIndex;
            const bool pitted = driver.stopsMade() > stopsBefore[i];
            json.beginObject()
                .key("car").integer(driver.number())
                .key("position").integer(row.position)
                .key("gap").number(row.gapToLeader)
                // `cross` is when the car crossed the line to end this lap; `time` is the
                // race clock after the lap, which also includes the pit loss if it stopped.
                .key("cross").number(pitted ? driver.cumulativeTime() - strategy.track().pitLoss()
                                            : driver.cumulativeTime())
                .key("time").number(driver.cumulativeTime())
                .key("lapTime").number(driver.lastLapTime())
                .key("compound").integer(static_cast<long long>(compoundBefore[i]))
                .key("age").integer(ageBefore[i])
                .key("fitted").integer(static_cast<long long>(driver.currentCompoundIndex()))
                .key("fittedAge").integer(driver.tireAge())
                .key("pitted").boolean(pitted)
                .key("stops").integer(driver.stopsMade())
                .key("traffic").number(driver.trafficLoss())
                .endObject();
        }
        json.endArray();
        json.endObject();
    }
    json.endArray();

    // Events leave the queue in the canonical order and are logged as they come.
    EventLog log;
    while (!queue.empty()) {
        log.record(queue.pop());
    }
    const EventLogValidation check = validateEventLog(log);
    if (!check.valid) {
        failure = "event log failed state-machine validation at entry " +
                  std::to_string(check.badIndex) + ": " + check.message;
        return false;
    }

    json.key("eventLogValid").boolean(true);
    writeOverview(json, strategy, recorder, log);
    if (!writeDriverAnalytics(json, strategy, recorder, lapIndex, rule, failure)) {
        return false;
    }
    if (!writePitAnalytics(json, strategy, recorder, log, failure)) {
        return false;
    }
    json.key("events").beginArray();
    for (const RaceEvent& event : log.entries()) {
        json.beginObject();
        json.key("type").string(eventTypeName(event.type));
        json.key("time").number(event.timestamp);
        json.key("lap").integer(event.lap);
        json.key("car").integer(event.driverNumber);
        json.key("seq").integer(static_cast<long long>(event.sequenceId));
        switch (event.type) {
            case RaceEventType::CompoundChange:
                json.key("from").integer(event.oldCompoundIndex);
                json.key("to").integer(event.newCompoundIndex);
                break;
            case RaceEventType::LapCompleted:
                json.key("compound").integer(event.oldCompoundIndex);
                break;
            case RaceEventType::PositionChange:
                json.key("oldPosition").integer(event.oldPosition);
                json.key("newPosition").integer(event.newPosition);
                break;
            case RaceEventType::OvertakeAttempt:
            case RaceEventType::Overtake:
                json.key("other").integer(event.otherDriverNumber);
                break;
            case RaceEventType::TrafficHold:
                json.key("other").integer(event.otherDriverNumber);
                json.key("loss").number(event.holdLossSeconds);
                break;
            case RaceEventType::PitEntry:
            case RaceEventType::PitExit:
            case RaceEventType::RaceFinish:
                break;
        }
        json.endObject();
    }
    json.endArray();

    // When the last car finishes: the length of the replay.
    double duration = 0.0;
    for (const Driver& driver : simulator.drivers()) {
        duration = std::max(duration, driver.cumulativeTime());
    }
    json.key("duration").number(duration);

    json.key("final").beginArray();
    for (const Standing& row : simulator.standings()) {
        const Driver& driver = simulator.drivers()[row.driverIndex];
        json.beginObject()
            .key("car").integer(driver.number())
            .key("position").integer(row.position)
            .key("time").number(driver.cumulativeTime())
            .key("traffic").number(driver.trafficLoss())
            .endObject();
    }
    json.endArray();

    json.endObject();
    return true;
}

// ---------------------------------------------------------------------------
// Strategy blocks shared by the base race and every pit-loss scenario
// ---------------------------------------------------------------------------

/// Writes the keys "dp" and "backtrack". The trace is only worth showing if it IS
/// the optimizer's recurrence, so it is checked against `optimal` first.
bool writeDpAndBacktrack(JsonWriter& json, const PitStrategy& strategy,
                         const StrategyResult& optimal, std::string& failure) {
    json.key("dp");
    const TraceOutcome trace = writeDpTrace(json, strategy);

    if (!nearlyEqual(trace.finalValue, optimal.dpValue)) {
        failure = "DP trace ends at " + std::to_string(trace.finalValue) +
                  " but PitStrategy::optimize() reports dp[N] = " + std::to_string(optimal.dpValue);
        return false;
    }
    if (trace.plan.size() != optimal.stints.size()) {
        failure = "DP trace backtracks to a different number of stints than optimize()";
        return false;
    }
    for (std::size_t i = 0; i < trace.plan.size(); ++i) {
        if (trace.plan[i].compoundIndex != optimal.stints[i].compoundIndex ||
            trace.plan[i].laps != optimal.stints[i].laps) {
            failure = "DP trace backtracks to a different plan than optimize()";
            return false;
        }
    }
    json.key("backtrack");
    writePlan(json, trace.plan);
    return true;
}

/// The array of best-per-stop-count results (the caller writes the key).
void writeStopCounts(JsonWriter& json, const std::vector<StopCountResult>& stopCounts) {
    json.beginArray();
    for (const StopCountResult& result : stopCounts) {
        json.beginObject()
            .key("stops").integer(result.stops)
            .key("feasible").boolean(result.feasible)
            .key("reason").string(result.reason);
        json.key("report");
        writeReport(json, result.report);
        json.endObject();
    }
    json.endArray();
}

/// Brute force on a short copy of the race: same pit loss, same compounds. Writes
/// the result object (the caller writes the key); false if the two disagree.
bool writeVerification(JsonWriter& json, const PitStrategy& strategy, std::string& failure) {
    Track shortTrack = strategy.track();
    shortTrack.setTotalLaps(std::min(strategy.track().totalLaps(), kVerificationLaps));
    const PitStrategy shortStrategy(shortTrack, strategy.compounds());
    const StrategyResult dpShort = shortStrategy.optimize();
    const double reference = bruteForceBest(shortStrategy, shortTrack.totalLaps(), true);

    const double gap = dpShort.totalTime - reference;
    if (!dpShort.feasible || !isReachable(reference) || std::fabs(gap) > kClosureTolerance) {
        failure = "the DP and the brute-force reference disagree on a " +
                  std::to_string(shortTrack.totalLaps()) + "-lap race";
        return false;
    }
    json.beginObject()
        .key("laps").integer(shortTrack.totalLaps())
        .key("dpTotal").number(dpShort.totalTime)
        .key("bruteForceTotal").number(reference)
        .key("gap").number(gap)
        .endObject();
    return true;
}

/// One pit-loss scenario for the Strategy Lab: the optimizer's answer for that pit
/// loss and everything the page animates for it. COST: one optimize(), one DP
/// replay, one stop-count query and one brute-force check.
bool writeScenario(JsonWriter& json, const Track& baseTrack, const std::vector<TireCompound>& compounds,
                   double pitLoss, int maxStops, std::string& failure) {
    Track track = baseTrack;
    track.setPitLoss(pitLoss);
    const PitStrategy strategy(track, compounds);
    const StrategyResult optimal = strategy.optimize();
    if (!optimal.feasible) {
        failure = "no feasible strategy at pit loss " + std::to_string(pitLoss);
        return false;
    }

    json.beginObject();
    json.key("pitLoss").number(pitLoss);
    json.key("optimal");
    writeResult(json, optimal);
    if (!writeDpAndBacktrack(json, strategy, optimal, failure)) {
        return false;
    }
    json.key("stopCounts");
    writeStopCounts(json, bestForStopCount(strategy, std::min(maxStops, track.totalLaps())));
    json.key("verification");
    if (!writeVerification(json, strategy, failure)) {
        return false;
    }
    json.endObject();
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Bundle
// ---------------------------------------------------------------------------

bool writeBundleJson(std::ostream& out, const PitStrategy& strategy, const ExportSettings& settings,
                     std::string& failure) {
    const Track& track = strategy.track();
    if (track.totalLaps() < 1) {
        throw std::invalid_argument("export needs a race of at least 1 lap");
    }
    if (settings.maxStops < 0) {
        throw std::invalid_argument("--max-stops: must not be negative");
    }

    const StrategyResult optimal = strategy.optimize();
    if (!optimal.feasible) {
        throw std::invalid_argument("no feasible strategy covers " +
                                    std::to_string(track.totalLaps()) +
                                    " laps with this compound set");
    }

    // Everything that can throw on a bad setting runs before the first byte is
    // written, so a rejected setting never leaves a half-written document.
    const int maxStops = std::min(settings.maxStops, track.totalLaps());
    const std::vector<StopCountResult> stopCounts = bestForStopCount(strategy, maxStops);
    const std::vector<StrategyReport> alternatives = kBestPlans(strategy, settings.alternatives);
    const SweepResult sweep =
        pitLossSweep(strategy, settings.sweepFrom, settings.sweepTo, settings.sweepStep);

    OvertakeRule overtakeOn = settings.overtake;
    overtakeOn.enabled = true;
    OvertakeRule overtakeOff = settings.overtake;
    overtakeOff.enabled = false;
    {
        // Validate the rule now (RaceSimulator owns the bounds) rather than mid-document.
        RaceSimulator probe(track, strategy.compounds());
        probe.setOvertakeRule(overtakeOn);
    }

    JsonWriter json(out);
    json.beginObject();

    json.key("meta").beginObject()
        .key("version").integer(kBundleVersion)
        .key("engine").string("boxbox")
        .key("generator").string("boxbox export")
        .key("laps").integer(track.totalLaps())
        .key("pitLoss").number(track.pitLoss())
        .key("closureTolerance").number(kClosureTolerance)
        .key("timeEpsilon").number(kTimeEpsilon)
        .endObject();

    json.key("track");
    writeTrack(json, track);
    json.key("compounds");
    writeCompounds(json, strategy.compounds());

    // --- strategy -----------------------------------------------------------
    json.key("strategy").beginObject();

    json.key("stintCostTable");
    writeStintCostTable(json, strategy);

    json.key("optimal");
    writeResult(json, optimal);

    if (!writeDpAndBacktrack(json, strategy, optimal, failure)) {
        return false;
    }

    json.key("stopCounts");
    writeStopCounts(json, stopCounts);

    json.key("alternatives").beginArray();
    for (const StrategyReport& report : alternatives) {
        writeReport(json, report);
    }
    json.endArray();

    json.key("sweep").beginObject();
    json.key("from").number(settings.sweepFrom);
    json.key("to").number(settings.sweepTo);
    json.key("step").number(settings.sweepStep);
    json.key("rows").beginArray();
    for (const SweepRow& row : sweep.rows) {
        json.beginObject()
            .key("pitLoss").number(row.value)
            .key("stops").integer(row.stops)
            .key("totalTime").number(row.totalTime)
            .key("planChanged").boolean(row.planChanged);
        json.key("plan");
        writePlan(json, row.plan);
        json.endObject();
    }
    json.endArray();
    json.endObject();

    json.key("verification");
    if (!writeVerification(json, strategy, failure)) {
        return false;
    }

    // One full scenario per sweep value (and the race's own pit loss), so the Strategy
    // Lab can move a slider through them with every figure coming from the engine.
    std::vector<double> scenarioLosses;
    for (const SweepRow& row : sweep.rows) {
        scenarioLosses.push_back(row.value);
    }
    {
        bool present = false;
        for (const double value : scenarioLosses) {
            present = present || nearlyEqual(value, track.pitLoss());
        }
        if (!present) {
            scenarioLosses.push_back(track.pitLoss());
        }
        std::sort(scenarioLosses.begin(), scenarioLosses.end());
    }
    json.key("scenarios").beginArray();
    for (const double pitLoss : scenarioLosses) {
        if (!writeScenario(json, track, strategy.compounds(), pitLoss, maxStops, failure)) {
            return false;
        }
    }
    json.endArray();

    // Undercut and overcut: the same car stops at the end of lap `earlier` or lap `later`, then
    // runs the best continuation (compareFirstStopLaps, which solves each with the optimizer).
    // Rows by earlier lap 1 .. N-2; columns by later lap earlier+1 .. N-1.
    json.key("undercut");
    if (track.totalLaps() < 3 || track.totalLaps() > kMaxRangeLaps) {
        json.null();
    } else {
        const int laps = track.totalLaps();
        std::vector<std::vector<FirstStopComparison>> grid;
        for (int earlier = 1; earlier <= laps - 2; ++earlier) {
            grid.emplace_back();
            for (int later = earlier + 1; later <= laps - 1; ++later) {
                grid.back().push_back(compareFirstStopLaps(strategy, earlier, later));
            }
        }
        json.beginObject();
        json.key("firstCompound").integer(static_cast<long long>(grid.front().front().firstCompound));
        const auto writeField = [&](const char* name, auto pick) {
            json.key(name).beginArray();
            for (const std::vector<FirstStopComparison>& row : grid) {
                json.beginArray();
                for (const FirstStopComparison& cell : row) { json.number(pick(cell)); }
                json.endArray();
            }
            json.endArray();
        };
        writeField("earlierTotal", [](const FirstStopComparison& c) { return c.result.earlierPlanTotalTime; });
        writeField("laterTotal", [](const FirstStopComparison& c) { return c.result.laterPlanTotalTime; });
        writeField("delta", [](const FirstStopComparison& c) { return c.result.timeDeltaSeconds; });
        json.endObject();
    }

    json.endObject();  // strategy

    json.key("tires");
    if (!writeTires(json, strategy, failure)) {
        return false;
    }

    // Which tree nodes a lap-range query touches (depends only on the race length).
    json.key("structures");
    if (track.totalLaps() > kMaxRangeLaps) {
        json.null();
    } else {
        writeStructures(json, static_cast<std::size_t>(track.totalLaps()));
    }

    json.key("algorithms");
    writeAlgorithms(json, strategy);

    // --- race ---------------------------------------------------------------
    json.key("race").beginObject();

    // FR-8: a lone car replaying the optimizer's own plan must post its total.
    {
        RaceSimulator solo(track, strategy.compounds());
        solo.addDriver(1, "OPTIMAL", optimal);
        solo.runToFinish();
        const double simulated = solo.drivers().front().cumulativeTime();
        const double difference = std::fabs(simulated - optimal.totalTime);
        const bool passed = difference <= kClosureTolerance;

        json.key("closure").beginObject()
            .key("optimizerTotal").number(optimal.totalTime)
            .key("simulatedTotal").number(simulated)
            .key("difference").number(difference)
            .key("tolerance").number(kClosureTolerance)
            .key("passed").boolean(passed)
            .endObject();

        if (!passed) {
            failure = "FR-8 loop closure failed: the optimizer and the simulator disagree about "
                      "the same race";
            return false;
        }
    }

    json.key("runs").beginObject();
    json.key("overtakeOff");
    if (!writeRun(json, strategy, overtakeOff, optimal, failure)) {
        return false;
    }
    json.key("overtakeOn");
    if (!writeRun(json, strategy, overtakeOn, optimal, failure)) {
        return false;
    }
    json.endObject();  // runs
    json.endObject();  // race

    json.endObject();  // root
    return true;
}

// ---------------------------------------------------------------------------
// Command
// ---------------------------------------------------------------------------

void printExportHelp(std::ostream& out) {
    out <<
        "boxbox export - write the data bundle the web frontend renders\n"
        "\n"
        "Usage: boxbox [race options] export [options]\n"
        "\n"
        "  --out DIR              Output folder, created if missing. Default frontend/data.\n"
        "                         Writes bundle.json and bundle.js.\n"
        "  --max-stops N          Stop-count comparison covers 0..N stops. Default 3.\n"
        "  --alternatives K       List the K cheapest distinct plans. Default 5.\n"
        "  --sweep FROM:TO:STEP   Pit-loss sweep range in seconds. Default 0:40:2.\n"
        "  --min-gap SECONDS      Overtake rule for the \"overtake on\" run.\n"
        "  --pass-margin SECONDS  Overtake rule for the \"overtake on\" run.\n"
        "\n"
        "Race options (--laps, --pit-loss, --sector, --compound, ...) go before the\n"
        "word export. The bundle is deterministic: the same options give the same bytes.\n"
        "Exit status: 0 success, 1 bad usage or unwritable folder, 2 no feasible\n"
        "strategy, 3 a self-check on the exported data failed (nothing is written).\n"
        "\n"
        "Example:\n"
        "  boxbox --laps 50 --pit-loss 22 export --out frontend/data\n";
}

int runExportCommand(const std::vector<std::string>& args, const PitStrategy& strategy,
                     std::ostream& out, std::ostream& err) {
    ExportSettings settings;
    std::string directory = "frontend/data";

    try {
        for (std::size_t i = 0; i < args.size(); ++i) {
            const std::string& arg = args[i];
            const auto value = [&]() -> const std::string& {
                if (i + 1U >= args.size()) {
                    throw std::invalid_argument(arg + ": missing value");
                }
                ++i;
                return args[i];
            };

            if (arg == "help" || arg == "-h" || arg == "--help") {
                printExportHelp(out);
                return kExportOk;
            } else if (arg == "--out") {
                directory = value();
                if (directory.empty()) {
                    throw std::invalid_argument("--out: folder must not be empty");
                }
            } else if (arg == "--max-stops") {
                settings.maxStops = parseWhole(value(), "--max-stops");
            } else if (arg == "--alternatives") {
                settings.alternatives = static_cast<std::size_t>(parseWhole(value(), "--alternatives"));
            } else if (arg == "--sweep") {
                const std::string spec = value();
                const std::vector<std::string> parts = splitOn(spec, ':');
                if (parts.size() != 3U) {
                    throw std::invalid_argument("--sweep \"" + spec + "\": expected FROM:TO:STEP");
                }
                settings.sweepFrom = parseNumber(parts[0], "--sweep FROM");
                settings.sweepTo = parseNumber(parts[1], "--sweep TO");
                settings.sweepStep = parseNumber(parts[2], "--sweep STEP");
            } else if (arg == "--min-gap") {
                settings.overtake.minGap = parseNumber(value(), "--min-gap");
            } else if (arg == "--pass-margin") {
                settings.overtake.passMargin = parseNumber(value(), "--pass-margin");
            } else {
                throw std::invalid_argument("export: unrecognised option \"" + arg +
                                            "\"; try `boxbox export help`");
            }
        }
    } catch (const std::exception& error) {
        err << "boxbox: " << error.what() << '\n';
        return kExportUsageError;
    }

    // Build the whole document in memory first: nothing touches the disk unless
    // every self-check passed.
    std::ostringstream document;
    std::string failure;
    try {
        if (strategy.track().totalLaps() >= 1 && !strategy.optimize().feasible) {
            err << "boxbox: no feasible strategy covers " << strategy.track().totalLaps()
                << " laps with this compound set\n";
            return kExportInfeasible;
        }
        if (!writeBundleJson(document, strategy, settings, failure)) {
            err << "boxbox: export self-check failed: " << failure << '\n';
            return kExportCheckFailed;
        }
    } catch (const std::exception& error) {
        err << "boxbox: " << error.what() << '\n';
        return kExportUsageError;
    }

    const std::string json = document.str();
    const std::filesystem::path folder(directory);

    try {
        std::filesystem::create_directories(folder);

        std::ofstream jsonFile(folder / "bundle.json", std::ios::binary | std::ios::trunc);
        jsonFile << json << '\n';
        jsonFile.close();

        std::ofstream scriptFile(folder / "bundle.js", std::ios::binary | std::ios::trunc);
        scriptFile << "window.BOXBOX = " << json << ";\n";
        scriptFile.close();

        if (!jsonFile || !scriptFile) {
            throw std::runtime_error("could not write to \"" + directory + "\"");
        }
    } catch (const std::exception& error) {
        err << "boxbox: " << error.what() << '\n';
        return kExportUsageError;
    }

    out << "Exported " << json.size() << " bytes of JSON to " << directory
        << "/bundle.json and bundle.js\n";
    return kExportOk;
}

}  // namespace boxbox
