#ifndef BOXBOX_RACESTATE_HPP
#define BOXBOX_RACESTATE_HPP

#include <cstddef>
#include <stdexcept>
#include <string>
#include "boxbox/EventLog.hpp"
#include "boxbox/RaceEvent.hpp"

/// Legal per-driver race-state transitions, driven by RaceEvents
/// (owned by Pratyaksh).
///
/// RaceState tracks one driver's current state and rejects an event that
/// would represent an illegal transition (e.g. an Overtake while the car
/// is InPit). It does not compute times or decide outcomes - it only says
/// whether a given event is a legal next step from the current state.
namespace boxbox {

enum class DriverRaceState {
    Racing,
    PitEntryPending,  // PitEntry event seen, not yet in the pit lane proper
    InPit,
    Finished
};

/// Thrown when an event would be an illegal transition from the driver's
/// current state.
class IllegalStateTransition : public std::logic_error {
public:
    explicit IllegalStateTransition(const char* what) : std::logic_error(what) {}
};

class RaceState {
public:
    explicit RaceState(int driverNumber) : driverNumber_(driverNumber) {}

    DriverRaceState current() const noexcept { return state_; }
    int driverNumber() const noexcept { return driverNumber_; }

    /// Validate and apply one event for this driver. O(1).
    /// \throws IllegalStateTransition if the event is not a legal next step.
    /// \pre event.driverNumber == driverNumber() (checked, throws otherwise).
    void apply(const RaceEvent& event);

private:
    int driverNumber_;
    DriverRaceState state_ = DriverRaceState::Racing;
};

/// Result of replaying a whole log through the per-driver state machines.
struct EventLogValidation {
    bool valid = true;
    std::size_t badIndex = 0;  ///< index of the first offending entry, if !valid
    std::string message;       ///< empty when valid
};

/// Replay `log` in order, one RaceState per driver (created on first sight),
/// and check that the log is in canonical eventComesBefore order. Stops at the
/// first problem. A driver that has not reached Finished is NOT an error: a
/// log of a partly-run race is legal.
/// O(n) expected for n events (hash map from driver number to state).
EventLogValidation validateEventLog(const EventLog& log);

}  // namespace boxbox

#endif  // BOXBOX_RACESTATE_HPP