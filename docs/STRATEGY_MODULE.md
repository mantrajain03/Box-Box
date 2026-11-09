# Strategy Lab

Owner: Sarthak. Plan sections 4.1, 4.6, 4.7 and 8.3. PRD FR-10 to FR-15.

Custom-plan evaluation, sensitivity sweeps, the K cheapest plans, the best plan for each
stop count, strategy comparison, and clean air against traffic. Standard library only.

## Files

| File | What it is |
|---|---|
| `include/boxbox/StrategyAnalytics.hpp`, `src/StrategyAnalytics.cpp` | The module. No printing, no parsing |
| `cli/StrategyCommands.hpp`, `cli/StrategyCommands.cpp` | `boxbox strategy ...`: parsing and printing only |
| `tests/test_strategy_analysis.cpp`, `tests/StrategyTests.hpp` | Checks, run by `make test` |
| `benchmarks/bench_strategy.cpp`, `.hpp` | Optimizer, K-best, and the plan's other benchmark areas, run by `make bench` |
| `include/boxbox/LapTime.hpp` | `canonicalLapTime`, the one lap-time sum shared by the simulator and the tire analytics |

## The rule this module follows

Nothing here computes a lap time or a race total. Every number comes from
`PitStrategy::evaluatePlan` or `PitStrategy::optimize`, which already share one code path
(`buildResult`). A custom plan, an alternative, a sweep row and the optimum are therefore
totalled by identical arithmetic. `PitStrategy` and `StrategyResult` were not changed: the
reason a plan is invalid lives in this module's `validatePlan`, not in a new field.

## API

| Function | What it does | Cost |
|---|---|---|
| `validatePlan(strategy, plan)` | `{valid, message}`; the message is the first failing condition | O(S) |
| `evaluateCustom` / `evaluateAgainst` | Per-stint report, usage, green / pit / total, delta and % against the optimum | O(S) (+ one `optimize()` for `evaluateCustom`) |
| `pitLossSweep(from, to, step)` | Optimal plan at each pit loss; each point builds a `PitStrategy` and calls `optimize()` | O(P·N·C·M) |
| `compoundSweep(index, parameter, from, to, step)` | Rebuilds one compound with the new value and re-optimizes. Parameters: base pace, degradation, max stint, cliff lap, cliff degradation | O(P·N·C·M) |
| `kBestPlans(strategy, K)` | The K cheapest distinct plans, cheapest first | O(N·C·M·K log(C·M·K)) time, O(N·K + C·M·K) memory |
| `bestForStopCount(strategy, S)` | Best plan with exactly 0..S stops; infeasible counts carry a reason | O(S·N·C·M), O(S·N) memory |
| `compoundOnlyBest` | Best plan per single compound | O(C·N·M) |
| `standardCandidates` / `compareStrategies` | Optimum, each compound alone, best per stop count, plus user plans, all under one model | O(sum of S) + one `optimize()` |
| `cleanAirVsTraffic(plans, rule)` | The same field run with overtaking off, then on | O(N·D log D) per race |
| `compareFirstStopLaps(earlier, later)` | Undercut / overcut: stop after lap L or lap M on the optimum's opening compound, then the best continuation on fresh tires (an independent optimize of the remaining laps), costed by `PitAnalytics::compareUndercutOvercut` | two optimize() calls, O(N·C·M) |

N laps, C compounds, M longest stint, S stints in a plan (or stops + 1 where stated),
P sweep points, D cars, K plans.

### Sweeps

Values are computed as `from + i × step`, so rounding does not accumulate; both ends are
included when they fall on a step. A sweep is capped at 1,000 points and rejects a
non-finite value, a non-positive step, `to < from`, a negative pit loss and (for max
stint and cliff lap) a non-whole value. `SweepResult::firstChange` is the first row whose
plan differs from row 0's.

### K cheapest plans

A plan is a path through the lap-count DAG, and a path is a unique sequence of
(compound, length) stints, so distinct DP entries are distinct plans. The DP keeps the
K+1 best (cost, predecessor) entries at every lap instead of one, merged from every
(compound, length, predecessor rank) candidate. The result is **exact**: it is the true K
cheapest, not a sample. The bound is `kMaxAlternatives` = 50; the CLI says "at most 50",
not "all plans".

Order is exact cost, then compound index, stint length and predecessor rank. There is
deliberately no tolerance in that comparison: a tolerance test is not transitive, so it
is not a valid sort key (the same lesson as the event queue). Entry 0 is always the
optimizer's own plan; if the K-best DP ties and ranks another plan first, the optimizer's
plan is put in front and the duplicate dropped. Every returned plan is re-costed with
`evaluatePlan`, so the printed number is the model's, not the DP's running sum.

