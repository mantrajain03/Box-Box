#ifndef BOXBOX_TESTS_CLI_TESTS_HPP
#define BOXBOX_TESTS_CLI_TESTS_HPP

/// Entry point for the CLI-handler checks: the `strategy` and `tires` command
/// groups and the report printers (pit, driver, sector, race-lap). They take
/// std::ostream, so they are exercised in-process with string streams. main.cpp
/// itself (argument parsing and the entry point) is not linked into the tests.

/// Checks run and checks failed, for test_main.cpp to add to its totals.
struct CliTestTally {
    int checks = 0;
    int failures = 0;
};

/// Run every CLI-handler check and return the counts.
CliTestTally runCliTests();

#endif  // BOXBOX_TESTS_CLI_TESTS_HPP
