#include "ExportCommand.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
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
#include "boxbox/RaceState.hpp"
#include "boxbox/StrategyAnalytics.hpp"
#include "boxbox/TireCompound.hpp"
#include "boxbox/Track.hpp"
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
double bruteForceBest(const PitStrategy& strategy, int lapsLeft, bool isFirstStint) {
    if (lapsLeft == 0) {
        return 0.0;
    }
    double best = kUnreachable;
    for (std::size_t c = 0; c < strategy.compounds().size(); ++c) {
        const int longest = std::min(lapsLeft, strategy.compounds()[c].maxStintLaps());
        for (int length = 1; length <= longest; ++length) {
            const double rest = bruteForceBest(strategy, lapsLeft - length, false);
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

    json.endObject();
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

    json.key("dp");
    const TraceOutcome trace = writeDpTrace(json, strategy);

    // The trace is only worth showing if it IS the optimizer's recurrence.
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

    json.key("stopCounts").beginArray();
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

    // Brute force on a short copy of the race: same pit loss, same compounds.
    {
        Track shortTrack = track;
        shortTrack.setTotalLaps(std::min(track.totalLaps(), kVerificationLaps));
        const PitStrategy shortStrategy(shortTrack, strategy.compounds());
        const StrategyResult dpShort = shortStrategy.optimize();
        const double reference = bruteForceBest(shortStrategy, shortTrack.totalLaps(), true);

        const double gap = dpShort.totalTime - reference;
        if (!dpShort.feasible || !isReachable(reference) ||
            std::fabs(gap) > kClosureTolerance) {
            failure = "the DP and the brute-force reference disagree on a " +
                      std::to_string(shortTrack.totalLaps()) + "-lap race";
            return false;
        }
        json.key("verification").beginObject()
            .key("laps").integer(shortTrack.totalLaps())
            .key("dpTotal").number(dpShort.totalTime)
            .key("bruteForceTotal").number(reference)
            .key("gap").number(gap)
            .endObject();
    }

    json.endObject();  // strategy

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
