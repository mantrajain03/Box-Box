#ifndef UNDERCUT_DRIVER_HPP
#define UNDERCUT_DRIVER_HPP

#include <cstddef>
#include <string>
#include <vector>

#include "undercut/PitStrategy.hpp"

namespace undercut {

class RaceSimulator;

/// One car in the race: its identity, the stint plan it is following, and the
/// mutable state the simulator advances lap by lap.
///
/// A Driver carries no pace model of its own. It does not know the track's
/// base lap time and it does not know how a compound degrades. RaceSimulator
/// applies the one lap-time model (PRD sec. 3.2) and hands the resulting lap
/// time here to be charged. Keeping exactly one implementation of that model
/// in the project is what makes the FR-8 loop closure a real check rather than
/// two copies of the same arithmetic agreeing with each other.
class Driver {
public:
    /// \throws std::invalid_argument if any stint in `plan` is empty or has a
    ///         non-positive lap count.
    Driver(int number, std::string name, std::vector<Stint> plan);

    int number() const noexcept { return number_; }
    const std::string& name() const noexcept { return name_; }
    const std::vector<Stint>& plan() const noexcept { return plan_; }

    /// Total laps this driver's plan covers. O(S) for an S-stint plan.
    int plannedLaps() const noexcept;

    // --- mutable race state ------------------------------------------------

    /// Index of the stint in progress, clamped to the last stint once the
    /// driver has finished. 0 for an empty plan.
    std::size_t stintIndex() const noexcept { return stintIndex_; }

    /// Laps run on the current set of tires. 0 on a fresh set.
    int tireAge() const noexcept { return tireAge_; }

    int lapsCompleted() const noexcept { return lapsCompleted_; }
    int stopsMade() const noexcept { return stopsMade_; }
    double cumulativeTime() const noexcept { return cumulativeTime_; }
    double lastLapTime() const noexcept { return lastLapTime_; }

    bool finished() const noexcept;

    /// Index into the simulator's compound set for the stint in progress.
    /// Returns 0 for an empty plan; check plan().empty() first if that matters.
    std::size_t currentCompoundIndex() const noexcept;

    /// Name of the compound currently fitted, or an empty string for an empty
    /// plan. Returned by value: there is no stable reference for that case.
    std::string currentCompoundName() const;

    /// Return to the grid: lap 0, first stint, fresh tires, zero time.
    void reset() noexcept;

private:
    friend class RaceSimulator;

    /// Charge one completed lap of `lapTime` seconds.
    ///
    /// If the lap ended a stint that is followed by another, `pitLoss` is
    /// charged too and the driver switches to the next stint's compound with
    /// its tire age reset to 0 (FR-6). Returns true in exactly that case.
    /// No pit loss is charged after the final stint: the race has ended.
    bool completeLap(double lapTime, double pitLoss) noexcept;

    int number_;
    std::string name_;
    std::vector<Stint> plan_;

    std::size_t stintIndex_ = 0;
    int tireAge_ = 0;
    int lapsCompleted_ = 0;
    int stopsMade_ = 0;
    double cumulativeTime_ = 0.0;
    double lastLapTime_ = 0.0;
};

}  // namespace undercut

#endif  // UNDERCUT_DRIVER_HPP
