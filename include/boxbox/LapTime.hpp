#ifndef BOXBOX_LAP_TIME_HPP
#define BOXBOX_LAP_TIME_HPP

#include "boxbox/TireCompound.hpp"
#include "boxbox/Track.hpp"

namespace boxbox {

/// The canonical lap time (PRD sec. 3.2): the track's reference lap plus the
/// compound's offset, wear and cliff for a tire of `tireAge` laps (basePace
/// ADDS to the track base time). O(1).
///
/// This is the single place that sum is written. RaceSimulator::lapTimeFor and
/// the tire analytics both call it, so they cannot drift apart. The optimizer's
/// prefix table sums the same TireCompound::lapDelta values, which is what the
/// FR-8 loop-closure check keeps honest.
inline double canonicalLapTime(const Track& track, const TireCompound& compound,
                               int tireAge) {
    return track.baseLapTime() + compound.lapDelta(tireAge);
}

}  // namespace boxbox

#endif  // BOXBOX_LAP_TIME_HPP
