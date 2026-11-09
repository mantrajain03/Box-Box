# Plan Compliance

`PROJECT DEVELOPMENT PLAN.md` against the repository, bullet by bullet, as of 2026-11-10.
Status is **Done**, or **Deviation** where the repository deliberately differs from the plan's
wording (each deviation is listed again at the end with its reason). Nothing is **Missing**.

Evidence names the file that implements the item and the test file that checks it. Test
counts come from `make test`; the checks run by section are in PRD §8.1.

## 4.1 Strategy optimization and strategy lab

| Plan item | Status | Where |
|---|---|---|
| Optimizer, prefix sums, backtracking, ties, plan evaluation, cost table | Done | `PitStrategy` · `test_main.cpp` |
| A. Custom plan: validation, per-stint cost, delta against the optimum, invalid-plan reason | Done | `StrategyAnalytics::validatePlan`, `evaluateCustom` · `test_strategy_analysis.cpp` |
| A. Input as compound + start/end lap or length | Done | `strategy evaluate --plan NAME:LAPS` or `NAME:FROM-TO` · `test_cli.cpp` |
| B. Pit-loss sweep through the real optimizer | Done | `pitLossSweep` · `test_strategy_analysis.cpp` |
| C. Compound parameter sensitivity (pace, wear, max stint, cliff lap, cliff rate) | Done | `compoundSweep` · `test_strategy_analysis.cpp` |
| D. Distinct, ordered, bounded alternatives, best equals the optimum, bound stated | Done | `kBestPlans` (exact, K ≤ 50) · `test_strategy_analysis.cpp` |

## 4.2 Tire intelligence

| Plan item | Status | Where |
|---|---|---|
| A. Lap time by age, incremental and cumulative degradation, cliff point, change after the cliff, max stint, cost per length | Done | `DegradationProfile` (`cumulativeDegradation`, `cliffStepSeconds`, `lapTimeChangeAfterCliff`) · `test_tire_module.cpp` |
| B. Hand-written segment tree (sum, min, update, rebuild), half-open ranges | Done | `SegmentTree`, `TireRangeAnalyzer` · `test_tire_module.cpp` |
| C. Hand-written Fenwick tree, linear build, a real use (traffic loss by lap, position swaps) | Done | `FenwickTree`, `RaceLapRecorder::trafficLoss`, `countOrderSwaps` · `test_tire_module.cpp` |
| D. Threshold queries; binary search only when monotonic | Done | `firstAgeDegradationExceeds`, `longestStintUnderAverage`, `stintLengthsWithin`, `compareCompounds` · `tires threshold` |

## 4.3 Driver and lap analytics

| Plan item | Status | Where |
|---|---|---|
| A. Driver statistics, hash-map aggregation | Done | `DriverAnalytics` · `test_driver_analytics.cpp` |
| A. Average sector time | Done (modelled) | `sectorSummaries` — see the sector note below |
| B. Fastest / slowest K by heap; by driver, compound, lap interval | Done | `selectLaps`, `LapFilter`; `--top-laps`, `--top-laps-car`, `--top-laps-range` |
| C. Monotonic deque, W = 1, W = L, W > L | Done | `MonotonicDeque`, `trailingWindows` |
| D. Rankings and consistency with deterministic ties | Done | `rankDrivers`, `summarizeWindows` |
| E. Sector analytics: fastest, average, best combined, comparison, distribution | Done (modelled) | `modelledSectorTimes`, `sectorSummaries`, `fieldBestSectors`, `compareSectors`, `sectorDistribution`; `--sector-stats`, `--sector-compare` |

**Sector note.** The simulator times whole laps, so no sector is measured. As the plan
instructs, each lap carries a deterministic split in proportion to the track's configured
sector durations, labelled "modelled" in the record, the API and every report. The split
carries no information beyond the lap time; this is stated, not hidden.

## 4.4 Race event engine

| Plan item | Status | Where |
|---|---|---|
| A. Event types: lap completed, pit started / completed, compound changed, position changed, overtake attempt / completed, held by traffic, race finished | Done | `RaceEventType` (`PitEntry`, `PitExit`, `CompoundChange`, `PositionChange`, `OvertakeAttempt`, `Overtake`, `TrafficHold`, `RaceFinish`, `LapCompleted`) · `test_events.cpp` |
| A. Type, car, lap, time, sequence number, payload | Done | `RaceEvent` |
| B. Hand-written min-queue: insert, peek, pop, process up to a time, invalid times rejected | Done | `EventPriorityQueue` |
| B. Order: time, then lap, then sequence | Deviation | time, then type, then car, then sequence (the lap level is "if required" in the plan and unnecessary: laps are determined by time per car) |
| C. Deterministic log, immutable, CLI-visible, no effect on race time | Done | `EventLog`, `--events` |
| D. Race-state transitions, illegal ones rejected | Done | `RaceState`, `validateEventLog` |

## 4.5 Pit and stint analytics

