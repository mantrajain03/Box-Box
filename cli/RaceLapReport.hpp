#ifndef BOXBOX_CLI_RACE_LAP_REPORT_HPP
#define BOXBOX_CLI_RACE_LAP_REPORT_HPP

#include <iosfwd>

#include "boxbox/RaceLapAnalytics.hpp"
#include "boxbox/RaceSimulator.hpp"

namespace boxbox {

/// The `--race-laps FROM:TO` report: for laps FROM to TO, each car's fastest
/// lap and total lap time, and how many pairs of cars swapped places during
/// those laps. Prints only; every number comes from `recorder`.
///
/// Swaps compare the running order at the end of lap FROM - 1 (the grid when
/// FROM is 1) with the running order at the end of lap TO.
///
/// \throws std::out_of_range unless 1 <= firstLap <= lastLap <= laps recorded.
void printRaceLapReport(std::ostream& out, const RaceSimulator& simulator,
                        const RaceLapRecorder& recorder, int firstLap, int lastLap);

}  // namespace boxbox

#endif  // BOXBOX_CLI_RACE_LAP_REPORT_HPP
