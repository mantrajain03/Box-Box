# Tire Intelligence and Range-Query Module

Owner: Maulshree. Plan sections 4.2 and 8.1.

A hand-written segment tree and Fenwick tree, tire degradation analytics built
on the canonical lap-time model, range queries over simulated race laps,
position-swap counting, and the `tires` CLI command. Standard library only,
flat `boxbox` namespace, like the rest of the core.

## Files

| File | What it is |
|---|---|
| `include/boxbox/SegmentTree.hpp`, `src/SegmentTree.cpp` | Range sum and range minimum with point updates |
| `include/boxbox/FenwickTree.hpp`, `src/FenwickTree.cpp` | Prefix and range sums with point updates, O(N) build |
| `include/boxbox/TireAnalytics.hpp`, `src/TireAnalytics.cpp` | Degradation profiles, tire-age range analyzer, threshold and crossover queries |
| `include/boxbox/RaceLapAnalytics.hpp`, `src/RaceLapAnalytics.cpp` | Lap-time index over simulated laps, race recorder, position-swap count |
| `include/boxbox/LapTime.hpp` | `canonicalLapTime`, the single lap-time sum shared with `RaceSimulator` |
| `cli/TireCommands.hpp`, `cli/TireCommands.cpp` | `boxbox tires ...`: parsing and printing only |
| `cli/RaceLapReport.hpp`, `cli/RaceLapReport.cpp` | The `--race-laps` report: printing only |
| `tests/test_tire_module.cpp`, `tests/TireModuleTests.hpp` | Checks, run by `make test` |
| `benchmarks/bench_range_queries.cpp` | Benchmarks, run by `make bench` |

The sources sit directly in `src/` and the headers in `include/boxbox/`, so
the Makefile's existing `src/*.cpp`, `cli/*.cpp` and `tests/*.cpp` wildcards
build them unchanged.

Edits to shared files, all additive:

* `Makefile`: a separate `bench` target only. NFR-6 makes the Makefile the one
  way to build anything, so the benchmark needs its own target; `make` and
  `make test` are untouched.
* `tests/test_main.cpp`: calls `runTireModuleTests()` and adds its counts to
  the total.
* `cli/main.cpp`: passes everything after the word `tires` to the handler,
  and adds the opt-in `--race-laps FROM:TO` flag (parse it, let a
  `RaceLapRecorder` watch the race, call the report), plus help text for both.
  No algorithm logic. Without `tires` or `--race-laps`, output is unchanged
  byte for byte apart from the help text, and with `--race-laps` the only
  difference is the added report.

## Sources of truth

Nothing in this module re-implements the race model.

| Value | Where it comes from |
|---|---|
| Lap time at a tire age | `canonicalLapTime` in `include/boxbox/LapTime.hpp` (`Track::baseLapTime() + TireCompound::lapDelta(age)`), the one function both `RaceSimulator::lapTimeFor` and `DegradationProfile` call |
| Stint cost | `PitStrategy::stintCost`, read from the optimizer's prefix table |
| Cliff rate | `TireCompound::cliffDegradationPerLap()`, read directly |
| Simulated lap time | `Driver::lastLapTime()`, read from outside `RaceSimulator` |
| Running order | `RaceSimulator::standings()` |
| Tolerances | `kTimeEpsilon` and `definitelyLess` from `Common.hpp` |

The one place this module sums lap times itself is
`DegradationProfile::fromLapTimes`, for recorded data that no compound
describes and so the optimizer has no table for.

## Using it

```
bin/boxbox tires help
bin/boxbox tires profile --compound soft
bin/boxbox tires range --compound medium --from 20 --to 30
bin/boxbox tires range --compound medium --from 20 --to 30 --override 25=89.9
bin/boxbox tires threshold --compound hard --max-average 91.0
bin/boxbox tires threshold --compound soft --max-degradation 1.5
bin/boxbox tires threshold --compound soft --slower-than medium
bin/boxbox --compound X:0:0.2:10:5:0.5 tires profile
```

