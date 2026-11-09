// Box Box: benchmarks for the optimizer and the structures not covered by
// bench_range_queries.cpp (plan sec. 4.7).
//
//   make bench
//
// Same rules as bench_range_queries.cpp: fixed seeds and sizes, correctness
// checked first and separately with no clock involved, each timing the median
// of kRepeats runs, and a timing is never a pass or fail condition.
//
// This is part of the benchmark binary only (built by `make bench`), which is
// the one place the project reads std::chrono::steady_clock (NFR-5 exception
// recorded in docs/TIRE_MODULE.md and PRD sec. 5).

#include "bench_strategy.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "boxbox/Common.hpp"
#include "boxbox/DriverAnalytics.hpp"
#include "boxbox/EventPriorityQueue.hpp"
#include "boxbox/LapRecord.hpp"
#include "boxbox/MonotonicDeque.hpp"
#include "boxbox/PitStrategy.hpp"
#include "boxbox/RaceEvent.hpp"
#include "boxbox/StrategyAnalytics.hpp"
#include "boxbox/TireCompound.hpp"
#include "boxbox/Track.hpp"

namespace {

constexpr int kRepeats = 5;

/// Stops the optimizer from deleting a timed loop whose result is unused.
volatile double g_sink = 0.0;

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

void report(const std::string& operation, const std::string& dimensions, bool correct,
            const std::string& complexity, const std::string& reference, double implMs,
            double refMs) {
    std::cout << "Operation: " << operation << "\n"
              << "Input: " << dimensions << "\n"
              << "Reference: " << reference << "\n"
              << "Correctness: " << (correct ? "PASS" : "FAIL") << "\n"
              << "Theoretical complexity: " << complexity << "\n"
              << std::fixed << std::setprecision(3)
              << "Measured implementation time: " << implMs << " ms (median of " << kRepeats
              << ")\n";
    if (refMs >= 0.0) {
        std::cout << "Reference implementation time: " << refMs << " ms (median of " << kRepeats
                  << ")\n";
    }
    std::cout << "\n";
}

std::vector<boxbox::TireCompound> syntheticCompounds(std::size_t count) {
    std::vector<boxbox::TireCompound> compounds;
    for (std::size_t i = 0; i < count; ++i) {
        const double step = static_cast<double>(i);
        compounds.emplace_back("C" + std::to_string(i), -0.6 + 0.15 * step, 0.09 - 0.004 * step,
                               20 + static_cast<int>(3 * i), 14 + static_cast<int>(2 * i), 0.18);
    }
    return compounds;
}

/// The DP again, with every stint cost summed lap by lap instead of read from
/// the prefix table: O(N*C*M^2). Written without PitStrategy::stintCost.
double directDp(const boxbox::Track& track, const std::vector<boxbox::TireCompound>& compounds) {
    const int laps = track.totalLaps();
    std::vector<double> dp(static_cast<std::size_t>(laps) + 1U, boxbox::kUnreachable);
    dp[0] = 0.0;
    for (int lap = 1; lap <= laps; ++lap) {
        for (const boxbox::TireCompound& c : compounds) {
            for (int len = 1; len <= std::min(lap, c.maxStintLaps()); ++len) {
                double stint = 0.0;
                for (int age = 0; age < len; ++age) {
                    stint += track.baseLapTime() + c.lapDelta(age);
                }
                const double before = dp[static_cast<std::size_t>(lap - len)];
                const double candidate = before + stint + (lap - len > 0 ? track.pitLoss() : 0.0);
                dp[static_cast<std::size_t>(lap)] =
                    std::min(dp[static_cast<std::size_t>(lap)], candidate);
            }
        }
    }
    return dp[static_cast<std::size_t>(laps)];
}

bool benchOptimizer(const std::string& label, int laps, std::size_t compoundCount) {
    std::cout << "---- DP optimizer: " << label << " ----\n\n";
    boxbox::Track track("Bench", laps, 22.0);
    track.addSector("S1", 90.0);
    const std::vector<boxbox::TireCompound> compounds = syntheticCompounds(compoundCount);
    const boxbox::PitStrategy strategy(track, compounds);

    const boxbox::StrategyResult result = strategy.optimize();
    const bool ok = result.feasible && result.plannedLaps() == laps &&
                    boxbox::nearlyEqual(result.totalTime, directDp(track, compounds), 1e-6) &&
                    boxbox::nearlyEqual(result.totalTime, result.dpValue, 1e-6);

    const double implMs = medianMillis([&] { g_sink = strategy.optimize().totalTime; });
    const double refMs = medianMillis([&] { g_sink = directDp(track, compounds); });
    std::ostringstream dims;
    dims << "N=" << laps << " laps, C=" << compoundCount << " compounds, M="
         << strategy.maxStintLaps() << " (about " << static_cast<long long>(laps) *
                static_cast<long long>(compoundCount) * strategy.maxStintLaps()
         << " DP transitions)";
    report("Optimizer (prefix-sum DP)", dims.str(), ok, "O(N*C*M)",
           "same DP summing each stint lap by lap, O(N*C*M^2)", implMs, refMs);
    return ok;
}

bool benchStintCost(std::size_t queries) {
    std::cout << "---- prefix-sum stint cost ----\n\n";
    boxbox::Track track("Bench", 100, 22.0);
    track.addSector("S1", 90.0);
    const std::vector<boxbox::TireCompound> compounds = syntheticCompounds(6);
    const boxbox::PitStrategy strategy(track, compounds);

    Lcg rng(31ULL);
    struct Q {
        std::size_t c;
        int len;
    };
    std::vector<Q> qs;
    for (std::size_t i = 0; i < queries; ++i) {
        const std::size_t c = static_cast<std::size_t>(rng.next() % compounds.size());
        const int len = 1 + static_cast<int>(rng.next() % static_cast<std::uint64_t>(compounds[c].maxStintLaps()));
        qs.push_back(Q{c, len});
    }
    const auto direct = [&](const Q& q) {
        double s = 0.0;
        for (int age = 0; age < q.len; ++age) {
            s += track.baseLapTime() + compounds[q.c].lapDelta(age);
        }
        return s;
    };

    bool ok = true;
    for (const Q& q : qs) {
        ok = ok && boxbox::nearlyEqual(strategy.stintCost(q.c, q.len), direct(q), 1e-6);
    }
    const double implMs = medianMillis([&] {
        double s = 0.0;
        for (const Q& q : qs) s += strategy.stintCost(q.c, q.len);
        g_sink = s;
    });
    const double refMs = medianMillis([&] {
        double s = 0.0;
        for (const Q& q : qs) s += direct(q);
        g_sink = s;
    });
    report("Stint cost query", std::to_string(queries) + " queries, stints up to 38 laps", ok,
           "O(1) per query after an O(C*M) build", "direct summation, O(L) per query", implMs, refMs);
    return ok;
}

bool benchKBest(std::size_t k) {
    std::cout << "---- K cheapest plans: K=" << k << " ----\n\n";
    boxbox::Track track("Bench", 60, 22.0);
    track.addSector("S1", 90.0);
    const boxbox::PitStrategy strategy(track, boxbox::defaultCompounds());

    const std::vector<boxbox::StrategyReport> plans = boxbox::kBestPlans(strategy, k);
    bool ok = plans.size() == k && boxbox::nearlyEqual(plans.front().totalTime,
                                                      strategy.optimize().totalTime, 1e-9);
    for (std::size_t i = 0; i < plans.size(); ++i) {
        ok = ok && plans[i].feasible &&
             boxbox::nearlyEqual(plans[i].totalTime, strategy.evaluatePlan(plans[i].plan).totalTime, 1e-9);
        if (i > 0) {
            ok = ok && plans[i].totalTime >= plans[i - 1].totalTime - boxbox::kTimeEpsilon;
            for (std::size_t j = 0; j < i; ++j) {
                ok = ok && !boxbox::samePlan(plans[i].plan, plans[j].plan);
            }
        }
    }

    const double implMs = medianMillis([&] { g_sink = boxbox::kBestPlans(strategy, k).back().totalTime; });
    const double refMs = medianMillis([&] { g_sink = strategy.optimize().totalTime; });
    report("kBestPlans (exact K cheapest)", "N=60 laps, C=3, K=" + std::to_string(k), ok,
           "O(N*C*M*K log(C*M*K)) time, O(N*K) memory",
           "optimize() alone (the K=1 cost, for scale)", implMs, refMs);
    return ok;
}

bool benchEventQueue(const std::string& label, std::size_t events) {
    std::cout << "---- event priority queue: " << label << " ----\n\n";
    Lcg rng(41ULL);
    std::vector<boxbox::RaceEvent> input;
    for (std::size_t i = 0; i < events; ++i) {
        boxbox::RaceEvent e;
        e.type = static_cast<boxbox::RaceEventType>(rng.next() % 7U);
        e.timestamp = static_cast<double>(rng.next() % 5000U);  // many equal times
        e.driverNumber = 1 + static_cast<int>(rng.next() % 20U);
        input.push_back(e);
    }
    const auto sortAll = [&] {
        std::vector<boxbox::RaceEvent> all = input;
        for (std::size_t i = 0; i < all.size(); ++i) all[i].sequenceId = i;
        std::sort(all.begin(), all.end(), [](const boxbox::RaceEvent& a, const boxbox::RaceEvent& b) {
            return boxbox::eventComesBefore(a, b);
        });
        return all;
    };
    const auto viaQueue = [&] {
        boxbox::EventPriorityQueue q;
        for (const boxbox::RaceEvent& e : input) q.push(e);
        std::vector<boxbox::RaceEvent> out;
        out.reserve(input.size());
        while (!q.empty()) out.push_back(q.pop());
        return out;
    };

    const std::vector<boxbox::RaceEvent> a = viaQueue();
    const std::vector<boxbox::RaceEvent> b = sortAll();
    bool ok = a.size() == b.size();
    for (std::size_t i = 0; ok && i < a.size(); ++i) {
        ok = a[i].sequenceId == b[i].sequenceId;
    }
    const double implMs = medianMillis([&] { g_sink = static_cast<double>(viaQueue().size()); });
    const double refMs = medianMillis([&] { g_sink = static_cast<double>(sortAll().size()); });
    report("Event priority queue: push all, pop all", std::to_string(events) + " events", ok,
           "O(E log E): push and pop O(log E) each", "sort all events by the same ordering, O(E log E)",
           implMs, refMs);
    return ok;
}

bool benchTopK(std::size_t laps, std::size_t k) {
    std::cout << "---- top-K laps: " << laps << " laps, K=" << k << " ----\n\n";
    Lcg rng(51ULL);
    std::vector<boxbox::LapRecord> records(laps);
    for (std::size_t i = 0; i < laps; ++i) {
        records[i].driverNumber = 1 + static_cast<int>(rng.next() % 20U);
        records[i].lap = static_cast<int>(i % 60U) + 1;
        records[i].lapTime = 85.0 + static_cast<double>(rng.next() % 100000U) / 10000.0;
    }
    const auto viaSort = [&] {
        std::vector<boxbox::LapRecord> all = records;
        std::sort(all.begin(), all.end(), [](const boxbox::LapRecord& a, const boxbox::LapRecord& b) {
            return boxbox::lapRanksAhead(a, b, boxbox::LapOrder::Fastest);
        });
        all.resize(std::min(k, all.size()));
        return all;
    };
    const std::vector<boxbox::LapRecord> a = boxbox::selectLaps(records, k, boxbox::LapOrder::Fastest);
    const std::vector<boxbox::LapRecord> b = viaSort();
    bool ok = a.size() == b.size();
    for (std::size_t i = 0; ok && i < a.size(); ++i) {
        ok = a[i].driverNumber == b[i].driverNumber && a[i].lap == b[i].lap &&
             a[i].lapTime == b[i].lapTime;
    }
    const double implMs = medianMillis([&] {
        g_sink = boxbox::selectLaps(records, k, boxbox::LapOrder::Fastest).front().lapTime;
    });
    const double refMs = medianMillis([&] { g_sink = viaSort().front().lapTime; });
    report("Bounded-heap fastest K laps", std::to_string(laps) + " laps, K=" + std::to_string(k), ok,
           "O(L log K) time, O(K) space", "full sort of every lap, O(L log L)", implMs, refMs);
    return ok;
}

bool benchDeque(std::size_t length, std::size_t window) {
    std::cout << "---- monotonic deque: " << length << " values, W=" << window << " ----\n\n";
    Lcg rng(61ULL);
    std::vector<double> values(length);
    for (double& v : values) v = 85.0 + static_cast<double>(rng.next() % 100000U) / 10000.0;

    const auto viaDeque = [&] {
        boxbox::MonotonicDeque dq(window, boxbox::MonotonicDeque::Kind::Minimum);
        std::vector<double> out;
        out.reserve(values.size());
        for (const double v : values) {
            dq.push(v);
            out.push_back(dq.extreme());
        }
        return out;
    };
    const auto brute = [&] {
        std::vector<double> out;
        out.reserve(values.size());
        for (std::size_t i = 0; i < values.size(); ++i) {
            const std::size_t from = i + 1U > window ? i + 1U - window : 0U;
            double m = values[from];
            for (std::size_t j = from + 1U; j <= i; ++j) m = std::min(m, values[j]);
            out.push_back(m);
        }
        return out;
    };
    const std::vector<double> a = viaDeque();
    const std::vector<double> b = brute();
    bool ok = a.size() == b.size();
    for (std::size_t i = 0; ok && i < a.size(); ++i) {
        ok = a[i] == b[i];  // both return an element of the input, so the values are identical
    }
    const double implMs = medianMillis([&] { g_sink = viaDeque().back(); });
    const double refMs = medianMillis([&] { g_sink = brute().back(); });
    report("Sliding-window minimum", std::to_string(length) + " values, W=" + std::to_string(window), ok,
           "O(L) total, O(1) amortised per value", "rescan every window, O(L*W)", implMs, refMs);
    return ok;
}

}  // namespace

bool runStrategyBenchmarks() {
    std::cout << "Algorithm Benchmark: optimizer, strategy lab and shared structures\n"
              << "Timings vary by machine and are never a pass/fail check.\n\n";
    bool ok = true;
    ok = benchOptimizer("small (a real race)", 50, 3) && ok;
    ok = benchOptimizer("medium", 300, 3) && ok;
    ok = benchOptimizer("large", 2000, 3) && ok;
    ok = benchOptimizer("more compounds", 300, 9) && ok;
    ok = benchStintCost(200000) && ok;
    ok = benchKBest(1) && ok;
    ok = benchKBest(5) && ok;
    ok = benchKBest(20) && ok;
    ok = benchKBest(50) && ok;
    ok = benchEventQueue("small", 1000) && ok;
    ok = benchEventQueue("large", 200000) && ok;
    ok = benchTopK(1000, 10) && ok;
    ok = benchTopK(200000, 10) && ok;
    ok = benchDeque(1000, 5) && ok;
    ok = benchDeque(200000, 50) && ok;
    return ok;
}
