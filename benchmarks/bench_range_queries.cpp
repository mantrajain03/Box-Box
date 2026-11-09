// Box Box: benchmarks for the range-query structures (plan sec. 4.7).
//
//   make bench
//
// Rules followed:
//   * fixed seeds and fixed sizes; inputs come from the same LCG the tests use
//   * correctness is checked first and separately; a timing is never a pass
//     or fail condition
//   * each timing is the median of kRepeats runs, so one noisy run decides
//     nothing
//   * sizes run from real ones (a 45-age tire profile, a 20-car field)
//     up to 100,000, so the output shows where the structures stop mattering
//     as well as where they help
//
// EXCEPTION TO NFR-5, approved by Sarthak on 2026-11-06: this binary reads
// std::chrono::steady_clock to measure run times. Nothing else in the project
// reads the clock. The clock only times runs; it never seeds data or picks a
// code path, and the core, CLI and tests stay deterministic.
// This binary is built only by `make bench`, never by `make` or `make test`.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "boxbox/Common.hpp"
#include "boxbox/FenwickTree.hpp"
#include "boxbox/RaceLapAnalytics.hpp"
#include "boxbox/SegmentTree.hpp"

namespace {

using boxbox::FenwickTree;
using boxbox::SegmentTree;

constexpr int kRepeats = 5;

/// Stops the optimizer from deleting a timed loop whose result is unused.
volatile double g_sink = 0.0;

/// Same generator as the test harness.
class Lcg {
public:
    explicit Lcg(std::uint64_t seed) noexcept : state_(seed) {}
    std::uint64_t next() noexcept {
        state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
        return state_ >> 33U;
    }

private:
    std::uint64_t state_;
};

struct Query {
    std::size_t begin;
    std::size_t end;
};

std::vector<double> makeValues(Lcg& rng, std::size_t n) {
    std::vector<double> v(n);
    for (double& x : v) {
        x = 80.0 + static_cast<double>(rng.next() % 2000000U) / 100000.0;
    }
    return v;
}

std::vector<Query> makeQueries(Lcg& rng, std::size_t n, std::size_t count) {
    std::vector<Query> queries(count);
    for (Query& q : queries) {
        std::size_t a = static_cast<std::size_t>(rng.next() % n);
        std::size_t b = static_cast<std::size_t>(rng.next() % n);
        if (a > b) {
            std::swap(a, b);
        }
        q = Query{a, b + 1U};
    }
    return queries;
}

template <class Body>
double medianMillis(Body&& body) {
    std::vector<double> runs;
    for (int r = 0; r < kRepeats; ++r) {
        const auto t0 = std::chrono::steady_clock::now();
        body();
        const auto t1 = std::chrono::steady_clock::now();
        runs.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    std::sort(runs.begin(), runs.end());
    return runs[runs.size() / 2U];
}

double bruteSum(const std::vector<double>& v, const Query& q) {
    double s = 0.0;
    for (std::size_t i = q.begin; i < q.end; ++i) {
        s += v[i];
    }
    return s;
}

double bruteMin(const std::vector<double>& v, const Query& q) {
    double m = v[q.begin];
    for (std::size_t i = q.begin + 1U; i < q.end; ++i) {
        m = std::min(m, v[i]);
    }
    return m;
}

void report(const std::string& operation, std::size_t n, std::size_t queries, bool correct,
            const std::string& complexity, const std::string& reference, double implMs,
            double refMs) {
    std::cout << "Operation: " << operation << "\n"
              << "Input size: " << n << "\n"
              << "Queries: " << queries << "\n"
              << "Reference: " << reference << "\n"
              << "Correctness: " << (correct ? "PASS" : "FAIL") << "\n"
              << "Theoretical query complexity: " << complexity << "\n"
              << std::fixed << std::setprecision(3)
              << "Measured implementation time: " << implMs << " ms (median of " << kRepeats
              << ")\n"
              << "Reference implementation time: " << refMs << " ms (median of " << kRepeats
              << ")\n\n";
}

bool benchSize(const std::string& label, std::size_t n, std::size_t queryCount) {
    std::cout << "---- " << label << " ----\n\n";
    Lcg rng(1000ULL + n);
    const std::vector<double> values = makeValues(rng, n);
    const std::vector<Query> queries = makeQueries(rng, n, queryCount);

    // Correctness first, with no clock involved.
    const SegmentTree seg(values);
    bool sumOk = true;
    bool minOk = true;
    for (const Query& q : queries) {
        const double treeSum = seg.rangeSum(q.begin, q.end);
        sumOk = sumOk && boxbox::nearlyEqual(treeSum, bruteSum(values, q), 1e-6);
        // Exact: the tree must return the very element the scan returns.
        const double treeMin = seg.rangeMin(q.begin, q.end);
        minOk = minOk && boxbox::nearlyEqual(treeMin, bruteMin(values, q), 0.0);
    }
    bool fenOk = true;
    {
        FenwickTree check(values);
        std::vector<double> ref = values;
        for (std::size_t i = 0; i < queries.size(); ++i) {
            const Query& q = queries[i];
            if (i % 2U == 0U) {
                check.add(q.begin, 0.25);
                ref[q.begin] += 0.25;
            } else {
                fenOk = fenOk &&
                        boxbox::nearlyEqual(check.rangeSum(q.begin, q.end), bruteSum(ref, q), 1e-6);
            }
        }
    }

    // Then timing.
    const double segSumMs = medianMillis([&] {
        double s = 0.0;
        for (const Query& q : queries) {
            s += seg.rangeSum(q.begin, q.end);
        }
        g_sink = s;
    });
    const double bruteSumMs = medianMillis([&] {
        double s = 0.0;
        for (const Query& q : queries) {
            s += bruteSum(values, q);
        }
        g_sink = s;
    });
    report("Segment Tree Range Sum", n, queryCount, sumOk, "O(log N)", "Brute-force range sum",
           segSumMs, bruteSumMs);

    const double segMinMs = medianMillis([&] {
        double s = 0.0;
        for (const Query& q : queries) {
            s += seg.rangeMin(q.begin, q.end);
        }
        g_sink = s;
    });
    const double bruteMinMs = medianMillis([&] {
        double s = 0.0;
        for (const Query& q : queries) {
            s += bruteMin(values, q);
        }
        g_sink = s;
    });
    report("Segment Tree Range Min", n, queryCount, minOk, "O(log N)", "Brute-force range min",
           segMinMs, bruteMinMs);

    // Half point updates, half range sums: the array wins updates at O(1),
    // the tree wins sums at O(log N) against O(N).
    FenwickTree fen(values);
    std::vector<double> plain = values;
    const double fenMs = medianMillis([&] {
        double s = 0.0;
        for (std::size_t i = 0; i < queries.size(); ++i) {
            const Query& q = queries[i];
            if (i % 2U == 0U) {
                fen.add(q.begin, 0.25);
            } else {
                s += fen.rangeSum(q.begin, q.end);
            }
        }
        g_sink = s;
    });
    const double plainMs = medianMillis([&] {
        double s = 0.0;
        for (std::size_t i = 0; i < queries.size(); ++i) {
            const Query& q = queries[i];
            if (i % 2U == 0U) {
                plain[q.begin] += 0.25;
            } else {
                s += bruteSum(plain, q);
            }
        }
        g_sink = s;
    });
    report("Fenwick Tree Update + Range Sum (50/50 mix)", n, queryCount, fenOk,
           "O(log N) update, O(log N) sum", "Plain array, O(1) update and O(N) sum", fenMs,
           plainMs);

    return sumOk && minOk && fenOk;
}

/// O(D^2) reference: compare every pair, with start positions looked up once.
std::int64_t bruteSwaps(const std::vector<int>& before, const std::vector<int>& after) {
    std::vector<std::size_t> start(after.size());
    for (std::size_t i = 0; i < before.size(); ++i) {
        start[static_cast<std::size_t>(before[i])] = i;  // cars are numbered 0 .. D-1 here
    }
    std::int64_t swaps = 0;
    for (std::size_t i = 0; i < after.size(); ++i) {
        for (std::size_t j = i + 1U; j < after.size(); ++j) {
            const std::size_t startI = start[static_cast<std::size_t>(after[i])];
            const std::size_t startJ = start[static_cast<std::size_t>(after[j])];
            if (startJ < startI) {
                ++swaps;
            }
        }
    }
    return swaps;
}

bool benchSwaps(const std::string& label, std::size_t cars) {
    std::cout << "---- position swaps: " << label << " ----\n\n";
    Lcg rng(5000ULL + cars);
    std::vector<int> before(cars);
    for (std::size_t i = 0; i < cars; ++i) {
        before[i] = static_cast<int>(i);
    }
    std::vector<int> after = before;
    for (std::size_t i = after.size(); i > 1U; --i) {
        std::swap(after[i - 1U], after[static_cast<std::size_t>(rng.next() % i)]);
    }

    const bool ok = boxbox::countOrderSwaps(before, after) == bruteSwaps(before, after);
    const double fenMs = medianMillis([&] {
        g_sink = static_cast<double>(boxbox::countOrderSwaps(before, after));
    });
    const double bruteMs = medianMillis([&] {
        g_sink = static_cast<double>(bruteSwaps(before, after));
    });
    report("Fenwick Tree Position-Swap Count (inversions)", cars, 1, ok, "O(D log D) per count",
           "Every pair compared, O(D^2)", fenMs, bruteMs);
    return ok;
}

}  // namespace

int main() {
    std::cout << "Algorithm Benchmark: range-query structures\n"
              << "Timings vary by machine and are never a pass/fail check.\n\n";
    bool ok = true;
    ok = benchSize("real tire profile size (default HARD: 45 ages)", 45, 5000) && ok;
    ok = benchSize("small", 1000, 5000) && ok;
    ok = benchSize("medium", 10000, 20000) && ok;
    ok = benchSize("large", 100000, 50000) && ok;
    ok = benchSwaps("a real field (20 cars)", 20) && ok;
    ok = benchSwaps("1,000 cars", 1000) && ok;
    ok = benchSwaps("20,000 cars", 20000) && ok;
    std::cout << (ok ? "All correctness checks passed.\n" : "CORRECTNESS FAILURE\n");
    return ok ? 0 : 1;
}
