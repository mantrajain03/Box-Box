#ifndef BOXBOX_RACE_SIMULATOR_HPP
#define BOXBOX_RACE_SIMULATOR_HPP

#include <cstddef>
#include <string>
#include <vector>

#include "boxbox/Driver.hpp"
#include "boxbox/Leaderboard.hpp"
#include "boxbox/PitStrategy.hpp"
#include "boxbox/TireCompound.hpp"
#include "boxbox/Track.hpp"
#include "boxbox/EventPriorityQueue.hpp"

namespace boxbox {

/// One row of the classification.
struct Standing {
    std::size_t driverIndex = 0;  ///< index into RaceSimulator::drivers()
    int position = 0;             ///< 1-based
    double gapToLeader = 0.0;     ///< seconds behind the car in P1
};

/// Smallest OvertakeRule::minGap that setOvertakeRule accepts, in seconds.
///
/// A held car crosses the line exactly minGap behind the car it is stuck
/// behind. A gap far below a millisecond means nothing next to the comparison
/// tolerance, and one small enough would round away entirely, leaving a dead
/// heat that the car-number tie-break could settle in the held car's favour.
inline constexpr double kMinOvertakeGap = 1.0e-3;

/// FR-9: the deterministic "pass or hold" overtaking rule. Off by default.
///
/// Every car's own lap time still comes from PRD sec. 3.2, exactly as it does
/// with the rule off: the rule never changes a car's pace, only when it
/// crosses the line. Taking the running order at the start of the lap and
/// walking it from P1 down, each car B is compared with the car A directly
/// ahead of it, whose crossing time has already been settled:
///
///     arrival = B's start-of-lap time + B's lap time
///     if arrival < A's crossing + minGap                 B has caught A
///         if A's lap time - B's lap time >= passMargin   pass:  B keeps arrival
///         else                                           held:  B crosses minGap behind A
///     else                                               clear: B keeps arrival
///
/// Comparisons go through definitelyLess, never ==, so a car that arrives
/// exactly minGap behind is clear and a pace advantage of exactly passMargin
/// is enough to pass.
///
/// Pit loss is charged after the crossing is settled: the pit lane is not
/// raced. Cars on different lap counts never interact, so lapped traffic is
/// not modelled. A car that passes is not re-checked against cars further up
/// the road on the same lap - a deliberate simplification.
struct OvertakeRule {
    bool enabled = false;
    double minGap = 0.5;      ///< seconds; closest a car may follow across the line
    double passMargin = 1.0;  ///< seconds per lap a car must be quicker to get by
};

/// Lap-by-lap race replay.
///
/// Owns copies of the track and the compound set, so nothing here depends on
/// the lifetime of the caller's objects. Track, PitStrategy and Leaderboard
/// remain usable with no RaceSimulator in existence (NFR-3) - the dependency
/// runs one way only.
///
/// Cars are independent and never interact unless the FR-9 overtaking rule is
/// switched on with setOvertakeRule(). It is off by default, and a lone car is
/// never obstructed either way, so FR-8 closes with the rule on or off.
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

    /// FR-9. Replace the overtaking rule; it applies from the next lap run.
    /// \throws std::invalid_argument if minGap is not finite or is below
    ///         kMinOvertakeGap, or passMargin is negative or not finite. A
    ///         rejected rule leaves the current one in place.
    void setOvertakeRule(const OvertakeRule& rule);

    const OvertakeRule& overtakeRule() const noexcept { return overtakeRule_; }

    /// Advance every unfinished car by one lap. Returns false when the race is
    /// already over.
    ///
    /// COST: O(D) per call for D drivers with overtaking off. With FR-9 on it
    /// is O(D log D): the start-of-lap running order is drained from a
    /// Leaderboard heap, then walked once. With an event sink set (see
    /// setEventSink) each car also pushes a handful of events (at most 7: lap,
    /// attempt, pass or hold, finish or the three pit events, and a position
    /// change), O(log E) apiece for E queued events, plus one standings() call
    /// per lap, so a lap then costs O(D log E + D log D) more.
    bool advanceLap();

    /// Advance until every car has finished. O(N*D) for an N-lap race, or
    /// O(N*D log D) with overtaking on.
    void runToFinish();

    /// Put every car back on the grid.
    void reset();

    /// Laps the race has advanced so far.
    int lapsRun() const noexcept { return lapsRun_; }

    bool finished() const noexcept;

    /// Set (or, with nullptr, clear) the optional event sink. Changing it
    /// forgets the position baseline, so the next PositionChange is measured
    /// from the standings at the next lap rather than from a stale round.
    void setEventSink(EventPriorityQueue* sink) noexcept {
        eventSink_ = sink;
        positionsSeeded_ = false;
    }

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

    /// One lap under the FR-9 rule. O(D log D).
    void advanceLapWithTraffic();
    /// How the FR-9 rule treated a car on the lap just completed.
    enum class TrafficOutcome { Clear, Passed, Held };

    /// Emit this lap's events to the sink, if one is set. `other` is the car
    /// directly ahead and `holdSeconds` the time lost; both are only
    /// meaningful for Passed / Held.
    void emitLapEvents(const Driver& driver, std::size_t stintIndexBefore,
                        std::size_t compoundIndexBefore,
                        TrafficOutcome outcome = TrafficOutcome::Clear,
                        int other = -1, double holdSeconds = 0.0);

    /// Turn an already-ordered entry list into positions and gaps. O(D).
    std::vector<Standing> toStandings(const std::vector<LeaderboardEntry>& ordered) const;

    Track track_;
    std::vector<TireCompound> compounds_;
    std::vector<Driver> drivers_;
    OvertakeRule overtakeRule_;
    int lapsRun_ = 0;
    EventPriorityQueue* eventSink_ = nullptr;

    /// Running positions at the end of the previous lap round, by driver
    /// index; only kept while an event sink is set. See emitPositionChanges.
    std::vector<int> lastPositions_;
    bool positionsSeeded_ = false;

    /// After a full lap round, emit a PositionChange for every car whose place
    /// differs from the previous round. Read-only (standings() is const).
    /// O(D log D) per lap, and only when a sink is set.
    void emitPositionChanges();
};

}  // namespace boxbox

#endif  // BOXBOX_RACE_SIMULATOR_HPP
