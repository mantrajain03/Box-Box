# Complexity Audit

Plan sec. 8.3 item 14 and NFR-2: every complexity claim in the code states what the code
actually costs. Reviewed 2026-11-10 by Sarthak (with Claude), by reading each claim next to
the implementation it describes. This is a reading audit, not a measurement: the benchmarks
(`make bench`) show scaling for the structures they cover, and a timing is never evidence of
a bound on its own.

N laps, C compounds, M longest stint, S stints, D cars, E events, R lap records, K plans or
laps requested, W window, P sweep points.

## Result

Every `O(...)` annotation in `include/boxbox/` and `src/` was read. Four were wrong or
incomplete and were fixed in the comment (three in the first pass, one in the second:
`trafficAfterStops`, below); none was a code change.

| Where | Claim | Verdict |
|---|---|---|
| `PitAnalytics::totalPitLoss` | O(stops) | **False, fixed.** It builds the driver's summary first, so it is O(n) over the log. |
| `RaceSimulator::advanceLap` | O(D) per lap with overtaking off | **Incomplete, fixed.** With an event sink each car pushes up to 5 events at O(log E), so a lap costs O(D log E) more. |
| `kBestPlans` (new) | O(N·K) memory | **Incomplete, fixed before release.** One lap's candidate buffer, O(C·M·K), is rebuilt every lap and is the larger term when K is large. |

## Checked and accurate

| Module | Claims |
|---|---|
| `PitStrategy` | Prefix table O(C·M) time and space; `stintCost` / `stintDelta` O(1); `optimize` O(N·C·M) time, O(N) extra space (the `dp` and `choice` arrays; the backtracked plan is also O(N)); `evaluatePlan` and `singleCompoundPlan` O(S). |
| `Leaderboard` | `peek` O(1); `push`, `pop` O(log n); heapify constructor O(n); `takeTop(k)` O(n + k log n); `drainOrdered` O(n log n), and the code states a full ordering is no cheaper than a sort. |
| `RaceSimulator` | `standings` O(D log D) per call (O(N·D log D) if called every lap); `topK` O(D + k log D); `advanceLapWithTraffic` O(D log D); `runToFinish` O(N·D) or O(N·D log D). |
| `Track`, `TireCompound`, `Driver` | O(1) accessors and `lapDelta`; `Driver::finished()` O(1) (an earlier O(S) version was fixed in PRD phase 4). |
| `SegmentTree` | Build O(n); `rangeSum`, `rangeMin`, `rangeMinIndex`, `update` O(log n); `valueAt` O(1); storage O(n). |
| `FenwickTree` | Linear build O(n), against O(n log n) for n adds; `add`, `prefixSum`, `rangeSum` O(log n); storage O(n). |
| `TireAnalytics` | Profile build O(M) (the header states that end to end each CLI command is O(M) whatever the per-query figures say); range queries O(log M); threshold queries O(log M) by binary search **only** when the stored lap times never fall, else an O(M) scan; `compareCompounds` O(min(Ma, Mb)). |
| `RaceLapAnalytics` | `LapTimeIndex` construct O(N), `recordLap` O(log N), queries O(log N); `RaceLapRecorder::observe` O(D log N + D log D) per lap (now includes one Fenwick add per car); `trafficLoss` O(log N); `fieldTrafficLoss` O(D log N); `countOrderSwaps` O(D log D) in every case (sorted start positions and binary search, no hash map). |
| `EventPriorityQueue` | `push`, `pop` O(log E); `peek` O(1); `processUntil` O(k log E). |
| `EventLog`, `PitAnalytics` | `record` O(1) amortised; filters O(n); `summaryFor` O(n); `allSummaries` O(n) expected with a hash map (worst case O(n·D) under pathological hashing, stated). |
| `RaceState` | `apply` O(1); `validateEventLog` O(n) expected. |
| `DriverAnalytics` | Construct O(D + R) average (worst case O(R·D), stated); lookups O(1) average; `selectLaps` O(R log K) time, O(K) space; `trailingWindows` O(n); `rankDrivers` O(D log D). |
| `MonotonicDeque` | Construct O(W); `push` O(1) amortised; `extreme` O(1); storage O(W). |
| `StrategyAnalytics` | See `docs/STRATEGY_MODULE.md`: sweeps O(P·N·C·M); `bestForStopCount` O(S·N·C·M); `compoundOnlyBest` O(C·N·M); `cleanAirVsTraffic` O(N·D log D) per race. |

