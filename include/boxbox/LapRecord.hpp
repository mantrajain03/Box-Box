#ifndef BOXBOX_LAP_RECORD_HPP
#define BOXBOX_LAP_RECORD_HPP

#include <cstddef>

namespace boxbox {

/// One car's one completed race lap: the project's shared lap record
/// (plan sec. 10A; driver analytics, owner Mantra). A module that needs
/// per-lap race data should take this type rather than define its own copy.
///
/// The three times, and why nothing is counted twice:
///   lapTime      the lap as run: the car's own pace from PRD sec. 3.2 plus
///                any time lost behind another car (FR-9). The same number as
///                Driver::lastLapTime() for that lap.
///   trafficTime  the part of lapTime lost behind another car. Already INSIDE
///                lapTime, and 0 with overtaking off.
///   pitTime      pit loss charged at the end of the lap (PRD sec. 3.1), 0 on
///                a lap without a stop. OUTSIDE lapTime: the pit lane is not
///                part of the lap.
/// So a car's race time is the sum of lapTime + pitTime over its laps, and
/// lapTime - trafficTime is the lap it would have run in clean air.
///
/// pitStop is kept apart from pitTime because a pit loss of 0 s is a legal
/// setting, and a stop must still count as a stop then.
///
/// Compound, tire age and stint describe the tires the lap was RUN on. For a
/// lap that ends in a stop they are the old set, not the one fitted after it.
///
/// No sector times: the simulator times whole laps only. The sector times in
/// Track::sectors() are track configuration, not a measurement of any car
/// (plan sec. 4.3E), so they are deliberately not copied in here.
struct LapRecord {
    int driverNumber = 0;           ///< car number, as in LeaderboardEntry
    int lap = 0;                    ///< race lap, 1-based (PRD sec. 3.1)
    double lapTime = 0.0;           ///< seconds; includes traffic, excludes pit loss
    double trafficTime = 0.0;       ///< seconds of lapTime lost behind another car
    double pitTime = 0.0;           ///< seconds of pit loss charged after the lap
    bool pitStop = false;           ///< the car stopped at the end of this lap
    std::size_t compoundIndex = 0;  ///< index into the race's compound set
    int tireAge = 0;                ///< tire age during the lap; 0 on a fresh set
    int stint = 0;                  ///< 0-based stint the lap belongs to
    int position = 0;               ///< running position after the lap, 1 = leader
};

}  // namespace boxbox

#endif  // BOXBOX_LAP_RECORD_HPP
