#ifndef BOXBOX_CLI_PIT_REPORT_HPP
#define BOXBOX_CLI_PIT_REPORT_HPP

#include <iosfwd>
#include <vector>

#include "boxbox/EventLog.hpp"
#include "boxbox/LapRecord.hpp"
#include "boxbox/TireCompound.hpp"

namespace boxbox {

/// What --pit-report needs to print. Pointers are non-owning and may be null
/// where marked.
struct PitReportInput {
    const EventLog* log = nullptr;                     ///< required
    const std::vector<TireCompound>* compounds = nullptr;  ///< required, for names
    double pitLoss = 0.0;                              ///< seconds per stop, as the race ran
    int raceLaps = 0;                                  ///< race distance, for coverage and windows
    int windowFrom = 0;                                ///< --pit-window FROM:TO; 0 = none
    int windowTo = 0;
    const std::vector<LapRecord>* records = nullptr;   ///< optional: enables traffic after stops
    int trafficLapsAfter = 3;                          ///< laps counted after each stop
    int callWindow = 2;                                ///< +/- laps for the greedy pit-call problem
    int histogramBinLaps = 5;                          ///< stint histogram bin width
};

/// The --pit-report output: printing only, every figure from PitAnalytics.
/// Pit-lane time is the pit loss: the simulator charges a stop as one lump, so
/// no separate entry / service / exit durations exist, and the report says so.
/// \throws std::invalid_argument if `log` or `compounds` is null, a window is
///         out of range, callWindow is negative, or trafficLapsAfter or
///         histogramBinLaps is below 1. The caller's stream formatting is
///         restored on return.
void printPitReport(std::ostream& out, const PitReportInput& in);

}  // namespace boxbox

#endif  // BOXBOX_CLI_PIT_REPORT_HPP
