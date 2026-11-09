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
///
/// PositionChange sorts after LapCompleted so a car's lap is logged before the
/// change of place that lap produced; OvertakeAttempt sorts before the Overtake
/// or TrafficHold that resolves it.
enum class RaceEventType {
    PitEntry,
    CompoundChange,
    PitExit,
    LapCompleted,
    PositionChange,
    OvertakeAttempt,
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
    RaceEventType type = RaceEventType::LapCompleted;

    /// Seconds, same clock as Driver::cumulativeTime. Must be finite and not
    /// negative: EventPriorityQueue::push rejects anything else.
    double timestamp = 0.0;

    /// Matches LeaderboardEntry::driverNumber, not a driver array index.
    int driverNumber = 0;

    /// 1-based, per PRD sec 3.1.
    int lap = 0;

    /// Assigned by EventPriorityQueue at insertion. Final tiebreak only;
    /// never meaningful on its own.
    std::uint64_t sequenceId = 0;

    // --- CompoundChange / LapCompleted payload ---
    /// CompoundChange: compound index before the stop.
    /// LapCompleted: compound index the lap just completed was run on, which
    /// lets a log alone say which compound every stint started on.
    int oldCompoundIndex = -1;
    /// Compound index after the stop. CompoundChange only.
    int newCompoundIndex = -1;

    // --- CompoundChange / PitExit payload ---
    /// Age of the tire fitted by the stop, which is always 0 (a fresh set).
    /// The age of the set that was removed is not recorded here; it is the
    /// preceding LapCompleted's stint length and the Driver already owns it.
    int tireAgeAtEvent = 0;

    // --- PositionChange payload ---
    /// Running positions (1 = leader) before and after the lap round that
    /// produced this event. PositionChange only; 0 otherwise.
    int oldPosition = 0;
    int newPosition = 0;

    // --- OvertakeAttempt / Overtake / TrafficHold payload ---
    /// The car this event happened relative to (the car it caught, passed, or
    /// was held by). -1 when not applicable.
    int otherDriverNumber = -1;

    // --- TrafficHold payload ---
    /// Seconds of time lost to this specific hold. TrafficHold only.
    double holdLossSeconds = 0.0;
};

/// Strict weak order for the event queue (NFR-5).
///
/// Four levels, each firing only when every level above is an exact tie.
/// The timestamp level compares exactly, with no tolerance. A tolerance
/// comparison ("equal within 1e-9") is not transitive - a ~ b and b ~ c does
/// not imply a ~ c - so using it as the first key of a lexicographic order
/// would break strict weak ordering and let a heap misorder near-equal
/// chains. Exact comparison is a genuine total preorder, and it is still
/// deterministic because identical inputs produce bit-identical timestamps.
/// push() guarantees timestamps are finite, so NaN cannot reach this.
///
/// Exposed as a free function, matching Leaderboard::isAhead's pattern, so
/// a test can check the heap against a brute-force sort using exactly this
/// comparison.
inline bool eventComesBefore(const RaceEvent& a, const RaceEvent& b) noexcept {
    if (a.timestamp < b.timestamp) return true;
    if (b.timestamp < a.timestamp) return false;

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