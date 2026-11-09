#ifndef BOXBOX_CLI_TRACK_OUTLINE_HPP
#define BOXBOX_CLI_TRACK_OUTLINE_HPP

#include <cstddef>
#include <vector>

#include "boxbox/Track.hpp"

namespace boxbox {

struct OutlinePoint {
    double x = 0.0;
    double y = 0.0;
};

/// A stylised drawing of a circuit, for the web frontend only.
///
/// The engine has no track geometry: a Track is a name, a lap count, a pit loss
/// and sector times. This shape is generated from the circuit's name so each
/// circuit looks the same every time, and it is NOT a survey of any real
/// circuit. Nothing in the core or in any lap time depends on it.
struct TrackOutline {
    double width = 0.0;   ///< drawing area, in the same units as the points
    double height = 0.0;

    /// Closed loop of equally spaced points (the first point is not repeated at
    /// the end). Point 0 is on the start/finish line, so a progress p in [0, 1)
    /// along the lap is point p * points.size(), by linear interpolation.
    std::vector<OutlinePoint> points;

    /// Index of each sector's first point. Sector boundaries sit at the same
    /// fraction of the lap as the sector's share of Track::baseLapTime(), so the
    /// modelled sector split (a lap time shared out in proportion to the sector
    /// base times) and a car moving uniformly along the outline agree.
    std::vector<std::size_t> sectorStart;

    /// Open path running beside the start/finish line, entry first, exit last.
    std::vector<OutlinePoint> pitLane;

    /// The two ends of the start/finish line.
    OutlinePoint lineA;
    OutlinePoint lineB;
};

/// Build the outline for `track`. Deterministic: depends only on the track's
/// name and sector times. O(kDensePoints + kPointCount).
TrackOutline makeTrackOutline(const Track& track);

/// Number of points in TrackOutline::points.
inline constexpr std::size_t kOutlinePointCount = 240;

}  // namespace boxbox

#endif  // BOXBOX_CLI_TRACK_OUTLINE_HPP
