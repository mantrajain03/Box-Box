#ifndef BOXBOX_CLI_EXPORT_COMMAND_HPP
#define BOXBOX_CLI_EXPORT_COMMAND_HPP

#include <cstddef>
#include <iosfwd>
#include <string>
#include <vector>

#include "boxbox/PitStrategy.hpp"
#include "boxbox/RaceSimulator.hpp"

namespace boxbox {

/// Exit statuses, matching the rest of the CLI.
inline constexpr int kExportOk = 0;
inline constexpr int kExportUsageError = 1;   ///< bad arguments, or the files could not be written
inline constexpr int kExportInfeasible = 2;   ///< no feasible strategy for this race
inline constexpr int kExportCheckFailed = 3;  ///< a self-check on the exported data failed

/// The `export` command group. main.cpp builds the PitStrategy from the shared
/// race options and passes everything after the word `export` here:
///
///     export [--out DIR] [--max-stops N] [--alternatives K]
///            [--sweep FROM:TO:STEP] [--min-gap S] [--pass-margin S]
///
/// It writes DIR/bundle.json and DIR/bundle.js (the same data; the .js form is
/// `window.BOXBOX = {...};` so a page opened from file:// can load it with a
/// plain <script> tag). The web frontend renders this bundle and computes
/// nothing of its own.
///
/// This file only runs the existing core and serialises what comes back. The one
/// exception is the DP fill trace and the brute-force reference, which replay the
/// optimizer's recurrence over PitStrategy's public stintCost() so the frontend
/// can animate it; both are checked against PitStrategy::optimize() before
/// anything is written, so they cannot drift from the real optimizer.
int runExportCommand(const std::vector<std::string>& args, const PitStrategy& strategy,
                     std::ostream& out, std::ostream& err);

void printExportHelp(std::ostream& out);

/// What the bundle contains beyond the race itself.
struct ExportSettings {
    OvertakeRule overtake;       ///< used for the "overtake on" run; `enabled` is forced on
    int maxStops = 3;            ///< stop counts 0..maxStops in the stop-count comparison
    std::size_t alternatives = 5;  ///< how many cheapest distinct plans to list
    double sweepFrom = 0.0;      ///< pit-loss sweep range, seconds
    double sweepTo = 40.0;
    double sweepStep = 2.0;
};

/// Write the whole bundle as one JSON document. Deterministic: the same inputs
/// give byte-identical output (no clock, no timings, no randomness).
///
/// Returns false, with the reason in `failure`, when a self-check fails: the DP
/// trace disagrees with PitStrategy::optimize(), the FR-8 loop closure fails, or
/// an event log fails state-machine validation. The stream then holds a partial
/// document that must not be used.
///
/// COST: dominated by the sweep, O(P*N*C*M) for P sweep points, plus two race
/// replays and the stop-count and alternatives queries (see StrategyAnalytics).
/// \throws std::invalid_argument for a bad setting or an infeasible/empty race.
bool writeBundleJson(std::ostream& out, const PitStrategy& strategy, const ExportSettings& settings,
                     std::string& failure);

}  // namespace boxbox

#endif  // BOXBOX_CLI_EXPORT_COMMAND_HPP