### Best plan per stop count

A DP with a stint-count dimension: `f[m][l]` is the cheapest way to run `l` laps in
exactly `m` stints. `m = 1` is a single stint over the whole distance. Counts with no
feasible plan are reported with the reason, never skipped.

### Clean air against traffic

Two `RaceSimulator` runs of the same plans, rule off then on. With the rule off each car's
time equals its plan's total within `kClosureTolerance`, so FR-8 holds for every car in
the comparison. Traffic loss is read from `Driver::trafficLoss()`.

## CLI

```
boxbox [race options] strategy evaluate --plan SPEC
boxbox [race options] strategy compare [--plan SPEC ...] [--max-stops N]
boxbox [race options] strategy alternatives [--count K]
boxbox [race options] strategy sensitivity --pit-loss FROM:TO:STEP
boxbox [race options] strategy sensitivity --compound NAME --param P --range FROM:TO:STEP
boxbox [race options] strategy stops [--max N]
boxbox [race options] strategy traffic [--min-gap SECONDS] [--pass-margin SECONDS]
boxbox [race options] strategy undercut --earlier-lap L --later-lap M
```

`strategy compare` and the standard candidate set now also include the best plan using only each
compound (`only-<compound>`, from `compoundOnlyBest`; the earlier name is kept when it duplicates
an `all-<compound>` or `<n>-stop` plan). `strategy undercut` prints both plans and their totals and
says which stop is faster; a first stint longer than its compound allows is reported as
infeasible, never as a number. It is a simplified clean-air model and the output says so.

`SPEC` is `NAME:LAPS` or `NAME:FROM-TO` stints, comma separated. A range must start on the
lap after the previous stint, so a gap or an overlap is an error, not silently reshaped.
`P` is `pace`, `degradation`, `max-stint`, `cliff-lap` or `cliff-deg`.

Every number is checked for being finite and in range. A command parses and computes
everything before it prints, so a rejected request leaves nothing half-written on stdout.
Exit status 1 is bad usage; 2 is a plan you entered that cannot be run (the reason is
printed). Output is deterministic: two runs are byte-identical.

Hand check, the 4-lap race the tests use (one 10 s sector, 5 s pit loss; compound A:
+0 s, 1 s/lap wear, up to 2 laps; compound B: +2 s, no wear, up to 4 laps):

```
boxbox --laps 4 --pit-loss 5 --sector 10 --compound A:0:1:2 --compound B:2:0:4 strategy alternatives --count 4
```

lists A2 A2 = 21 + 21 + 5 = 47, B4 = 48, then B2 A2 and A2 B2 at 24 + 21 + 5 = 50, which
are the worked sums.

## Tests

`make test` runs `test_strategy_analysis.cpp`. The reference is written without the DP or
the prefix table: it enumerates every legal plan and sums each lap with
`canonicalLapTime`. It checks

* every `validatePlan` reason and the 4-lap hand case;
* `evaluateCustom` against the enumeration, for the 25 cheapest plans of each of 60 seeded races;
* 20 seeded pit-loss sweeps against repeated `optimize()`, the extremes, and every rejected range;
* compound sweeps against hand-rebuilt compounds;
* `kBestPlans` against the enumeration, K = 1, 4, 12, 50, on 60 seeded races, plus the
  tiny race where K exceeds the number of plans, reproducibility and the bounds;
* `bestForStopCount` against the enumeration for every stop count;
* comparison sets, clean air against traffic, and the FR-8 regression for every plan the
  lab produces.

Fault injection (throwaway copies): see PRD §8.1.

## Benchmarks

`make bench` runs `bench_strategy.cpp` after the range-query benchmarks. Correctness is
checked first with no clock involved; a timing is never a pass or fail check. It covers
the optimizer against the same DP summing every stint lap by lap, prefix-sum stint cost
against direct summation, K-best for K = 1, 5, 20, 50, and, because the plan asks for them
and nobody else had, the event queue against sorting all events, the bounded top-K heap
against a full sort, and the monotonic deque against rescanning every window.

## Limits

* Simplified model, as the plan requires: no traffic in any plan's total, no tire warm-up,
  no real pit-window rules. Reports say so.
* Pit-loss and compound sweeps are not incremental; each point is a full optimize.
* `kBestPlans` is capped at 50; it is exact below the cap.
* GUI work (CLAUDE.md Phase 6) is not started.
