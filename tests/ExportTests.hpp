#ifndef BOXBOX_TESTS_EXPORT_TESTS_HPP
#define BOXBOX_TESTS_EXPORT_TESTS_HPP

/// Entry point for the JSON writer and web-frontend export checks.

/// Checks run and checks failed, for test_main.cpp to add to its totals.
struct ExportTestTally {
    int checks = 0;
    int failures = 0;
};

/// Run every JsonWriter and export-bundle check and return the counts.
ExportTestTally runExportTests();

#endif  // BOXBOX_TESTS_EXPORT_TESTS_HPP
