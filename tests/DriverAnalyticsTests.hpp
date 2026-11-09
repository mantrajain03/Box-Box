#ifndef BOXBOX_TESTS_DRIVER_ANALYTICS_TESTS_HPP
#define BOXBOX_TESTS_DRIVER_ANALYTICS_TESTS_HPP

/// Entry point for the driver-analytics checks (LapRecorder, DriverAnalytics,
/// MonotonicDeque, lap selection, windows and rankings). They live in their
/// own file, like the tire module's, so the module can be reviewed on its
/// own; test_main.cpp calls this and adds the totals to its own.

/// Checks run and checks failed, for test_main.cpp to add to its totals.
struct DriverTestTally {
    int checks = 0;
    int failures = 0;
};

/// Run every driver-analytics check and return the counts.
DriverTestTally runDriverAnalyticsTests();

#endif  // BOXBOX_TESTS_DRIVER_ANALYTICS_TESTS_HPP
