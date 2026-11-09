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
/// fastest lap on each compound.
void printTopLaps(std::ostream& out, const RaceSimulator& simulator,
                  const DriverAnalytics& analytics, std::size_t k);

/// --lap-window W: each car's quickest and slowest run of W laps, and its
/// widest lap-time spread within W laps.
void printLapWindows(std::ostream& out, const DriverAnalytics& analytics, std::size_t window);

}  // namespace boxbox

#endif  // BOXBOX_CLI_DRIVER_REPORT_HPP
