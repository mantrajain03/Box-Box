#include "boxbox/RaceState.hpp"

#include <unordered_map>
#include <vector>

namespace boxbox {

void RaceState::apply(const RaceEvent& event) {
    if (event.driverNumber != driverNumber_) {
        throw IllegalStateTransition(
            "RaceState::apply - event is for a different driver");
    }

    switch (state_) {
        case DriverRaceState::Racing:
            switch (event.type) {
                case RaceEventType::LapCompleted:
                case RaceEventType::Overtake:
                case RaceEventType::TrafficHold:
                    // stays Racing
                    return;
                case RaceEventType::PitEntry:
                    state_ = DriverRaceState::PitEntryPending;
                    return;
                case RaceEventType::RaceFinish:
                    state_ = DriverRaceState::Finished;
                    return;
                default:
                    break;  // fall through to throw
            }
            break;

        case DriverRaceState::PitEntryPending:
            switch (event.type) {
                case RaceEventType::CompoundChange:
                    state_ = DriverRaceState::InPit;
                    return;
                default:
                    break;
            }
            break;

        case DriverRaceState::InPit:
            switch (event.type) {
                case RaceEventType::PitExit:
                    state_ = DriverRaceState::Racing;
                    return;
                default:
                    break;
            }
            break;

        case DriverRaceState::Finished:
            // No legal event after Finished.
            break;
    }

    throw IllegalStateTransition(
        "RaceState::apply - illegal event for current driver state");
}

EventLogValidation validateEventLog(const EventLog& log) {
    EventLogValidation result;
    std::unordered_map<int, RaceState> states;  // never iterated: deterministic
    const std::vector<RaceEvent>& entries = log.entries();

    for (std::size_t i = 0; i < entries.size(); ++i) {
        const RaceEvent& event = entries[i];

        if (i > 0 && eventComesBefore(event, entries[i - 1])) {
            result.valid = false;
            result.badIndex = i;
            result.message = "event is out of canonical order";
            return result;
        }

        auto found = states.find(event.driverNumber);
        if (found == states.end()) {
            found = states.emplace(event.driverNumber, RaceState(event.driverNumber)).first;
        }
        try {
            found->second.apply(event);
        } catch (const IllegalStateTransition& error) {
            result.valid = false;
            result.badIndex = i;
            result.message = error.what();
            return result;
        }
    }
    return result;
}

}  // namespace boxbox