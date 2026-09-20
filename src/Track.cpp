#include "boxbox/Track.hpp"

#include <cmath>
#include <stdexcept>
#include <utility>

namespace boxbox {

namespace {

void requireFiniteNonNegative(double value, const char* what) {
    if (!std::isfinite(value)) {
        throw std::invalid_argument(std::string("Track: ") + what +
                                    " must be a finite number");
    }
    if (value < 0.0) {
        throw std::invalid_argument(std::string("Track: ") + what +
                                    " must not be negative");
    }
}

}  // namespace

Track::Track(std::string name, int totalLaps, double pitLoss)
    : name_(std::move(name)), totalLaps_(totalLaps), pitLoss_(pitLoss) {
    if (totalLaps_ < 0) {
        throw std::invalid_argument("Track: totalLaps must not be negative");
    }
    requireFiniteNonNegative(pitLoss_, "pitLoss");
}

// O(1) amortised. baseLapTime_ is accumulated here rather than recomputed on
// read, which is what lets baseLapTime() be a genuine O(1) accessor: the
// optimizer reads it inside its innermost loop.
void Track::addSector(std::string name, double baseTime) {
    requireFiniteNonNegative(baseTime, "sector baseTime");

    Sector sector;
    sector.name = std::move(name);
    sector.baseTime = baseTime;

    sectors_.push_back(std::move(sector));
    baseLapTime_ += baseTime;
}

void Track::setTotalLaps(int laps) {
    if (laps < 0) {
        throw std::invalid_argument("Track: totalLaps must not be negative");
    }
    totalLaps_ = laps;
}

void Track::setPitLoss(double seconds) {
    requireFiniteNonNegative(seconds, "pitLoss");
    pitLoss_ = seconds;
}

}  // namespace boxbox
