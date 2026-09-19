#include "undercut/RaceSimulator.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "undercut/Common.hpp"

namespace undercut {

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

// O(D) per lap.
bool RaceSimulator::advanceLap() {
    if (finished()) {
        return false;
    }

    for (Driver& driver : drivers_) {
        if (driver.finished()) {
            continue;
        }
        driver.completeLap(lapTimeFor(driver), track_.pitLoss());
    }

    ++lapsRun_;
    return true;
}

// O(N*D). The loop is bounded by the race distance as well as by finished(),
// so a future change that broke lap accounting would stop rather than hang.
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

}  // namespace undercut
