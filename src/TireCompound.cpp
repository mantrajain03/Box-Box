#include "boxbox/TireCompound.hpp"

#include <cctype>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace boxbox {

namespace {

void requireFinite(double value, const char* what) {
    if (!std::isfinite(value)) {
        throw std::invalid_argument(std::string("TireCompound: ") + what +
                                    " must be a finite number");
    }
}

}  // namespace

TireCompound::TireCompound(std::string name,
                           double basePace,
                           double degradationPerLap,
                           int maxStintLaps,
                           int cliffLap,
                           double cliffDegradationPerLap)
    : name_(std::move(name)),
      basePace_(basePace),
      degradationPerLap_(degradationPerLap),
      maxStintLaps_(maxStintLaps),
      cliffLap_(cliffLap),
      cliffDegradationPerLap_(cliffDegradationPerLap) {
    requireFinite(basePace_, "basePace");
    requireFinite(degradationPerLap_, "degradationPerLap");
    requireFinite(cliffDegradationPerLap_, "cliffDegradationPerLap");

    if (maxStintLaps_ < 1) {
        throw std::invalid_argument("TireCompound: maxStintLaps must be at least 1");
    }
    if (degradationPerLap_ < 0.0) {
        throw std::invalid_argument("TireCompound: degradationPerLap must not be negative");
    }
    if (cliffDegradationPerLap_ < 0.0) {
        throw std::invalid_argument(
            "TireCompound: cliffDegradationPerLap must not be negative");
    }
}

// O(1). Deliberately branch-light and side-effect free: the optimizer calls
// this once per (compound, age) while building the prefix table, and the
// simulator will call it once per driver per lap.
double TireCompound::lapDelta(int tireAge) const noexcept {
    const int age = (tireAge > 0) ? tireAge : 0;

    double delta = basePace_ + degradationPerLap_ * static_cast<double>(age);

    if (cliffLap_ >= 0 && age > cliffLap_) {
        delta += cliffDegradationPerLap_ * static_cast<double>(age - cliffLap_);
    }
    return delta;
}

char TireCompound::code() const noexcept {
    if (name_.empty()) {
        return '?';
    }
    const unsigned char first = static_cast<unsigned char>(name_.front());
    return static_cast<char>(std::toupper(first));
}

// Illustrative but internally consistent: each step toward the harder compound
// is slower on a fresh lap, degrades more gently, reaches its cliff later and
// can run further. No compound can cover a full 50-lap race, so a realistic
// race always requires at least one stop.
std::vector<TireCompound> defaultCompounds() {
    std::vector<TireCompound> set;
    set.reserve(3);
    //                  name       basePace  deg/lap  maxStint  cliffLap  cliffDeg
    set.emplace_back("SOFT",         -0.60,   0.090,        22,       16,    0.180);
    set.emplace_back("MEDIUM",        0.00,   0.055,        32,       26,    0.110);
    set.emplace_back("HARD",          0.55,   0.035,        45,       38,    0.070);
    return set;
}

}  // namespace boxbox
