# Complexity Audit

Plan sec. 8.3 item 14 and NFR-2: every complexity claim in the code states what the code
actually costs. Reviewed 2026-11-10 by Sarthak (with Claude), by reading each claim next to
the implementation it describes. This is a reading audit, not a measurement: the benchmarks
(`make bench`) show scaling for the structures they cover, and a timing is never evidence of
a bound on its own.

N laps, C compounds, M longest stint, S stints, D cars, E events, R lap records, K plans or
laps requested, W window, P sweep points.

## Result

Every `O(...)` annotation in `include/boxbox/` and `src/` was read. Three were wrong or
incomplete and were fixed in the comment; none was a code change.

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
