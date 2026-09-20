#ifndef BOXBOX_TIRE_COMPOUND_HPP
#define BOXBOX_TIRE_COMPOUND_HPP

#include <string>
#include <vector>

namespace boxbox {

/// A tire compound's deterministic pace model (PRD FR-4, sec. 3.2).
///
/// The class answers exactly one question: how many seconds does this compound
/// add to the track's reference lap time when the tire is `tireAge` laps old?
///
///     lapDelta(age) = basePace
///                   + degradationPerLap * age
///                   + cliffDegradationPerLap * max(0, age - cliffLap)
///
/// `basePace` is an OFFSET relative to the track's base lap time, not a
/// replacement for it: soft compounds carry a negative basePace, hard
/// compounds a positive one. A compound set is therefore track-independent.
///
/// The cliff term models the sharp fall-off real tires suffer past a
/// threshold. Set `cliffLap` to a negative value to disable it, which makes
/// degradation purely linear.
///
/// Depends on nothing but the standard library and can be used with no other
/// Box Box type in existence (NFR-3).
class TireCompound {
public:
    /// \param name                   display name, e.g. "SOFT"
    /// \param basePace               seconds/lap relative to the track base lap
    /// \param degradationPerLap      seconds added per lap of tire age, >= 0
    /// \param maxStintLaps           longest usable stint on this compound, >= 1
    /// \param cliffLap               age past which the cliff applies; < 0 disables
    /// \param cliffDegradationPerLap extra seconds per lap beyond the cliff, >= 0
    ///
    /// \throws std::invalid_argument if maxStintLaps < 1, or either degradation
    ///         rate is negative, or any argument is not finite.
    TireCompound(std::string name,
                 double basePace,
                 double degradationPerLap,
                 int maxStintLaps,
                 int cliffLap = -1,
                 double cliffDegradationPerLap = 0.0);

    const std::string& name() const noexcept { return name_; }
    double basePace() const noexcept { return basePace_; }
    double degradationPerLap() const noexcept { return degradationPerLap_; }
    int maxStintLaps() const noexcept { return maxStintLaps_; }
    int cliffLap() const noexcept { return cliffLap_; }
    double cliffDegradationPerLap() const noexcept { return cliffDegradationPerLap_; }

    /// True when this compound has a degradation cliff configured.
    bool hasCliff() const noexcept { return cliffLap_ >= 0; }

    /// Seconds this compound adds to the track's base lap time at `tireAge`.
    /// Age 0 is a fresh set. Negative ages are clamped to 0. O(1).
    double lapDelta(int tireAge) const noexcept;

    /// Single-character display code: the first letter of the name, uppercased.
    /// Returns '?' for an empty name.
    char code() const noexcept;

private:
    std::string name_;
    double basePace_;
    double degradationPerLap_;
    int maxStintLaps_;
    int cliffLap_;
    double cliffDegradationPerLap_;
};

/// The default three-compound set used by the CLI when none is supplied.
/// Figures are illustrative but internally consistent: softer compounds are
/// quicker, degrade faster and cannot run as long.
std::vector<TireCompound> defaultCompounds();

}  // namespace boxbox

#endif  // BOXBOX_TIRE_COMPOUND_HPP
