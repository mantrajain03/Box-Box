#include "boxbox/EventLog.hpp"

namespace boxbox {

void EventLog::record(const RaceEvent& event) {
    entries_.push_back(event);
}

std::vector<RaceEvent> EventLog::entriesForDriver(int driverNumber) const {
    std::vector<RaceEvent> result;
    for (const auto& e : entries_) {
        if (e.driverNumber == driverNumber) {
            result.push_back(e);
        }
    }
    return result;
}

std::vector<RaceEvent> EventLog::entriesOfType(RaceEventType type) const {
    std::vector<RaceEvent> result;
    for (const auto& e : entries_) {
        if (e.type == type) {
            result.push_back(e);
        }
    }
    return result;
}

}  // namespace boxbox