## Added 2026-11-10 (second pass)

New code in this pass, each claim read against the implementation:

| Where | Claim | Verdict |
|---|---|---|
| `RaceSimulator::emitPositionChanges` | one `standings()` call per lap, O(D log D), only with a sink | True: it returns at once with no sink. |
| `PitAnalytics::stintHistogram` | O(n + B) | True: one pass for the extremes, one to fill. B = (longest − shortest)/width + 1. |
| `PitAnalytics::buildStopIndex`, `StopLapIndex::stopsInLaps` | O(n + N) build, O(1) query | True: one pass over the log, one prefix pass, two array reads. |
| `PitAnalytics::windowSummary`, `compareStopTiming`, `fieldPitLoss` | O(n) | True: each scans the log (or builds one summary). |
| `PitAnalytics::checkStintCoverage`, `minimumStopLaps` | O(S log S), O(W log W) | True: a sort dominates; the walk after it is linear. The greedy is optimal, and is checked against an exhaustive search over every subset of laps. |
| `PitAnalytics::trafficAfterStops` | O(R log R) | **False as first written, fixed.** The header said O(R + s log s); the per-car sort of laps is O(R log R) and dominates, with one binary search per stop. |
| `modelledSectorTimes`, `sectorSummaries`, `fieldBestSectors`, `compareSectors` | O(S), O(R·S) | True. `sectorDistribution` is O(R + bins): two passes over R records. |
| `compareFirstStopLaps` | two `optimize()` calls, O(N·C·M) | True: each continuation is an optimize on at most N laps. |
| `StrategyAnalytics::kBestPlans` memory | O(N·K + C·M·K) | True (corrected earlier in this audit). |

## Evidence from the bug hunt

