#ifndef BOXBOX_TESTS_TIRE_MODULE_TESTS_HPP
#define BOXBOX_TESTS_TIRE_MODULE_TESTS_HPP

/// Entry point for the tire module's checks (SegmentTree, FenwickTree,
/// TireAnalytics, RaceLapAnalytics). They live in their own file so the module
/// can be reviewed separately; test_main.cpp calls this and adds the totals to
/// its own.

/// Checks run and checks failed, for test_main.cpp to add to its totals.
struct TireTestTally {
    int checks = 0;
    int failures = 0;
};

/// Run every tire-module check and return the counts.
TireTestTally runTireModuleTests();

#endif  // BOXBOX_TESTS_TIRE_MODULE_TESTS_HPP
