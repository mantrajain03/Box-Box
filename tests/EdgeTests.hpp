#ifndef BOXBOX_TESTS_EDGE_TESTS_HPP
#define BOXBOX_TESTS_EDGE_TESTS_HPP

/// Entry point for the edge-case and guard checks found by the coverage
/// review: the rejections and degenerate inputs that the module tests did not
/// reach (non-finite arguments, plans that do not cover the race, cars with no
/// laps, records with partial sector data, unusual report inputs).

/// Checks run and checks failed, for test_main.cpp to add to its totals.
struct EdgeTestTally {
    int checks = 0;
    int failures = 0;
};

/// Run every edge-case check and return the counts.
EdgeTestTally runEdgeTests();

#endif  // BOXBOX_TESTS_EDGE_TESTS_HPP
