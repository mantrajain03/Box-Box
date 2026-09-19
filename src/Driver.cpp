#include "undercut/Driver.hpp"

#include <stdexcept>
#include <utility>

namespace undercut {

Driver::Driver(int number, std::string name, std::vector<Stint> plan)
    : number_(number), name_(std::move(name)), plan_(std::move(plan)) {
    for (const Stint& stint : plan_) {
        if (stint.laps <= 0) {
            throw std::invalid_argument(
                "Driver: every planned stint must be at least one lap long");
        }
    }
}

int Driver::plannedLaps() const noexcept {
    int laps = 0;
    for (const Stint& stint : plan_) {
        laps += stint.laps;
    }
    return laps;
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
}

// FR-6. O(1).
//
// Charging order matters and mirrors PRD sec. 3.4 exactly: the lap is run
// first, then the stop is taken at the END of that lap. An m-stint plan
// therefore charges m-1 pit losses, because the loop below never fires on the
// final stint.
bool Driver::completeLap(double lapTime, double pitLoss) noexcept {
    if (finished()) {
        return false;
    }

    cumulativeTime_ += lapTime;
    lastLapTime_ = lapTime;
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

}  // namespace undercut
