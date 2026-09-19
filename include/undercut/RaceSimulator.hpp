#ifndef UNDERCUT_RACE_SIMULATOR_HPP
#define UNDERCUT_RACE_SIMULATOR_HPP

#include <cstddef>
#include <string>
#include <vector>

#include "undercut/Driver.hpp"
#include "undercut/Leaderboard.hpp"
#include "undercut/PitStrategy.hpp"
#include "undercut/TireCompound.hpp"
#include "undercut/Track.hpp"

namespace undercut {

/// One row of the classification.
struct Standing {
    std::size_t driverIndex = 0;  ///< index into RaceSimulator::drivers()
    int position = 0;             ///< 1-based
    double gapToLeader = 0.0;     ///< seconds behind the car in P1
};

/// Lap-by-lap race replay.
///
/// Owns copies of the track and the compound set, so nothing here depends on
/// the lifetime of the caller's objects. Track, PitStrategy and Leaderboard
/// remain usable with no RaceSimulator in existence (NFR-3) - the dependency
/// runs one way only.
///
/// Overtaking is not modelled (FR-9 is not implemented yet), so every race run
/// today is a no-overtake race: cars are independent and never interact.
class RaceSimulator {
public:
    RaceSimulator(Track track, std::vector<TireCompound> compounds);

    /// Enter a car running `plan`.
    /// \throws std::invalid_argument if the plan does not cover the race
    ///         distance exactly, names a compound outside the set, or runs a
    ///         stint longer than its compound allows.
    void addDriver(int number, std::string name, std::vector<Stint> plan);

    /// Enter a car running the plan from a StrategyResult (FR-7). This is the
    /// path that closes the loop: no stop lap is written by hand anywhere.
    /// \throws std::invalid_argument if the result is not feasible, or fails
    ///         the checks above.
    void addDriver(int number, std::string name, const StrategyResult& plan);

    /// Advance every unfinished car by one lap. Returns false when the race is
    /// already over. O(D) per call for D drivers.
    bool advanceLap();

    /// Advance until every car has finished. O(N*D) for an N-lap race.
    void runToFinish();

    /// Put every car back on the grid.
    void reset();

    /// Laps the race has advanced so far.
    int lapsRun() const noexcept { return lapsRun_; }

    bool finished() const noexcept;

    /// FR-5. The time the next lap will take for `driver`, computed from THAT
    /// driver's own compound and its own tire age - never a flat track lap
    /// time. This is the project's only implementation of PRD sec. 3.2 beyond
    /// the prefix table the optimizer builds from the same TireCompound.
    ///
    /// Returns 0.0 for a finished driver or an empty plan. O(1).
    double lapTimeFor(const Driver& driver) const;

    const std::vector<Driver>& drivers() const noexcept { return drivers_; }
    const Track& track() const noexcept { return track_; }
    const std::vector<TireCompound>& compounds() const noexcept { return compounds_; }

    /// The full classification, leader first.
    ///
    /// COST: O(D) to snapshot the field, O(D) to heapify, then D pops at
    /// O(log D) each - so O(D log D) per call, and O(N*D log D) if it is called
    /// on every lap of an N-lap race. Using a min-heap does NOT make a full
    /// ordering cheaper than sorting, and this comment will not claim it does
    /// (NFR-2). Every driver's key changes every lap, so no incremental scheme
    /// would help either.
    ///
    /// Ordering is Leaderboard::isAhead, which is a total order, so the table
    /// is identical on every run (NFR-5).
    std::vector<Standing> standings() const;

    /// Just the leading `count` cars, in order - the live-timing query.
    ///
    /// COST: O(D + k log D) for k = min(count, D). This is where the heap earns
    /// its place over a sort: the podium does not require ordering the field.
    std::vector<Standing> topK(std::size_t count) const;

    /// A snapshot of the field as heap entries. O(D). Exposed because
    /// Leaderboard is usable on its own (NFR-3) and a caller may want to run
    /// its own queries over the same data.
    std::vector<LeaderboardEntry> snapshotEntries() const;

private:
    void validatePlan(const std::vector<Stint>& plan) const;

    /// Turn an already-ordered entry list into positions and gaps. O(D).
    std::vector<Standing> toStandings(const std::vector<LeaderboardEntry>& ordered) const;

    Track track_;
    std::vector<TireCompound> compounds_;
    std::vector<Driver> drivers_;
    int lapsRun_ = 0;
};

}  // namespace undercut

#endif  // UNDERCUT_RACE_SIMULATOR_HPP
