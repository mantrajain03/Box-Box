#ifndef BOXBOX_CLI_DRIVER_REPORT_HPP
#define BOXBOX_CLI_DRIVER_REPORT_HPP

#include <cstddef>
#include <iosfwd>

#include "boxbox/DriverAnalytics.hpp"
#include "boxbox/RaceSimulator.hpp"

namespace boxbox {

/// The driver-analytics reports (plan sec. 4.3). These print only: every
/// number comes from `analytics`, and compound names from `simulator`.

/// --driver-stats: each car's time breakdown, lap-time statistics, stints
/// and positions, then the pace and consistency rankings.
void printDriverStats(std::ostream& out, const RaceSimulator& simulator,
                      const DriverAnalytics& analytics);

/// --top-laps K: the K fastest and K slowest laps of the race, and the
/// fastest lap on each compound. `filter` narrows every table to one car
/// and/or a range of laps (--top-laps-car, --top-laps-range); the default
/// considers every lap.
/// \throws std::invalid_argument if the filter names a car that is not in the
///         field.
void printTopLaps(std::ostream& out, const RaceSimulator& simulator,
                  const DriverAnalytics& analytics, std::size_t k,
                  const LapFilter& filter = LapFilter{});

/// --lap-window W: each car's quickest and slowest run of W laps, and its
/// widest lap-time spread within W laps.
void printLapWindows(std::ostream& out, const DriverAnalytics& analytics, std::size_t window);

/// --sector-stats: each car's fastest and average modelled sector and best
/// combined lap, the field's best sectors, and each sector's distribution.
/// Sector times are a MODELLED split of each lap (see LapRecord); the report
/// says so and prints "no sector data" for a track without sectors.
void printSectorStats(std::ostream& out, const RaceSimulator& simulator,
                      const DriverAnalytics& analytics);

/// --sector-compare A:B: sector-by-sector comparison of two cars.
/// \throws std::invalid_argument if either car is not in the field or has no
///         sector data.
void printSectorComparison(std::ostream& out, const RaceSimulator& simulator,
                           const DriverAnalytics& analytics, int carA, int carB);

}  // namespace boxbox

#endif  // BOXBOX_CLI_DRIVER_REPORT_HPP
