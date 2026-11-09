#ifndef BOXBOX_TESTS_STRATEGY_TESTS_HPP
#define BOXBOX_TESTS_STRATEGY_TESTS_HPP

/// Entry point for the strategy-lab checks (StrategyAnalytics: custom-plan
/// evaluation, sweeps, K cheapest plans, stop counts, comparison). Same shape
/// as the other module test files; test_main.cpp adds the totals to its own.

/// Checks run and checks failed, for test_main.cpp to add to its totals.
struct StrategyTestTally {
    int checks = 0;
    int failures = 0;
};

/// Run every strategy-lab check and return the counts.
StrategyTestTally runStrategyAnalysisTests();

#endif  // BOXBOX_TESTS_STRATEGY_TESTS_HPP