Race options (`--compound`, `--sector`, ...) go before the word `tires`.
Compound names ignore case. `--override AGE=SECONDS` replaces one age's lap
time as a what-if and may be repeated. Exit status is 0 on success and 1 on
bad usage, as for the rest of the CLI.

For compound sensitivity sweeps (Sarthak's module), vary a `TireCompound` and
build a profile directly. Everything his report needs is already exposed:

```cpp
const boxbox::DegradationProfile a = boxbox::DegradationProfile::fromCompound(track, variant);
const boxbox::DegradationProfile b = boxbox::DegradationProfile::fromCompound(track, reference);

a.averageLapTime()[k];                               // mean lap of a k-lap stint
boxbox::longestStintUnderAverage(a, 91.0).value;     // longest stint averaging <= 91.0 s
a.cliffActivationAge();  a.cliffDegradationPerLap(); // cliff, read from TireCompound

const boxbox::CompoundComparison cmp = boxbox::compareCompounds(a, b);
cmp.firstSlowerAge;  // first age a is slower than b (empty = never)
cmp.switchAges;      // every age the faster compound changes
```

`fromStrategy(strategy, index)` is kept as an adapter for callers that already
hold a `PitStrategy`. `fromCompound` builds a one-compound `PitStrategy` and
calls it, so both give bit-identical profiles from the same code path.

The race lap analytics have one opt-in CLI flag, agreed with Mantra. It
implies `--simulate`, and the report comes after the final classification:

```
bin/boxbox --laps 50 --rivals --overtake --race-laps 30:50
```

```
Race laps 30 - 50   lap times include traffic, not pit loss
     #  driver             fastest lap   on lap    window total
     1  OPTIMAL                 89.400       35        1901.120
     2  ALL-SOFT                89.400       35        1901.120
     3  ALL-MEDIUM              90.220       30        1906.170
     4  ALL-HARD                90.690       30        1911.840

  Position swaps during laps 30 - 50: 2 pairs of cars changed places
```

Swaps compare the running order at the end of lap FROM - 1 (the grid when
FROM is 1) with the order at the end of lap TO. Bad ranges (`0:5`, `9:5`, a
lap past the race distance) are rejected with exit status 1 before anything
is printed. The report printer is `cli/RaceLapReport.cpp`; it only prints.

The same analytics from code:

```cpp
boxbox::RaceLapRecorder recorder(simulator);
while (simulator.advanceLap()) {
    recorder.observe(simulator);
}
const auto best = recorder.carLaps(0).fastestLap(20, 35);  // {lap, seconds}
const std::int64_t swaps = recorder.swapsBetween(10, 20);  // pairs that changed places
```

## Conventions

**Tire ages and stint lengths.** `maxStintLaps` is inclusive: it is the
longest legal stint, so stints of 1 to `maxStintLaps` laps are all legal. Tire
age starts at 0, so a k-lap stint runs ages 0 to k - 1, and the legal ages are
0 to `maxStintLaps - 1`. In a profile:

| Array | Entries | Index |
|---|---|---|
| `lapTime()`, `incrementalDegradation()`, `degradationVsFresh()` | `maxStint()` | tire age, 0 .. maxStint - 1 |
| `stintCost()`, `averageLapTime()` | `maxStint() + 1` | stint length k, 0 .. maxStint; entry 0 is the empty stint |

The last lap of a k-lap stint is `lapTime()[k - 1]`. For example, the default
SOFT compound has `maxStintLaps` 22: stints of 1 to 22 laps, run at ages 0 to 21.

**Comparing compounds.** `compareCompounds(a, b)` looks only at ages legal on
both compounds (0 to `agesCompared - 1`). At each age, A is slower when its lap
is more than `kTimeEpsilon` slower than B's, faster when B's is more than
`kTimeEpsilon` slower than A's, and otherwise tied.

* `firstSlowerAge` is the first age at which A is slower, even if A is faster
  later. It is empty if A is never slower.
* `switchAges` lists every age at which the faster compound changes. Ties do
  not count on their own. The first non-tied age only sets who leads. A tied
  age keeps the previous leader. A switch is recorded at the first non-tied
  age whose leader differs from the last non-tied age before it. So
  slower, tie, tie, faster is one switch, at the "faster" age, and
  slower, tie, slower is no switch.
* An empty `switchAges` means the gap never changes sign.

**Other conventions.**

* Library ranges over tire ages are half-open, `[begin, end)`. `rangeSum(b, b)`
  is 0; `rangeMin` on an empty range throws, because there is no answer.
* The `tires` CLI takes inclusive ages (`--from 3 --to 10`) and converts them.
* Race laps are numbered from 1, and lap windows (`fastestLap(20, 35)`) include
  both ends. Lap 0 in `runningOrder(0)` is the grid.
* A bad index throws `std::out_of_range`. A value that is not finite, an empty
  range where no answer exists, or misuse (laps out of order, a missed
  `observe()`) throws `std::invalid_argument`. A rejected call changes nothing.
* Minimum ties go to the smaller index: the younger tire age, or the lower lap
  number. Compared with plain `<`, no tolerance, so results match a scan.
* A cliff counts as active only when its rate is above zero.

## Complexity

N = values or race laps; M = a compound's maxStintLaps; D = cars.

| Operation | Time |
|---|---|
| SegmentTree build | O(N) |
| SegmentTree rangeSum, rangeMin, rangeMinIndex, update | O(log N) |
| FenwickTree build from values | O(N) |
| FenwickTree add, prefixSum, rangeSum | O(log N) |
| `DegradationProfile::fromCompound`, `fromStrategy`, `fromLapTimes` | O(M) |
| `TireRangeAnalyzer` construct and reset | O(M) |
| `TireRangeAnalyzer` queries and `overrideLapTime` | O(log M) |
| `firstAgeDegradationExceeds`, `longestStintUnderAverage` | O(log M) when lap times never fall, else O(M) |
| `stintLengthsWithin` | O(M) |
| `compareCompounds` | O(min(Ma, Mb)) time, O(S) space for S switches |
| `LapTimeIndex` construct | O(N) |
| `LapTimeIndex` recordLap, totalTime, fastestLap | O(log N) |
| `RaceLapRecorder` construct | O(D·N + D log D) |
| `RaceLapRecorder::observe` | O(D log N + D log D) per lap, O(N·D log D) per race |
| `countOrderSwaps`, `swapsBetween` | O(D log D) in every case (sorted lookup, no hash map) |

Storage: the segment tree keeps 2N sums plus 2N (value, index) pairs, about
48 bytes per value. The Fenwick tree keeps only its N + 1 nodes, about 8 bytes
per value: it offers only the four operations the plan lists (point update,
prefix sum, range sum, build), so it needs no copy of the values. The recorder
keeps one lap-time index per car and one running order per lap, O(D·N).

**What a `tires` command actually costs.** Each command builds a fresh
profile, O(M), so each is O(M) end to end whatever the per-query figures say.
Tire profiles are small (45 ages for the default HARD compound), and at that
size a plain scan beats either tree. The trees earn their place through
checked range queries and overrides on profiles, and through speed on
race-length data.

**`observe` is dominated by the standings.** Recording the laps is O(D log N),
but taking the running order costs O(D log D) per lap, as
`RaceSimulator::standings` documents.

## Design decisions

**Where the segment tree is used, and why.** Two places.

* Simulated race laps (`LapTimeIndex`). These go up and down with pit stops
  and traffic, so "fastest lap between laps 20 and 35" is a real question, and
  a range minimum needs a tree. Laps arrive one at a time, so each is written
  with a point update as the race runs. Range sums come from the same tree;
  on append-only laps they could also come from running totals in O(1), so
  the tree is there for the minimum. Agreed with Mantra.
* Tire profiles (`TireRangeAnalyzer`). The plan (section 4.2) lists range
  queries over tire ages as required use cases. Lap times there only rise with
  age, so on an unmodified profile the fastest age in a range is simply its
  first age; range minimum only has something to find after an `--override`
  what-if, which can make the curve dip; recalculating after that change is
  exactly the update a segment tree is for. Mantra agreed (2026-11-08) to keep
  these queries so the plan is unchanged, on two conditions, both tested:
  overrides are what-if only and never reach `PitStrategy` or
  `RaceSimulator`, and with no override every answer matches the prefix table
  and the first age's `lapDelta`.

**Where the Fenwick tree is used, and why.** `countOrderSwaps` counts how many
pairs of cars swapped places between two laps: Mantra's suggestion, and the
one real use found for it. That is an inversion count. Each car is numbered
by its place in the first order, the second order is walked front to back,
and for each car the tree answers how many cars already walked started ahead
of it: one prefix sum and one point update per car. Start positions are found
by binary search in a list sorted by car number, not a hash map, so the
O(D log D) bound holds in every case (a hash map is O(1) only on average, and
the plan asks for that worst case to be stated). That is against O(D²)
for checking every pair.

The earlier `LapStatAccumulator` (running totals by lap) was dropped in
agreement with Mantra, because lap data is only ever appended and running
totals already give any range sum in O(1). The plan (sec. 8.1, item 8) still
asks for cumulative lap-time or traffic-loss statistics on a Fenwick tree, so
`RaceLapRecorder::trafficLoss` and `fieldTrafficLoss` (added 2026-11-10) keep
each car's per-lap traffic loss in a `FenwickTree` and answer any lap window
in O(log N). On append-only data a prefix array would answer in O(1); the
tree is kept to satisfy the plan and because it also takes a correction to an
earlier lap without an O(N) rebuild. This reverses part of the 2026-11-06
agreement with Mantra and is listed under "Open questions".

**One tree per tire profile, not two.** A degradation value is that lap's
time minus the original fresh lap, so a range's total degradation is
`lapTimeSum(b, e) - (e - b) * freshLap`.

**When binary search is allowed.** Both binary searches are gated on
`lapTimeNonDecreasing()`, which is checked on the stored numbers while the
profile is built and cannot be set by hand.

* Degradation: if lap times never fall, `lapTime[a] - lapTime[0]` never falls,
  because subtracting the same number keeps the order, rounding included.
* Average stint lap: each new lap is at least as slow as every earlier one, so
  the mean can only rise, and the stints that fit under a threshold are 1 .. K.
  In floating point the raw quotient `stintCost[k] / k` can dip by a few
  1e-14 s, which made an earlier version's binary search disagree with a scan
  on 15% of queries placed at the tolerance edge with zero wear. The profile
  stores the mean as a running maximum: identical in exact arithmetic, never
  falling, so binary search and scan always agree.

When lap times can fall, as with recorded data, both queries scan.

**Why crossover always scans.** The gap between two compounds changes slope at
each one's cliff. For example, a compound 0.5 s slower with no wear and a
cliff after age 10, against one wearing 0.2 s/lap, is slower when new, faster
from age 3 and slower again from age 12. A binary search would be wrong, and
so would reporting only the first crossover: `compareCompounds` gives
`firstSlowerAge = 0` and `switchAges = {3, 12}`.

## Tests

`make test` runs this module's checks as part of the single total the harness
prints (245,538 checks across all modules on 2026-11-10; the harness does not
print a per-module split, so earlier per-module figures are not repeated).
They cover:

* empty, single-element, boundary and invalid inputs, ties, values that are
  not finite, rejected calls leaving the structure unchanged, repeated
  updates, for both trees
* both trees against brute force on 60 seeded arrays each, with interleaved
  updates, plus a 20,000-element array
* a point update changes exactly the ranges that contain it
* the Fenwick linear build against n separate `add()` calls
* a hand-worked profile and every cliff edge case: the boundary (age 3 =
  cliffLap pays nothing, age 4 pays), a 0 s/lap rate, a cliff from age 1, on
  the last legal age, just past it, no cliff, and the default compounds
* 300 seeded compounds (one in five with zero wear), with every lap time,
  stint cost and cliff activation checked against `lapDelta`,
  `PitStrategy::stintCost` and the compound's own cliff rate
* threshold queries against brute-force scans, including at the tolerance edge
  and one rounding step either side for zero and 1e-13 s/lap wear
* recorded laps that fall, including a shape where binary search would be wrong
* `compareCompounds` (agreed with Sarthak): several switches from a cliff, many
  switches, no crossover, identical compounds, ties that do and do not change
  the leader, a tie at the start, gaps just inside and just outside
  `kTimeEpsilon`, crossovers on the first and last age legal on both, and one
  just past the shorter stint that must not be reported
* `compareCompounds` against an independent brute force on 400 seeded pairs,
  a quarter with coarse laps full of ties and a quarter with gaps right at the
  tolerance; switches are the same either way round
* `fromCompound` bit-identical to `fromStrategy` on 300 seeded compounds, and
  the documented age and stint-length mapping
* `LapTimeIndex`: order of recording, ties going to the lower lap, every
  invalid window, and 60 seeded races against a brute-force scan
* `countOrderSwaps`: hand cases (a fully reversed field of 5 gives 10), every
  invalid input including a foreign car numbered between real ones, the wording
  of the duplicate-car error, and 200 seeded fields with non-sequential car
  numbers against checking every pair, plus symmetry
* `RaceLapRecorder` on 40 seeded races, half with overtaking:
  * the grid is in car-number order
  * every running order matches a sort with `Leaderboard::isAhead`
  * recorded laps plus pit stops add up to each car's race time within
    `kClosureTolerance`
  * without traffic, every lap equals the model worked out from the plan
  * swap counts match brute force, and cars really do swap places
  * Fenwick-backed traffic loss over random lap windows matches each lap's
    recorded time minus the model lap, per car and for the whole field, and is
    exactly zero with overtaking off
* misuse: a recorder created after the start, a missed `observe()`, bad
  indices, and a repeated `observe()` with no lap run (a no-op)
* agreed with Mantra: with no override, `TireRangeAnalyzer` range sums equal
  differences of `PitStrategy::stintCost`, and the minimum of any age range is
  its first age, equal to `lapDelta` there (300 seeded compounds)
* agreed with Mantra: overriding every age of every compound leaves
  `PitStrategy::stintCost`, the optimal plan and a simulated race exactly
  unchanged, and the profile itself untouched
* repeated runs give identical results

Inputs come from the same seeded LCG as `test_main.cpp`.

Fault injection, each mutant built clean in a throwaway copy. These results
were recorded by the module's author; nothing in the repository reproduces
them, and they were not re-run in the 2026-11-10 review, so treat the counts
as the author's report rather than a repeatable check:

| Mutant | Failures |
|---|---|
| Segment tree minimum tie-break reversed | 3,802 |
| Fenwick linear build skips the last parent | 200 |
| Lower-bound binary search steps past the answer | 16,421 |
| Running maximum removed from stored averages | 3,813 |
| Cliff activation age off by one | 97 |
| Cliff reported active with a 0 s/lap rate | 23 |
| Stint cost summed locally instead of read from PitStrategy | 2,589 |
| Swap count counts cars ahead instead of behind | 388 |
| Car added to the swap-count tree before it is counted | 405 |
| Recorder stores race time instead of lap time | 1,389 |
| Fastest-lap number off by one | 8,023 |
| Comparison: a tie resets the leader | 145 |
| Comparison: the first leader counted as a switch | 410 |
| Comparison: reports the last slower age, not the first | 294 |
| Comparison: tolerance dropped from the slower test | 157 |
| Comparison: tolerance dropped from the faster test | 145 |
| `fromCompound` takes its own path instead of `fromStrategy` | 771 |
| Analyzer tree built from degradation instead of lap times | 22,140 |
| Swap-count lookup accepts a car that is not in the first order | 2 |
| Duplicate car in the first order not reported as such | 1 |
| Swap-count lookup comparator off by one | `make test` aborts on an unexpected error |

One further mutant, `prefixSum(pos)` in place of `prefixSum(pos + 1)` in the
swap count, passed every check. That is correct, not a gap: the car's own slot
is still empty when it is counted, so the two are the same number.

The duplicate-car check in `countOrderSwaps` is kept for its message. Without
it a duplicate in the first order is still rejected, because with equal
lengths a car must then be missing or repeated; that is why its mutant fails
only the test that reads the message.

## Benchmarks

`make bench`, -O2. Each time is the median of 5 runs, and correctness is
checked before any timing. Numbers vary by machine. The table below was
measured on Linux with g++ 13; run `make bench` for figures on your own
machine, including the 1,000 and 10,000 sizes the table omits and the later
cases (Fenwick linear build against n adds, segment tree update + minimum
against a plain array, and threshold binary search against a scan).

| Operation | Size | Queries | Tree | Brute force |
|---|---|---|---|---|
| Segment tree range sum | 45 | 5,000 | 0.19 ms | 0.08 ms |
| Segment tree range min | 45 | 5,000 | 0.27 ms | 0.12 ms |
| Fenwick update + sum (50/50) | 45 | 5,000 | 0.10 ms | 0.04 ms |
| Segment tree range sum | 100,000 | 50,000 | 7.1 ms | 2,162 ms |
| Segment tree range min | 100,000 | 50,000 | 14.0 ms | 2,657 ms |
| Fenwick update + sum (50/50) | 100,000 | 50,000 | 3.1 ms | 1,193 ms |
| Position swaps (Fenwick) | 20 cars | 1 | 0.001 ms | under 0.001 ms |
| Position swaps (Fenwick) | 1,000 cars | 1 | 0.054 ms | 0.165 ms |
| Position swaps (Fenwick) | 20,000 cars | 1 | 1.4 ms | 96 ms |

At real sizes (a 45-age tire profile, a 20-car field) a plain scan is as fast
or faster. The structures pull ahead at large sizes (the 100,000-value rows
above); where exactly the crossover falls is for `make bench` to show on the
machine in hand, since the table has no 1,000 or 10,000 row.

## Exception to NFR-5

`make bench` reads `std::chrono::steady_clock` to time runs. Sarthak approved
this on 2026-11-06 as an explicit exception to NFR-5, on these conditions:

* only the benchmark binary reads the clock; the core, CLI and tests never do
* the clock times runs and nothing else: no seeding, no clock-based
  randomness, no code path chosen by time
* benchmarks stay separate from tests, and a timing is never a pass or fail check

`PRD.md` is not edited here. Mantra maintains the PRD and README, and will add
the benchmark exception to NFR-5 himself.

## Status

* **Windows: validated 2026-11-10** with `mingw32-make` and g++ 15.2.0
  (MinGW-W64 x86_64-ucrt-posix-seh, target `x86_64-w64-mingw32`): a clean
  `mingw32-make` with zero warnings, `mingw32-make test` (245,538 checks, 0
  failed) and `mingw32-make bench` (all correctness checks passed). This is a
  Windows MinGW-W64 toolchain, not an MSYS2 UCRT64 shell; nobody has run it
  under MSYS2 itself.
* **Tire-profile and comparison interface: agreed with Sarthak** on
  2026-11-06, and implemented as agreed.
* **Shared-file edits** are merged on `main`.

## Open questions

* Whether `RaceLapRecorder::trafficLoss` / `fieldTrafficLoss` (Fenwick-backed,
  added 2026-11-10 to meet plan sec. 8.1 item 8) should stay, given that
  Mantra's 2026-11-06 reply preferred no Fenwick tree for lap totals. They are
  additive and nothing else depends on them.

## Answered

* **Pratyaksh (2026-11-06).** Events need no running totals from this module:
  PitAnalytics computes stop counts, pit loss and stint lengths from the
  EventLog after the race. What events need is the compound (old and new) and
  tire age at the instant a CompoundChange or PitExit fires, which is live
  driver state read from `Driver`.
* **Sarthak (2026-11-06).**
  1. `make bench` may use `steady_clock`; see "Exception to NFR-5".
  2. He will use the profiles for compound-parameter sensitivity, built from
     the existing `Track`, `TireCompound` and `PitStrategy` without duplicating
     the formula. The report must include the crossover age and whether a
     crossover exists inside the legal stint range, without assuming the gap is
     monotonic across a cliff. The interface is to be agreed with him first.
  3. Flat `boxbox` namespace, as in the rest of the codebase. Done.
  4. Windows validation is needed from someone on MSYS2 UCRT64, with full
     output, before it is marked complete.
* **Mantra (2026-11-06).**
  1. No Fenwick tree for lap totals: lap data is append-only and `Driver`
     already keeps running totals, so any range sum is O(1). If one is used
     later it should take plain numbers, like `Leaderboard`. A real use:
     counting position swaps between two laps. Done as `countOrderSwaps`, and
     `LapStatAccumulator` removed.
  2. Segment tree over simulated lap times, fed from `Driver::lastLapTime()`,
     outside `RaceSimulator`, plain `<`, lower lap wins ties, tested against
     brute force. Done as `LapTimeIndex` and `RaceLapRecorder`.
  3. He will review the shared-file edits once sent. The Makefile should need
     no changes, `main.cpp` stays free of algorithm logic with unchanged
     output, tests are checked against brute force with zero warnings, and the
     PRD and README are his. The files now sit where the existing Makefile
     finds them; the `bench` target is the one remaining change.
* **Sarthak, interface review (2026-11-06).** Approved, with these decisions:
  1. Sensitivity sweeps pass a `TireCompound` directly; `fromStrategy` stays as
     an adapter, and both must use the same lap-time calculation. Done as
     `fromCompound`, which delegates to `fromStrategy`; a test checks they are
     bit-identical.
  2. Keep both `firstSlowerAge` and every switch age. Done.
  3. Tie handling with `kTimeEpsilon` is right; ties do not count as switches
     on their own, and transitions through tied ages are documented. Done; see
     "Comparing compounds" under Conventions.
  4. His report includes average lap time and the longest stint under a
     configurable average threshold, reusing the exposed cliff values.
     Already available as `averageLapTime()` and `longestStintUnderAverage()`.
  * Document whether `maxStintLaps` is inclusive and how ages map to stint
    lengths. Done; see "Tire ages and stint lengths".
  * `firstSlowerAge` means the first slower age even if A is faster later.
    Done, and tested.
  * Tests for several switches, ties, no crossover and the legal-range
    boundaries. Done.
* **Mantra, second reply (2026-11-08).**
  1. Keep both tire-curve and simulated-lap range queries, so the plan does
     not change. Conditions: overrides are what-if only and never reach
     `PitStrategy` or `RaceSimulator`, and with no override the tree must
     match the prefix table and `lapDelta` at the first age. Both are tests.
  2. Push the branch instead of sending a diff, so he reviews exactly what he
     would commit. The `DEPS` change is fine; he will check `main.cpp` only
     parses, calls and prints, with output unchanged unless the new flags are used.
  3. `make bench` as a separate target is right. Conditions, all met: kept out
     of `make` and `make test`, listed in `make help`, recipe text that works
     in cmd and sh, built into `build/` and `bin/` so `make clean` removes it,
     and every clock read inside `benchmarks/`. He will add the benchmark
     exception to NFR-5 in the PRD.
  4. The separate test file is fine: one binary, one total, no framework,
     seeded inputs, brute-force references.
  5. He will run Windows validation after the push (see Status).
  6. One small opt-in flag for the race-lap queries, with recording in this
     module and `main.cpp` only parsing, calling and printing; output unchanged
     without it, and listed in `--help`. Done as `--race-laps FROM:TO`.