| Check | Result |
|---|---|
| `-Wall -Wextra` (the project's flags) on the default build, `make test` and `make bench` | 0 warnings |
| `src/` and `cli/` under `-Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast -Wuseless-cast -Wnull-dereference -Wlogical-op -Wduplicated-cond -Wduplicated-branches -Wcast-qual -Wformat=2 -Wnon-virtual-dtor -Woverloaded-virtual -Wdouble-promotion -Wfloat-conversion -Wundef -Wredundant-decls -Wmissing-declarations` | 0 warnings in 23 files |
| tests and benchmarks under the same set (shorter list) | only `-Wuseless-cast` on `static_cast<std::size_t>` of a `uint64_t` in test helpers: harmless here and needed on 32-bit targets |
| full suite at `-O0 -D_GLIBCXX_DEBUG -D_GLIBCXX_ASSERTIONS -ftrapv` (bounds-checked containers, signed-overflow traps) | passes |
| full suite at `-O3` | passes |
| `cppcheck --enable=all --inconclusive --check-level=exhaustive` on `src/`, `cli/`, `include/` | no error or warning; one `performance` note (`Leaderboard::push` takes its 24-byte entry by value: left, it is a public signature); the rest are style suggestions (`useStlAlgorithm`, a member that could be static) |
| line coverage of `src/` and `cli/` (gcov, full suite) | see below |
| fault injection on the code added in this pass (14 one-line mutants in throwaway copies) | see below |

### Fault injection on the second pass

Fourteen one-line mutants, each built in a throwaway copy and run against the full suite. All
fourteen were caught (by failed checks, or by aborting the run):

| Mutant | Outcome |
|---|---|
| greedy pit-call test uses `<=` for `<` | caught (`test_events.cpp` greedy brute force) |
| stint coverage misses an overlap | 1 failed check |
| stop-index prefix off by one | 8 failed |
| stint histogram bin index wrong | 7 failed |
| traffic after a stop counts an extra lap | 15 failed |
| pit window upper bound exclusive | 12 failed |
| sector split remainder wrong (parts no longer add up) | aborted (record validation) |
| field best sector tie-break reversed | 97 failed |
| position baseline never advances | 13 failed |
| overtake attempt never emitted | 2 failed |
| state machine rejects a position change | 13 failed |
| undercut earlier plan built with the later lap | aborted |
| plan range gap allowed in the CLI parser | 3 failed |
| cumulative degradation drops a term | 8 failed |

### Independent code review

Two read-only reviewers read the new code. Real defects they found, all fixed and each now
covered by a regression test in `test_edges.cpp`:

* a one-lap stint lost its compound in the pit summary (the `PitEntry` that closes it is logged
  before that lap's `LapCompleted`), so every later stint in `--pit-report` carried the wrong
  compound — it showed under zero pit loss, where every stint is one lap;
* `RaceSimulator::setEventSink` and `addDriver` left a stale position baseline, so the next
  `PositionChange` could carry a wrong `oldPosition` or read past the baseline;
* `fieldBestSectors` could read out of bounds for hand-built records whose cars had different
  sector counts (now refused when the analytics is built);
* a last sector of base time 0 could leave a remainder a few 1e-15 below zero, which the
  record check then rejected;
* `printPitReport` threw on a stop logged past the race distance or on a negative call window,
  and left its number formatting on the caller's stream;
* integer overflow for an extreme histogram bin width or `lapsAfter`, and a long plan
  accumulator in the CLI parser;
* output printed before a failure in `strategy alternatives` and `strategy stops`; stale
  comments about sector analytics and the undercut sign convention.

### Coverage

Every reachable line of the library and the CLI handlers is executed by the suite: 99.4% of
3,558 lines (gcov, full suite; `JsonWriter` and `ExportCommand`, added in parallel by someone
else, are outside this audit). The 20 lines that are not, and why none can be reached through
the public interface:

| Line | Why it stays |
|---|---|
| `Driver::completeLap` return on a finished car | private; the simulator never steps a finished car (`advanceLap` stops first) |
| `LapRecorder`/`DriverAnalytics` fall-through returns (`rankValue` default, `sectorDistribution` empty count) | a switch over a closed enum; a count that `sectorCountOf` already guarantees is non-zero |
| `PitStrategy::optimize` unreachable-state `continue`s and `return` | every lap is reachable by repeating one-lap stints; they guard a future constraint, as the header says |
| `RaceSimulator::advanceLap` skipping a finished car | all cars run the same number of laps, so none finishes early |
| `evaluateAgainst` "evaluatePlan rejected a plan validatePlan accepted" | the two checks are the same conditions; kept so a future change cannot silently disagree |
| `runSweep` "no feasible plan" | `optimize()` is always feasible for a constructed `PitStrategy` |
| `kBestPlans` infeasible optimum, `bestForStopCount` positive stops on a 0-lap race | same reason; the latter is excluded by the argument check |

## Caveats the audit found worth stating

* **Hash maps.** `std::unordered_map` is O(1) on average and O(n) in the worst case. The
  code and docs say "average" wherever a map is used; none of the maps is iterated, so
  output order never depends on the hash.
* **Amortised bounds.** `push` on the monotonic deque, `EventLog::record` and `Track::addSector`
  are amortised O(1), not worst-case O(1), and are labelled so.
* **Floating-point comparisons.** The plan's checklist asks for tolerance-based comparison
  everywhere. Three places compare exactly on purpose and say why: the segment tree and
  `selectLaps` (so a query returns the very element a scan would), and `eventComesBefore` and
  the K-best sort key (a tolerance is not transitive, so it is not a valid sort key).
  Everywhere a computed total is compared, `kTimeEpsilon` or `kClosureTolerance` is used.

## Not verified

* No claim was checked by instrumenting operation counts; the reading above is the evidence.
* The bodies of `Leaderboard`, `SegmentTree`, `FenwickTree`, `TireAnalytics` and
  `DriverAnalytics` were read in the earlier module audit and were not re-derived line by
  line in this pass; for those the verdict above rests on that review plus the comment
  matching the structure (heap, tree, ring buffer) they implement.
* `make bench` shows real scaling only for the structures it times. The DP's O(N·C·M) shape
  is visible there (N = 50, 300, 2,000 and C = 3, 9), but timings are machine dependent.
