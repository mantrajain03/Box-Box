#ifndef BOXBOX_CLI_STRATEGY_COMMANDS_HPP
#define BOXBOX_CLI_STRATEGY_COMMANDS_HPP

#include <iosfwd>
#include <string>
#include <vector>

#include "boxbox/PitStrategy.hpp"

namespace boxbox {

/// Exit statuses, matching the rest of the CLI.
inline constexpr int kStrategyOk = 0;
inline constexpr int kStrategyUsageError = 1;
inline constexpr int kStrategyInfeasible = 2;

/// The `strategy` command group. main.cpp builds the PitStrategy from the
/// shared race options (--laps, --pit-loss, --sector, --compound, ...) and
/// passes everything after the word `strategy` here:
///
///     strategy evaluate --plan SPEC
///     strategy compare [--plan SPEC ...] [--max-stops N]
///     strategy alternatives [--count K]
///     strategy sensitivity --pit-loss FROM:TO:STEP
///     strategy sensitivity --compound NAME --param P --range FROM:TO:STEP
///     strategy stops [--max N]
///     strategy traffic [--min-gap SECONDS] [--pass-margin SECONDS]
///     strategy help
///
/// A plan SPEC is a comma-separated list of NAME:LAPS or NAME:FROM-TO stints,
/// e.g. SOFT:15,HARD:35 or SOFT:1-15,HARD:16-50.
///
/// This file parses and prints only. Every number comes from StrategyAnalytics.
/// Returns kStrategyOk; kStrategyUsageError after printing a message to `err`;
/// or kStrategyInfeasible when a plan the user entered cannot be run.
int runStrategyCommand(const std::vector<std::string>& args, const PitStrategy& strategy,
                       std::ostream& out, std::ostream& err);

void printStrategyHelp(std::ostream& out);

}  // namespace boxbox

#endif  // BOXBOX_CLI_STRATEGY_COMMANDS_HPP
