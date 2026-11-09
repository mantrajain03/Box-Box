#include "AlgorithmCatalog.hpp"

// Costs below are quoted from the headers and docs/COMPLEXITY_AUDIT.md, which read each claim
// against its implementation. N laps, C compounds, M longest stint, D cars, E events, R lap
// records, K laps requested, W window, S stints, n values.

namespace boxbox {

const std::vector<AlgorithmInfo>& algorithmCatalog() {
    static const std::vector<AlgorithmInfo> catalog = {
        {"dp", "Dynamic programming and backtracking", "Optimisation",
         "Finds the cheapest way to split the race into stints. dp[L] is the cheapest way to run L laps: the minimum over "
         "compound c and stint length l of dp[L-l] + stintCost[c][l], plus the pit loss when it is not the first stint. "
         "The stored choices are walked backwards from the finish to recover the plan.",
         {"include/boxbox/PitStrategy.hpp", "src/PitStrategy.cpp"},
         {{"build the stint-cost table", "O(C*M) time and space"},
          {"stintCost", "O(1)"},
          {"optimize", "O(N*C*M) time, O(N) extra space"},
          {"backtrack", "O(N)"}},
         "Ties go to the first candidate enumerated (compound order, then ascending stint length), so the plan is the same on every run.",
         {"lab", "command"}},

        {"leaderboard", "Binary min-heap leaderboard", "Heaps",
         "Orders the field: more laps first, then less elapsed time, then the lower car number. A hand-written heap in a "
         "plain vector with the 2i+1 / 2i+2 child layout, sifted up and down explicitly.",
         {"include/boxbox/Leaderboard.hpp", "src/Leaderboard.cpp"},
         {{"peek", "O(1)"}, {"push, pop", "O(log n)"}, {"build from n entries", "O(n)"},
          {"takeTop(k)", "O(n + k log n)"}, {"drainOrdered", "O(n log n)"}},
         "A full ordering is not cheaper than sorting. The heap earns its place on the top of the order: the leader costs O(n) to "
         "heapify and O(1) to read. RaceSimulator::standings is O(D log D) per call.",
         {"command", "sim"}},

        {"prefix", "Prefix sums", "Ranges",
         "Turns a sum over a run of laps into one subtraction. The optimizer's stint-cost table is a prefix sum of the "
         "compound's lap-time penalty by tire age; StopLapIndex is a prefix sum of stops per lap.",
         {"include/boxbox/PitStrategy.hpp", "src/PitStrategy.cpp", "include/boxbox/PitAnalytics.hpp", "src/PitAnalytics.cpp",
          "src/TireAnalytics.cpp"},
         {{"build the table", "O(C*M)"}, {"stintCost, stintDelta", "O(1)"},
          {"StopLapIndex build, query", "O(n + N), O(1)"}},
         "The same table is read by the optimizer and by the simulator's check, which is what lets the two totals be compared.",
         {"lab", "tires", "pit"}},

        {"segtree", "Segment tree", "Ranges",
         "Answers the total and the minimum over any range of laps or tire ages, with point updates. Used for a car's laps by "
         "race lap and a compound's lap times by tire age.",
         {"include/boxbox/SegmentTree.hpp", "src/SegmentTree.cpp", "include/boxbox/RaceLapAnalytics.hpp"},
         {{"build", "O(n)"}, {"rangeSum, rangeMin, rangeMinIndex, update", "O(log n)"}, {"valueAt", "O(1)"}, {"storage", "O(n)"}},
         "Iterative and bottom-up: leaves at n .. 2n-1, node i combines 2i and 2i+1. Minimum ties go to the smaller index, compared "
         "exactly with < so a query returns the element a scan would. On append-only data a running total answers sums in O(1); "
         "the tree is here for the minimum.",
         {"drivers", "tires"}},

        {"fenwick", "Fenwick tree", "Ranges",
         "Sums over a range of laps, and counts how many pairs of cars changed places between two laps (an inversion count). "
         "Each car's traffic loss per lap lives in one.",
         {"include/boxbox/FenwickTree.hpp", "src/FenwickTree.cpp", "include/boxbox/RaceLapAnalytics.hpp", "src/RaceLapAnalytics.cpp"},
         {{"build from n values", "O(n)"}, {"add, prefixSum, rangeSum", "O(log n)"}, {"storage", "O(n)"},
          {"countOrderSwaps", "O(D log D)"}},
         "prefixSum(k) adds the nodes k, k minus its lowest set bit, and so on; a range is two prefix sums.",
         {"drivers", "pit"}},

        {"deque", "Monotonic deque", "Windows",
         "The quickest and slowest lap in the last W laps, for every lap of a race, in one pass.",
         {"include/boxbox/MonotonicDeque.hpp", "src/MonotonicDeque.cpp", "src/DriverAnalytics.cpp"},
         {{"construct", "O(W)"}, {"push", "O(1) amortised"}, {"extreme", "O(1)"}, {"storage", "O(W)"},
          {"trailingWindows", "O(n)"}},
         "A new value removes every worse value from the back; the value that slides out leaves from the front. Equal values both "
         "stay, so the front is the earliest of equal extremes. Rescanning every window would cost O(n*W).",
         {"tires"}},

        {"eventqueue", "Priority queue of events", "Heaps",
         "Holds every event the simulator emits and releases them in time order, then type, car number and insertion order.",
         {"include/boxbox/EventPriorityQueue.hpp", "src/EventPriorityQueue.cpp", "include/boxbox/EventLog.hpp",
          "include/boxbox/RaceState.hpp", "src/RaceState.cpp"},
         {{"push, pop", "O(log E)"}, {"peek", "O(1)"}, {"processUntil(t)", "O(k log E)"},
          {"validate the log", "O(n) expected"}},
         "Events are compared exactly: a tolerance is not transitive, so it is not a valid sort key. The released log is replayed "
         "through a per-car state machine and any illegal sequence fails the export.",
         {"pit", "sim"}},

        {"bsearch", "Binary search", "Search",
         "The first tire age whose loss against a fresh tire exceeds a limit, and the longest stint whose average lap fits a limit.",
         {"include/boxbox/TireAnalytics.hpp", "src/TireAnalytics.cpp"},
         {{"threshold query, lap times never fall", "O(log M)"}, {"threshold query otherwise", "O(M) scan"},
          {"compareCompounds", "O(min(Ma, Mb)) full scan"}},
         "Bisection is used only when the stored lap times never fall, which makes the searched property monotonic. Otherwise the "
         "query scans, and the answer is the same either way.",
         {"tires"}},

        {"topk", "Bounded heap for the top K", "Heaps",
         "The K fastest or slowest laps of the race, of one car, or on one compound.",
         {"include/boxbox/DriverAnalytics.hpp", "src/DriverAnalytics.cpp"},
         {{"selectLaps", "O(R log K) time, O(K) space"}},
         "The heap holds at most K laps with the worst of them at the root, so a new lap only has to beat the root. Sorting every "
         "lap would cost O(R log R).",
         {"drivers"}},

        {"greedy", "Greedy interval stabbing", "Optimisation",
         "The fewest laps that put a pit call inside every stop window.",
         {"include/boxbox/PitAnalytics.hpp", "src/PitAnalytics.cpp"},
         {{"minimumStopLaps", "O(W log W)"}, {"checkStintCoverage", "O(S log S)"}},
         "Sort the windows by latest lap and take a window's latest lap whenever no chosen lap is inside it. Optimal by an exchange "
         "argument, and checked in the tests against an exhaustive search over every subset of laps.",
         {"pit"}},

        {"overtake", "Overtaking rule", "Rules",
         "A deterministic pass-or-hold rule. A car that catches the one ahead is held minGap behind it unless it is passMargin "
         "a lap quicker. It changes when a car crosses the line, never its pace.",
         {"include/boxbox/RaceSimulator.hpp", "src/RaceSimulator.cpp"},
         {{"advanceLap, rule off", "O(D)"}, {"advanceLap, rule on", "O(D log D)"},
          {"with an event sink", "plus O(D log E)"}},
         "Comparisons go through definitelyLess, so a car exactly minGap behind is clear and a pace advantage of exactly passMargin "
         "is enough to pass. A lone car is never obstructed, so the optimizer and the simulator still agree with the rule on.",
         {"sim", "drivers", "command"}},

        {"hashmap", "Hash maps", "Ranges",
         "Take a car number to that car's summary and stops. They are only ever looked up, never iterated.",
         {"include/boxbox/DriverAnalytics.hpp", "src/DriverAnalytics.cpp", "src/PitAnalytics.cpp"},
         {{"DriverAnalytics build", "O(D + R) average"}, {"lookup", "O(1) average, O(D) worst case"},
          {"PitAnalytics::allSummaries", "O(n) expected"}},
         "std::unordered_map is O(1) on average and O(n) in the worst case, and the code says average wherever one is used. Because "
         "no map is iterated, output order never depends on the hash.",
         {"drivers", "pit"}},
    };
    return catalog;
}

}  // namespace boxbox
