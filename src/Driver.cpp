#include "boxbox/Driver.hpp"

#include <stdexcept>
#include <utility>

namespace boxbox {

Driver::Driver(int number, std::string name, std::vector<Stint> plan)
    : number_(number), name_(std::move(name)), plan_(std::move(plan)) {
    for (const Stint& stint : plan_) {
        if (stint.laps <= 0) {
            throw std::invalid_argument(
                "Driver: every planned stint must be at least one lap long");
        }
        plannedLaps_ += stint.laps;
    }
}

// O(1). plan_ is fixed at construction, so its lap total is summed once there
// rather than on every call: finished() runs several times per car per lap,
// and re-summing would make every one of those calls O(S).
int Driver::plannedLaps() const noexcept {
    return plannedLaps_;
}

bool Driver::finished() const noexcept {
    return lapsCompleted_ >= plannedLaps();
}

std::size_t Driver::currentCompoundIndex() const noexcept {
    if (plan_.empty()) {
        return 0;
    }
    // stintIndex_ stops advancing on the final stint, so this stays in range
    // after the driver has finished and still reports the tires it ended on.
    return plan_[stintIndex_].compoundIndex;
}

std::string Driver::currentCompoundName() const {
    if (plan_.empty()) {
        return std::string();
    }
    return plan_[stintIndex_].compoundName;
}

void Driver::reset() noexcept {
    stintIndex_ = 0;
    tireAge_ = 0;
    lapsCompleted_ = 0;
    stopsMade_ = 0;
    cumulativeTime_ = 0.0;
    lastLapTime_ = 0.0;
    trafficLoss_ = 0.0;
}

// FR-6. O(1).
//
// Charging order matters and mirrors PRD sec. 3.4 exactly: the lap is run
// first, then the stop is taken at the END of that lap. An m-stint plan
// therefore charges m-1 pit losses, because the loop below never fires on the
// final stint.
bool Driver::completeLap(double lapTime, double pitLoss, double trafficDelay) noexcept {
    if (finished()) {
        return false;
    }

    // lapTime + 0.0 is exactly lapTime, so a car that is never held up
    // accumulates bit-for-bit the same total it did before FR-9 existed.
    const double lapRun = lapTime + trafficDelay;
    cumulativeTime_ += lapRun;
    lastLapTime_ = lapRun;
    trafficLoss_ += trafficDelay;
    ++lapsCompleted_;
    ++tireAge_;

    // tireAge_ now equals the number of laps run on this set, so it is also
    // the count of laps completed within the current stint.
    if (tireAge_ >= plan_[stintIndex_].laps && stintIndex_ + 1U < plan_.size()) {
        cumulativeTime_ += pitLoss;
        ++stintIndex_;
        tireAge_ = 0;
        ++stopsMade_;
        return true;
    }
    return false;
}

}  // namespace boxbox
