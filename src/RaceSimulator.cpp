#include "boxbox/RaceSimulator.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

#include "boxbox/Common.hpp"

namespace boxbox {

RaceSimulator::RaceSimulator(Track track, std::vector<TireCompound> compounds)
    : track_(std::move(track)), compounds_(std::move(compounds)) {
    if (compounds_.empty()) {
        throw std::invalid_argument("RaceSimulator: at least one tire compound is required");
    }
}

// The guarantee FR-8 rests on: a car's plan must cover the race exactly, so
// its lap-by-lap total is directly comparable with what the optimizer costed.
void RaceSimulator::validatePlan(const std::vector<Stint>& plan) const {
    int coveredLaps = 0;

    for (const Stint& stint : plan) {
        if (stint.laps <= 0) {
            throw std::invalid_argument(
                "RaceSimulator: every planned stint must be at least one lap long");
        }
        if (stint.compoundIndex >= compounds_.size()) {
            throw std::invalid_argument(
                "RaceSimulator: planned stint names a compound outside the compound set");
        }
        if (stint.laps > compounds_[stint.compoundIndex].maxStintLaps()) {
            throw std::invalid_argument(
                "RaceSimulator: planned stint is longer than its compound allows");
        }
        coveredLaps += stint.laps;
    }

    if (coveredLaps != track_.totalLaps()) {
        throw std::invalid_argument(
            "RaceSimulator: plan covers " + std::to_string(coveredLaps) + " laps but the race is " +
            std::to_string(track_.totalLaps()) + " laps long");
    }
}

void RaceSimulator::addDriver(int number, std::string name, std::vector<Stint> plan) {
    validatePlan(plan);
    drivers_.emplace_back(number, std::move(name), std::move(plan));
}

// FR-7. The stop laps come from the optimizer's own backtracked plan; nothing
// in this path invents or hard-codes a lap number.
void RaceSimulator::addDriver(int number, std::string name, const StrategyResult& plan) {
    if (!plan.feasible) {
        throw std::invalid_argument("RaceSimulator: cannot enter a driver on an infeasible plan");
    }
    addDriver(number, std::move(name), plan.stints);
}

// Both checks run before anything is stored, so a rejected rule leaves the
// current one in place.
void RaceSimulator::setOvertakeRule(const OvertakeRule& rule) {
    if (!std::isfinite(rule.minGap) || rule.minGap < kMinOvertakeGap) {
        throw std::invalid_argument(
            "RaceSimulator: overtake minGap must be finite and at least kMinOvertakeGap (1 ms)");
    }
    if (!std::isfinite(rule.passMargin) || rule.passMargin < 0.0) {
        throw std::invalid_argument(
            "RaceSimulator: overtake passMargin must be finite and not negative");
    }
    overtakeRule_ = rule;
}

bool RaceSimulator::finished() const noexcept {
    for (const Driver& driver : drivers_) {
        if (!driver.finished()) {
            return false;
        }
    }
    return true;
}

// FR-5. O(1). The driver supplies the compound and the tire age; the track
// supplies the reference lap. This is PRD sec. 3.2 with basePace additive.
double RaceSimulator::lapTimeFor(const Driver& driver) const {
    if (driver.plan().empty() || driver.finished()) {
        return 0.0;
    }
    const TireCompound& compound = compounds_[driver.currentCompoundIndex()];
    return track_.baseLapTime() + compound.lapDelta(driver.tireAge());
}

// O(D) per lap with overtaking off; O(D log D) with it on.
bool RaceSimulator::advanceLap() {
    if (finished()) {
        return false;
    }

        if (overtakeRule_.enabled) {
        advanceLapWithTraffic();
    } else {
        for (Driver& driver : drivers_) {
            if (driver.finished()) {
                continue;
            }
            const std::size_t stintBefore = driver.stintIndex();
            const std::size_t compoundBefore = driver.currentCompoundIndex();
            driver.completeLap(lapTimeFor(driver), track_.pitLoss());
            emitLapEvents(driver, stintBefore, compoundBefore);
        }
    }

    ++lapsRun_;
    return true;
}

// FR-9, the "pass or hold" rule documented on OvertakeRule.
//
// COST: O(D log D) per lap - O(D) to snapshot and heapify, D pops at O(log D)
// for the start-of-lap running order, then one O(D) walk. The order has to be
// rebuilt every lap because every car's key changes every lap.
//
// Walking front to back means the car directly ahead has always been settled
// before the car behind it is looked at, so each car needs only that one
// neighbour's crossing time and pace.
void RaceSimulator::advanceLapWithTraffic() {
    Leaderboard board(snapshotEntries());
    const std::vector<LeaderboardEntry> order = board.drainOrdered();

    // The car directly ahead in the start-of-lap order, once settled.
    bool haveAhead = false;
    int aheadLaps = 0;
    double aheadCrossing = 0.0;  // when it crossed the line, before any pit loss
    double aheadPace = 0.0;      // its own lap time from sec. 3.2

    for (const LeaderboardEntry& entry : order) {
        Driver& driver = drivers_[entry.driverIndex];
        if (driver.finished()) {
            haveAhead = false;  // off the track, so it cannot hold anyone up
            continue;
        }

        const double pace = lapTimeFor(driver);
        const double arrival = driver.cumulativeTime() + pace;

        const bool sameLap = haveAhead && aheadLaps == entry.lapsCompleted;
        const double holdLine = aheadCrossing + overtakeRule_.minGap;
        const bool caught = sameLap && definitelyLess(arrival, holdLine);
        const bool canPass = !definitelyLess(aheadPace - pace, overtakeRule_.passMargin);

        double delay = 0.0;
        double crossing = arrival;
        if (caught && !canPass) {
            delay = holdLine - arrival;  // positive: caught means arrival < holdLine
            crossing = holdLine;
        }

        // Pit loss, if this lap ends a stint, is charged inside completeLap. It
        // never reaches `crossing`, which is all the car behind is compared
        // against: the pit lane is not raced.
        const std::size_t stintBefore = driver.stintIndex();
        const std::size_t compoundBefore = driver.currentCompoundIndex();
        driver.completeLap(pace, track_.pitLoss(), delay);
        emitLapEvents(driver, stintBefore, compoundBefore);

        haveAhead = true;
        aheadLaps = entry.lapsCompleted;
        aheadCrossing = crossing;
        aheadPace = pace;
    }
}

// O(N*D), or O(N*D log D) with overtaking on. The loop is bounded by the race
// distance as well as by finished(), so a future change that broke lap
// accounting would stop rather than hang.
void RaceSimulator::runToFinish() {
    const int lapLimit = track_.totalLaps();
    while (lapsRun_ <= lapLimit && advanceLap()) {
        // advanceLap does the work
    }
}

void RaceSimulator::reset() {
    for (Driver& driver : drivers_) {
        driver.reset();
    }
    lapsRun_ = 0;
}
void RaceSimulator::emitLapEvents(const Driver& driver,
                                   std::size_t stintIndexBefore,
                                   std::size_t compoundIndexBefore) {
    if (eventSink_ == nullptr) {
        return;
    }

    RaceEvent lapEvent;
    lapEvent.type = RaceEventType::LapCompleted;
    lapEvent.timestamp = driver.cumulativeTime();
    lapEvent.driverNumber = driver.number();
    lapEvent.lap = driver.lapsCompleted();
    eventSink_->push(lapEvent);

    const bool stopped = driver.stintIndex() != stintIndexBefore;
    if (stopped) {
        RaceEvent pitEntry;
        pitEntry.type = RaceEventType::PitEntry;
        pitEntry.timestamp = driver.cumulativeTime();
        pitEntry.driverNumber = driver.number();
        pitEntry.lap = driver.lapsCompleted();
        eventSink_->push(pitEntry);

        RaceEvent compoundChange;
        compoundChange.type = RaceEventType::CompoundChange;
        compoundChange.timestamp = driver.cumulativeTime();
        compoundChange.driverNumber = driver.number();
        compoundChange.lap = driver.lapsCompleted();
        compoundChange.oldCompoundIndex = static_cast<int>(compoundIndexBefore);
        compoundChange.newCompoundIndex =
            static_cast<int>(driver.currentCompoundIndex());
        compoundChange.tireAgeAtEvent = driver.tireAge();
        eventSink_->push(compoundChange);

        RaceEvent pitExit;
        pitExit.type = RaceEventType::PitExit;
        pitExit.timestamp = driver.cumulativeTime();
        pitExit.driverNumber = driver.number();
        pitExit.lap = driver.lapsCompleted();
        pitExit.tireAgeAtEvent = driver.tireAge();
        eventSink_->push(pitExit);
    }
}

// O(D).
std::vector<LeaderboardEntry> RaceSimulator::snapshotEntries() const {
    std::vector<LeaderboardEntry> entries;
    entries.reserve(drivers_.size());

    for (std::size_t i = 0; i < drivers_.size(); ++i) {
        LeaderboardEntry entry;
        entry.driverIndex = i;
        entry.driverNumber = drivers_[i].number();
        entry.lapsCompleted = drivers_[i].lapsCompleted();
        entry.cumulativeTime = drivers_[i].cumulativeTime();
        entries.push_back(entry);
    }
    return entries;
}

// O(D). The gap is measured against whoever the caller's ordering put first.
std::vector<Standing> RaceSimulator::toStandings(
    const std::vector<LeaderboardEntry>& ordered) const {
    std::vector<Standing> table;
    table.reserve(ordered.size());

    const double leaderTime = ordered.empty() ? 0.0 : ordered.front().cumulativeTime;

    for (std::size_t position = 0; position < ordered.size(); ++position) {
        Standing row;
        row.driverIndex = ordered[position].driverIndex;
        row.position = static_cast<int>(position) + 1;
        row.gapToLeader = ordered[position].cumulativeTime - leaderTime;
        table.push_back(row);
    }
    return table;
}

// FR-2. O(D) heapify + D pops at O(log D) = O(D log D). See the header: a full
// ordering is no cheaper from a heap than from a sort.
std::vector<Standing> RaceSimulator::standings() const {
    Leaderboard board(snapshotEntries());
    return toStandings(board.drainOrdered());
}

// O(D + k log D). The query the heap is actually good at.
std::vector<Standing> RaceSimulator::topK(std::size_t count) const {
    Leaderboard board(snapshotEntries());
    return toStandings(board.takeTop(count));
}

}  // namespace boxbox