| Plan item | Status | Where |
|---|---|---|
| Stop count, stop laps, compound usage | Done | `DriverPitSummary` |
| Stint distribution, average / longest / shortest stint | Done | `stintStats`, `fieldStintStats`, `stintHistogram` |
| Time in the pit lane, total pit loss | Deviation | `totalPitLoss`, `fieldPitLoss`: the simulator charges a stop as one lump (entry, tire change, exit share a timestamp), so pit-lane time **is** the pit loss; no entry / service / exit split exists to report |
| Pit-stop timing comparison | Done | `compareStopTiming` |
| Strategy stop-count comparison | Done | `bestForStopCount`, `strategy stops` |
| Traffic after a stop | Done | `trafficAfterStops` from `LapRecord`s |
| Pit-window summaries for configured intervals | Done | `windowSummary`, `--pit-window FROM:TO` |
| Sorting (chronological report) | Done | `chronologicalPitStops` |
| Hash maps for aggregation | Done | `allSummaries`, `validateEventLog`, `DriverAnalytics` |
| Interval processing (coverage) | Done | `checkStintCoverage` |
| Prefix sums for cumulative pit statistics | Done | `StopLapIndex` (`buildStopIndex`) |
| Greedy pit-window analysis | Done | `minimumStopLaps` (fewest laps stabbing every stop window), checked against brute force |
| Undercut / overcut, reproducible by independent evaluation | Done | `compareUndercutOvercut`, `compareFirstStopLaps`, `strategy undercut` |

## 4.6 Strategy comparison

| Plan item | Status | Where |
|---|---|---|
| Optimal vs custom, one-stop vs multi-stop, compound-only, alternatives, the three sensitivities | Done | `compareStrategies`, `standardCandidates`, `bestForStopCount`, `compoundOnlyBest`, `kBestPlans`, sweeps |
| Clean air vs overtaking | Done | `cleanAirVsTraffic`, `strategy traffic` |
| Time loss, percentage, per-stint share, infeasibility reason | Done | `StrategyReport` |
| Same assumptions for every candidate | Done | one `PitStrategy` per comparison; printed assumptions line |

## 4.7 Benchmarks

| Plan item | Status | Where |
|---|---|---|
| DP over laps and compounds; prefix sums vs direct; heap top-K vs sort; segment tree, Fenwick, deque, event queue vs reference; alternatives under different bounds | Done | `benchmarks/` · `make bench` |
| Fixed seeds, correctness before timing, complexity printed, median of repeats, small / medium / large | Done | `report()` in both benchmark files |

## 7 CLI

| Plan command | Status | How to reach it |
|---|---|---|
| `strategy evaluate`, `compare`, `alternatives`, `sensitivity` | Done | `boxbox strategy ...` (also `stops`, `traffic`, `undercut`) |
| `tires profile`, `tires range` | Done | `boxbox tires ...` (also `threshold`) |
| `optimize`, `simulate`, `drivers stats`, `drivers top-k`, `drivers windows`, `pit report`, `events log` | Deviation | the same features as flags: default run, `--simulate`, `--driver-stats`, `--top-laps`, `--lap-window`, `--pit-report`, `--events` (the plan lets the syntax be finalized) |
| `benchmark` | Deviation | `make bench`; kept out of the CLI so the CLI never reads the clock (PRD NFR-5) |
| Validate and reject non-finite numbers, deterministic output, units, help text, non-zero exit on error | Done | `test_cli.cpp` (including a 1,500-case fuzz) |

## 10 Integration contracts

| Contract | Status | Where |
|---|---|---|
| A. Lap record | Done | `LapRecord` (sector times only as the modelled split) |
| B. Strategy result: stints, start / end lap, duration, stops, green / pit / total, feasibility, message | Deviation | `StrategyResult` carries start lap and length (end lap = start + length − 1), per-stint time and feasibility; the validation message lives in `PlanValidation` / `StrategyReport::reason`, so `PitStrategy`'s public interface is unchanged (a decision recorded with the project owner) |
| C. Race event | Done | `RaceEvent` |
| D. Driver summary, optional sector statistics | Done | `DriverSummary`, `DriverSectorSummary` |

## 11 Testing

| Plan item | Status | Where |
|---|---|---|
| Unit tests per structure: empty, single, min / max index, invalid index, repeated updates, boundary, duplicates, ties, large | Done | segment tree, Fenwick tree (60,000 slots), deque (100,000 values), event queue (100,000 events, interleaved), leaderboard (50,000 entries, interleaved) |
| Differential tests (all eight pairs) and seeded property tests | Done | every `test_*.cpp` |
| Regression invariants 1-12 | Done | loop closure, traffic accounting, ordering and determinism tests |
| Zero warnings, `make test`, benchmarks separate, no timing thresholds | Done | clean build under `-Wall -Wextra`; stricter set in `docs/COMPLEXITY_AUDIT.md` |

## Deviations, in one place

1. **Event-queue ordering** has no lap level: time, type, car number, sequence.
2. **`StrategyResult`** has no message field; the reason is in `PlanValidation`.
3. **Pit-lane duration** is the lump pit loss; no entry / service / exit split exists.
4. **CLI verbs** `optimize`, `simulate`, `drivers`, `pit`, `events` are flags; `benchmark` is `make bench`.
5. **Sectors** are a modelled split, not measured (the plan's own prescription).
