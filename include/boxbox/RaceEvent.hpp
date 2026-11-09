#ifndef BOXBOX_RACEEVENT_HPP
#define BOXBOX_RACEEVENT_HPP

#include <cstdint>
#include "boxbox/Common.hpp"

/// Race event model (FR: event engine, owned by Pratyaksh).
///
/// A RaceEvent is a single timestamped occurrence during a race: a lap
/// completion, a pit stop, a compound change, an overtake, a traffic hold,
/// or the race finish. Events are produced by RaceSimulator as it advances
/// the race and consumed in timestamp order by EventPriorityQueue /
/// EventLog, which PitAnalytics then summarises.
///
/// Ordering is a strict weak order over four levels, agreed with the team:
///   1. timestamp     - earliest first
///   2. event type    - race-semantics priority (see RaceEventType)
///   3. driverNumber  - ascending
///   4. sequenceId    - insertion order, final deterministic fallback
namespace boxbox {

/// Declaration order IS priority order (ascending = processed earlier)
/// when two events tie on timestamp. Centralized here deliberately, so
/// there is exactly one place this ordering can be edited.
///
/// Rationale for CompoundChange before PitExit: a compound change models
/// tyre fitting, which happens before the car leaves the pit lane.
enum class RaceEventType {
    PitEntry,
    CompoundChange,
    PitExit,
    LapCompleted,
    Overtake,
    TrafficHold,
    RaceFinish
};

/// One timestamped race occurrence.
///
/// Not every field is meaningful for every event type; each field's comment
/// states which event type(s) populate it. Fields left at their default are
/// unused for that event's type, not missing data.
struct RaceEvent {
    RaceEventType type;

    /// Seconds, same clock as Driver::cumulativeTime.
    double timestamp = 0.0;

    /// Matches LeaderboardEntry::driverNumber, not a driver array index.
    int driverNumber = 0;

    /// 1-based, per PRD sec 3.1.
    int lap = 0;

    /// Assigned by EventPriorityQueue at insertion. Final tiebreak only;
    /// never meaningful on its own.
    std::uint64_t sequenceId = 0;

    // --- CompoundChange payload ---
    /// Compound index before the stop. CompoundChange only.
    int oldCompoundIndex = -1;
    /// Compound index after the stop. CompoundChange only.
    int newCompoundIndex = -1;

    // --- CompoundChange / PitExit payload ---
    /// Tire age at the moment of this event. Always 0 for CompoundChange
    /// (a fresh set is fitted), meaningful as "age of the set just removed"
    /// is NOT tracked here deliberately - the simulator's Driver already
    /// owns that. PitExit: age of the newly fitted tire, i.e. 0.
    int tireAgeAtEvent = 0;

    // --- Overtake / TrafficHold payload ---
    /// The car this event happened relative to (the car passed, or the car
    /// that caused the hold). -1 when not applicable.
    int otherDriverNumber = -1;

    // --- TrafficHold payload ---
    /// Seconds of time lost to this specific hold. TrafficHold only.
    double holdLossSeconds = 0.0;
};

/// Strict weak order for the event queue (NFR-5).
///
/// Four levels, each firing only when every level above is a true tie.
/// Deliberately NOT a single epsilon-based `<` on timestamp alone: that
/// would leave near-equal timestamps in an ambiguous zone that breaks
/// strict weak ordering once combined with the other keys. Falling through
/// to the next level only on a genuine tie (neither definitelyLess(a,b) nor
/// definitelyLess(b,a)) keeps the overall relation strict and transitive,
/// which is required for correct heap behaviour.
///
/// Exposed as a free function, matching Leaderboard::isAhead's pattern, so
/// a test can check the heap against a brute-force sort using exactly this
/// comparison.
inline bool eventComesBefore(const RaceEvent& a, const RaceEvent& b) noexcept {
    if (definitelyLess(a.timestamp, b.timestamp)) return true;
    if (definitelyLess(b.timestamp, a.timestamp)) return false;

    if (a.type != b.type) {
        return static_cast<int>(a.type) < static_cast<int>(b.type);
    }
    if (a.driverNumber != b.driverNumber) {
        return a.driverNumber < b.driverNumber;
    }
    return a.sequenceId < b.sequenceId;
}

}  // namespace boxbox

#endif  // BOXBOX_RACEEVENT_HPP