#ifndef BOXBOX_TRACK_HPP
#define BOXBOX_TRACK_HPP

#include <cstddef>
#include <string>
#include <vector>

namespace boxbox {

/// One timing sector of a circuit.
struct Sector {
    std::string name;
    double baseTime = 0.0;  ///< seconds, on a notional reference lap
};

/// A circuit and the race being run on it (PRD FR-3).
///
/// `baseLapTime()` is the sum of the sector base times and is the track's whole
/// contribution to a lap: the rest comes from the tire compound. It is
/// maintained incrementally as sectors are added, so the accessor is O(1).
///
/// Constructible and fully usable without a RaceSimulator (NFR-3).
class Track {
public:
    /// \param name      circuit name
    /// \param totalLaps race distance in laps; 0 is allowed and means "no race"
    /// \param pitLoss   seconds lost per pit stop, >= 0
    ///
    /// \throws std::invalid_argument if totalLaps < 0, or pitLoss is negative
    ///         or not finite.
    Track(std::string name, int totalLaps, double pitLoss);

    /// Append a sector. \throws std::invalid_argument if baseTime is negative
    /// or not finite. O(1) amortised.
    void addSector(std::string name, double baseTime);

    const std::string& name() const noexcept { return name_; }
    int totalLaps() const noexcept { return totalLaps_; }
    double pitLoss() const noexcept { return pitLoss_; }

    const std::vector<Sector>& sectors() const noexcept { return sectors_; }
    std::size_t sectorCount() const noexcept { return sectors_.size(); }

    /// Sum of the sector base times, in seconds. A track with no sectors
    /// returns 0.0. O(1): the sum is kept up to date by addSector.
    double baseLapTime() const noexcept { return baseLapTime_; }

    /// \throws std::invalid_argument if laps < 0.
    void setTotalLaps(int laps);

    /// \throws std::invalid_argument if seconds is negative or not finite.
    void setPitLoss(double seconds);

private:
    std::string name_;
    int totalLaps_;
    double pitLoss_;
    std::vector<Sector> sectors_;
    double baseLapTime_ = 0.0;
};

}  // namespace boxbox

#endif  // BOXBOX_TRACK_HPP
