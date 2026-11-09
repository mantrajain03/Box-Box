#ifndef BOXBOX_CLI_TIRE_COMMANDS_HPP
#define BOXBOX_CLI_TIRE_COMMANDS_HPP

#include <iosfwd>
#include <string>
#include <vector>

#include "boxbox/PitStrategy.hpp"

namespace boxbox {

/// Exit statuses, matching the rest of the CLI.
inline constexpr int kTiresOk = 0;
inline constexpr int kTiresUsageError = 1;

/// The `tires` command group. main.cpp builds the PitStrategy from the shared
/// race options (--track, --sector, --compound, ...) and passes everything
/// after the word `tires` here:
///
///     tires profile [--compound NAME]
///     tires range --compound NAME --from AGE --to AGE [--override AGE=SECONDS ...]
///     tires threshold --compound NAME --max-degradation SECONDS
///     tires threshold --compound NAME --max-average SECONDS
///     tires threshold --compound NAME --slower-than OTHER
///     tires help
///
/// This file parses and prints only. Every number comes from TireAnalytics.
/// Returns kTiresOk, or kTiresUsageError after printing a message to `err`.
int runTiresCommand(const std::vector<std::string>& args, const PitStrategy& strategy,
                    std::ostream& out, std::ostream& err);

void printTiresHelp(std::ostream& out);

}  // namespace boxbox

#endif  // BOXBOX_CLI_TIRE_COMMANDS_HPP
