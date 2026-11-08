#include "boxbox/RaceState.hpp"

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

}  // namespace boxbox