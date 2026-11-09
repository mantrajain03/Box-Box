#ifndef BOXBOX_TESTS_EVENT_TESTS_HPP
#define BOXBOX_TESTS_EVENT_TESTS_HPP

/// Entry point for the race-event-engine checks (RaceEvent ordering,
/// EventPriorityQueue, EventLog, RaceState, PitAnalytics and the simulator's
/// event sink). They live in their own file, like the tire and driver
/// modules', and test_main.cpp adds the totals to its own.

/// Checks run and checks failed, for test_main.cpp to add to its totals.
struct EventTestTally {
    int checks = 0;
    int failures = 0;
};

/// Run every event-engine check and return the counts.
EventTestTally runEventEngineTests();

#endif  // BOXBOX_TESTS_EVENT_TESTS_HPP
