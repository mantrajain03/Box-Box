# BOX BOX --- Complete Backend Development Plan

> **Project:** Box Box --- F1 Pit-Stop and Race-Strategy Scheduler\
> **Language:** C++17\
> **Core constraint:** All race logic, analytics, optimization, data
> structures, and algorithmic processing must be implemented in C++.\
> **Project type:** Data Structures & Algorithms group project\
> **Team:** Maulshree · Pratyaksh · Sarthak · Mantra\
> **Document purpose:** Consolidated record of the existing
> implementation, planned backend expansion, team ownership, integration
> strategy, milestones, and acceptance criteria.

------------------------------------------------------------------------

## 1. Project Overview

Box Box is a deterministic Formula 1 race-strategy and simulation
engine. It answers the central strategy question:

**How many pit stops should a car make, on which laps, and which tire
compounds should it use to minimize total race time?**

The engine models tire degradation, stint limits, pit-stop losses,
lap-by-lap race progression, competing drivers, live standings, and
deterministic overtaking. Its core optimization problem is represented
as a shortest-path problem over completed lap counts and solved using
dynamic programming.

The project is intentionally DSA-focused. Algorithms and data structures
are not incidental implementation details: they are the central
engineering contribution. Each feature must have a clearly identified
algorithmic purpose, a documented complexity, and tests that validate
its correctness.

### Main objectives

1.  Preserve and strengthen the existing C++17 race-strategy engine.
2.  Expand the engine with meaningful data structures and algorithms
    that solve real race-analysis problems.
3.  Build a deterministic race-event and analytics model that can be
    tested independently.
4.  Add strategy comparison, tire intelligence, driver statistics,
    pit-stop analysis, and algorithm benchmarking.
5.  Maintain a thin command-line interface (CLI) for configuring and
    inspecting engine operations.
6.  Keep the core independent of presentation and input/output concerns.
7.  Ensure all new functionality is covered by deterministic tests and
    honest complexity analysis.

### Non-goals

The following are not part of this development plan:

-   Weather simulation.
-   Safety-car and virtual-safety-car modelling.
-   Real-world telemetry ingestion.
-   Networking or online multiplayer.
-   Persistent accounts or cloud storage.
-   Random race outcomes or unseeded randomness.
-   Fuel, ERS, DRS, aerodynamic, or detailed vehicle-physics modelling.
-   Real-world FIA rule enforcement.
-   A claim that the simplified simulator reproduces a complete real F1
    race.

These can be considered separate future research directions, but must
not delay the agreed scope.

------------------------------------------------------------------------

## 2. Existing Implementation --- Baseline

The following capabilities are already implemented according to the
current project README. They are the baseline and must be preserved.

### 2.1 C++17 core and project structure

-   C++17 implementation.
-   Standard library only in the current core, CLI, and tests.
-   Public headers under `include/boxbox/`.
-   Core implementation under `src/`.
-   CLI under `cli/main.cpp`.
-   Hand-written test harness under `tests/test_main.cpp`.
-   Makefile as the build entry point.
-   GCC/MSYS2 UCRT64 development environment on Windows.
-   Makefile support for Windows shells and Linux `make`.
-   Zero-warning build requirement.

### 2.2 Tire and track model

Each track has sector times. The base lap time is the sum of the sector
times.

Each tire compound contains:

-   Compound name.
-   Base-pace offset.
-   Per-lap degradation.
-   Maximum permitted stint length.
-   Optional cliff lap.
-   Optional additional degradation after the cliff.

The lap-time formula is:

``` text
lapTime(compound, age) =
    track.baseLapTime()
    + compound.basePace
    + compound.degradationPerLap * age
    + cliffPenalty(compound, age)

cliffPenalty =
    compound.cliffDegradationPerLap * (age - compound.cliffLap)
    when age > compound.cliffLap

    0 otherwise
```

Tire age starts at zero on a fresh set. A stop at the end of lap `k`
means the next stint begins on lap `k + 1`.

### 2.3 Dynamic-programming strategy optimizer

The existing `PitStrategy` optimizer minimizes total race time.

``` text
dp[0] = 0

dp[l] = min over compounds c and stint lengths k:
    dp[l-k]
    + stintCost(c, k)
    + (l-k > 0 ? pitLoss : 0)
```

The optimizer:

-   Uses a prefix-sum table for per-compound stint deltas.
-   Answers a stint-cost query in O(1).
-   Tries legal compounds and stint lengths.
-   Records choices for reconstruction.
-   Reconstructs the strategy by backtracking from `dp[N]`.
-   Uses a named floating-point epsilon for deterministic tie handling.

Current complexity:

-   Prefix table construction: O(C·M).
-   Stint cost query: O(1).
-   Optimization: O(N·C·M) time and O(N) space.
-   Plan reconstruction: O(N).
-   Plan evaluation: O(S).

Where: - `N` = race laps. - `C` = number of compounds. - `M` = maximum
legal stint length. - `S` = number of stints in a plan.

### 2.4 Race simulation and loop closure

The existing `RaceSimulator` and `Driver` advance cars one lap at a time
using the same lap-time model as the optimizer.

The loop-closure requirement verifies that replaying the optimizer's
plan produces the optimizer's predicted total time within
`kClosureTolerance = 1e-6` seconds.

This check is a core correctness invariant and must remain enabled in
every applicable simulated run.

### 2.5 Leaderboard and overtaking

The existing `Leaderboard` uses a hand-written binary min-heap.

The ordering is based on:

1.  Laps completed.
2.  Race time.
3.  Car number as a deterministic tie-breaker.

Existing operations include standings and top-K retrieval.

The optional overtaking model applies a deterministic pass-or-hold rule:

-   Cars are processed in running order.
-   A car that reaches the line too close to the car ahead is held at
    the configured minimum gap.
-   A car may pass when its lap-time advantage meets the configured pass
    margin.
-   Pit loss is applied after the crossing is settled.
-   Cars on different laps do not interact.
-   A lone car is never held up.

Traffic time lost is recorded for each car.

### 2.6 Existing CLI options

The current CLI supports:

  Option                    Purpose
  ------------------------- --------------------------------------
  `--laps N`                Race distance
  `--pit-loss SECONDS`      Time lost per pit stop
  `--track NAME`            Circuit name
  `--sector SPEC`           Add or replace track sectors
  `--compound SPEC`         Add or replace tire compounds
  `--show-table`            Display stint-cost table
  `--simulate`              Replay the race and run loop closure
  `--rivals`                Add one reference car per compound
  `--lap-log N`             Print standings every N laps
  `--overtake`              Enable overtaking
  `--min-gap SECONDS`       Minimum following gap
  `--pass-margin SECONDS`   Required advantage to pass

The current exit statuses are:

-   `0`: success.
-   `1`: invalid usage.
-   `2`: no feasible strategy.
-   `3`: loop-closure failure.

### 2.7 Existing tests

The current hand-written test harness reports:

**3,311 checks, 0 failures.**

Existing coverage includes:

-   DP validation against a hand-worked case.
-   DP comparison against brute-force enumeration on seeded
    configurations.
-   Exact race-lap coverage by stints.
-   Zero and large pit-loss cases.
-   Compound switching and tire-age reset.
-   Loop closure on seeded races.
-   Edge cases including zero laps and one lap.
-   Heap validation against brute-force sorting.
-   Deterministic repeated runs.
-   Hand-computed overtaking cases.
-   Seeded traffic invariants.
-   Fault-injection checks against deliberately broken implementations.

The existing test count is a baseline, not a target to preserve
numerically. New tests should increase meaningful coverage without
weakening existing assertions.

------------------------------------------------------------------------

## 3. Target Backend Scope

The expanded backend will be organized into the following functional
modules.

  -----------------------------------------------------------------------
  Module                  Purpose                 Main algorithmic
                                                  contribution
  ----------------------- ----------------------- -----------------------
  Strategy Optimization   Find and compare race   DP, DAG shortest path,
                          strategies              prefix sums,
                                                  backtracking

  Tire Intelligence       Analyze degradation and Prefix sums, segment
                          stint performance       tree, binary search

  Driver Analytics        Calculate driver and    Hash maps, heaps,
                          lap statistics          Fenwick tree, monotonic
                                                  deque

  Race Event Engine       Process race events     Priority queue, event
                          deterministically       ordering, state
                                                  transitions

  Pit-Stop Analytics      Analyze stops, stints,  Sorting, interval
                          and pit windows         processing, greedy
                                                  analysis

  Strategy Comparison     Evaluate custom and     DP, plan evaluation,
                          alternative plans       k-best candidate
                                                  exploration

  Benchmarking &          Measure algorithm       Brute-force references,
  Validation              performance and verify  seeded tests,
                          correctness             complexity experiments

  CLI Integration         Expose the engine's     Input validation and
                          capabilities through    formatted reporting
                          commands                
  -----------------------------------------------------------------------

All modules must call the C++ core. The CLI must not duplicate
algorithmic logic.

------------------------------------------------------------------------

## 4. Detailed Feature and Algorithm Plan

## 4.1 Strategy Optimization and Strategy Lab

### Existing functionality to preserve

-   Minimum-time strategy using dynamic programming.
-   Legal stint-length enforcement.
-   Compound selection.
-   Pit-loss accounting.
-   Prefix-sum-based stint-cost queries.
-   Backtracking to reconstruct the optimal strategy.
-   Deterministic tie-breaking.
-   Plan evaluation.
-   Optional stint-cost table.

### Planned additions

#### A. Custom strategy evaluation

Allow a user to provide a sequence of stints, each containing:

-   Compound.
-   Starting lap.
-   Ending lap or stint length.

The engine must validate:

-   Stints cover the race exactly.
-   There are no gaps or overlaps.
-   Each stint uses a known compound.
-   Each stint respects the compound's maximum stint length.
-   Pit stops occur only between stints.
-   The first stint does not incur a pit loss before the race.
-   The final stint does not incur an unnecessary pit loss after the
    race.

Return:

-   Green-flag time.
-   Pit time.
-   Total time.
-   Number of stops.
-   Per-stint cost.
-   Difference from the optimal DP result.
-   A clear invalid-plan reason when validation fails.

**Algorithm:** Linear plan traversal.

**Target complexity:** O(S), where S is the number of stints.

#### B. Pit-loss sensitivity sweep

Evaluate the optimal strategy for a range of pit-loss values.

For each tested pit loss, report:

-   Optimal total time.
-   Number of stops.
-   Stint sequence.
-   Difference from the previous tested value.
-   Whether the selected strategy changed.

The sweep must call the existing optimizer with each requested pit-loss
value. It must not use a separate approximate strategy formula.

**Algorithm:** Repeated DP optimization.

**Target complexity:** O(P·N·C·M), where P is the number of pit-loss
samples.

#### C. Compound parameter sensitivity

Allow controlled variation of a selected compound parameter, such as:

-   Base pace.
-   Degradation per lap.
-   Maximum stint length.
-   Cliff lap.
-   Cliff degradation.

For every variation, recompute the optimal plan and report the result.

This makes it possible to answer questions such as:

-   At what degradation rate does a medium tire stop being competitive?
-   How much faster must a hard compound become to change the selected
    strategy?
-   How does a shorter maximum stint affect the number of stops?

#### D. Alternative strategy exploration

Add an optional strategy exploration mode that returns several distinct
feasible strategies, ordered by total time.

The initial implementation should use a clearly documented and tested
approach. A suitable option is a bounded k-best path exploration over
the lap-count DAG.

Requirements:

-   The best result must match the existing optimizer's result.
-   Returned strategies must be distinct by their stint sequence.
-   Each result must be independently evaluated.
-   The requested number of strategies must be bounded.
-   The algorithm must document its actual complexity and memory use.
-   If a bounded search is used, the CLI must clearly state that the
    list is the best found within the configured search bounds rather
    than claiming an unbounded global k-best guarantee.

**DSA:** DAG path exploration, priority queue, dynamic programming.

### Acceptance criteria

-   Existing optimal plans remain unchanged for existing fixtures.
-   Custom strategies are validated and evaluated correctly.
-   Sensitivity sweeps match independent repeated optimizer calls.
-   Alternative strategies are valid, distinct, ordered, and
    reproducible.
-   The best alternative is equal to the established DP optimum.

------------------------------------------------------------------------

## 4.2 Tire Intelligence and Degradation Analytics

This module turns the tire model into a reusable analytical component.

### A. Degradation profile

For each compound, calculate and expose:

-   Lap time by tire age.
-   Cumulative degradation.
-   Incremental degradation per lap.
-   Cliff activation point.
-   Lap-time change after cliff activation.
-   Maximum legal stint.
-   Total cost for every legal stint length.

The existing prefix-sum logic should be reused where possible rather
than duplicated.

### B. Segment tree for range analytics

Implement a hand-written segment tree for range queries over generated
lap-time or degradation arrays.

Initial supported operations:

-   Range sum.
-   Range minimum.
-   Point update, if the selected use case requires changing a lap
    value.
-   Rebuild after a compound or parameter configuration changes.

Use cases:

-   Total degradation over a selected age interval.
-   Minimum lap time in an age interval.
-   Compare selected windows of tire performance.
-   Recalculate analytics after a controlled parameter change.

The implementation must define whether ranges are inclusive or half-open
and use that convention consistently.

**Target complexity:**

  Operation      Complexity
  -------------- ------------
  Build          O(M)
  Range query    O(log M)
  Point update   O(log M)
  Storage        O(M)

A segment tree is justified only for range queries and updates. It must
not replace the simpler prefix-sum implementation for static stint-cost
queries where prefix sums are already optimal.

### C. Fenwick tree for cumulative statistics

Implement a hand-written Fenwick tree for point updates and prefix/range
sums over lap-indexed statistics.

Possible use cases:

-   Cumulative lap-time deltas.
-   Cumulative traffic loss by lap.
-   Cumulative driver performance values.
-   Fast range totals over recorded lap statistics.

**Target complexity:**

  -----------------------------------------------------------------------
  Operation                           Complexity
  ----------------------------------- -----------------------------------
  Point update                        O(log N)

  Prefix sum                          O(log N)

  Range sum                           O(log N)

  Build                               O(N log N) for a simple
                                      update-based build, or O(N) if a
                                      linear build is implemented and
                                      tested
  -----------------------------------------------------------------------

Do not use both the Fenwick tree and segment tree for identical
operations without explaining the trade-off. The module must demonstrate
why each structure is useful.

### D. Stint-length and degradation threshold analysis

Add queries such as:

-   Longest stint that remains under a configured average lap-time
    threshold.
-   First tire age at which degradation exceeds a configured limit.
-   First age at which one compound becomes slower than another.
-   Legal stint lengths that satisfy a specified time or degradation
    constraint.

Use binary search only when the searched property is monotonic. If a
cliff or compound crossover makes the property non-monotonic, use a
correct scan or a documented alternative instead of applying binary
search incorrectly.

### Acceptance criteria

-   Segment-tree queries match brute-force range calculations.
-   Fenwick queries match brute-force prefix and range sums.
-   Tire profiles match the canonical lap-time model.
-   Threshold queries handle cliff boundaries and non-monotonic cases
    correctly.
-   Existing stint-cost prefix sums remain the optimizer's source of
    truth.

------------------------------------------------------------------------

## 4.3 Driver and Lap Analytics

This module calculates statistics from completed simulation data. It
must consume the simulator's recorded results rather than inventing
independent race outcomes.

### A. Driver statistics

For every driver, calculate:

-   Total race time.
-   Green-flag time.
-   Pit time.
-   Traffic time lost.
-   Number of pit stops.
-   Number of completed laps.
-   Average lap time.
-   Best lap time.
-   Worst lap time.
-   Average sector time, when sector-level records are available.
-   Tire compound usage.
-   Stint history.
-   Position history, when recorded.

Use hash maps keyed by driver ID or car number for efficient driver
lookup and aggregation.

**Expected average complexity:** O(1) lookup for `std::unordered_map`,
with worst-case O(D) lookup under pathological hashing.

### B. Fastest and slowest lap queries

Use heap-based structures for top and bottom lap-time queries.

Support:

-   Fastest K laps.
-   Slowest K laps.
-   Fastest lap by driver.
-   Fastest lap by compound.
-   Fastest lap within a selected lap interval.

For top-K queries, use a bounded heap where appropriate.

**Target complexity:** O(L log K) for selecting K best or worst values
from L lap records, with O(K) auxiliary storage.

### C. Monotonic deque for sliding-window analysis

Implement a monotonic deque for efficient sliding-window minimum and
maximum lap-time queries.

Use cases:

-   Best lap in the last W laps.
-   Worst lap in the last W laps.
-   Rolling pace indicators.
-   Detecting local performance deterioration.
-   Comparing short-window pace between drivers.

**Target complexity:** O(L) time over L lap records, because each item
enters and leaves the deque at most once.

The window size must be configurable and edge cases such as W = 1, W =
L, and W \> L must be handled explicitly.

### D. Driver ranking and consistency

Provide deterministic ranking reports.

Possible consistency metrics:

-   Mean lap time.
-   Minimum and maximum lap time.
-   Range between best and worst lap.
-   Mean absolute deviation from the driver's average.
-   Sliding-window pace variation.

Every metric must have a documented definition. Do not label a metric as
a real-world F1 performance score; it is a project-defined statistic
derived from the simulator's simplified model.

### E. Sector analytics

When sector-level lap records are available, calculate:

-   Fastest sector per driver.
-   Average sector time.
-   Best combined sector performance.
-   Sector-by-sector driver comparison.
-   Sector time distribution.

The current track model stores sector durations as track configuration.
If the simulator does not yet record sector-level driver performance,
first add a deterministic sector decomposition to the lap record. Do not
claim to simulate sector-by-sector vehicle interactions.

### Acceptance criteria

-   Driver aggregates match direct calculations over stored lap records.
-   Top-K results match full sorting on test fixtures.
-   Monotonic-deque results match brute-force window scans.
-   Ranking tie-breaks are deterministic.
-   Sector analytics clearly distinguish configured sector durations
    from simulated sector performance.

------------------------------------------------------------------------

## 4.4 Race Event Engine

Add a deterministic event-processing layer to organize race events and
their ordering.

### A. Event model

Define a typed race event structure. Initial event types may include:

-   Lap completed.
-   Pit stop started.
-   Pit stop completed.
-   Compound changed.
-   Driver position changed.
-   Overtake attempt.
-   Overtake completed.
-   Car held by traffic.
-   Race finished.

Every event should include enough information to identify:

-   Event type.
-   Driver or car ID.
-   Lap number.
-   Simulation timestamp or elapsed time.
-   Stable sequence number for deterministic tie-breaking.
-   Relevant event payload.

The event engine must not introduce weather, safety cars, or unsupported
external race conditions.

### B. Priority queue

Implement a hand-written min-priority queue for scheduled events,
ordered by:

1.  Event time.
2.  Lap number, if required by the event-time convention.
3.  Stable insertion sequence.

Support:

-   Insert event.
-   Inspect next event.
-   Remove next event.
-   Process all events up to a supplied simulation time.
-   Reject or safely handle invalid event times according to a
    documented policy.

**Target complexity:**

  Operation          Complexity
  ------------------ ------------
  Insert             O(log E)
  Peek               O(1)
  Pop                O(log E)
  Process E events   O(E log E)

Where E is the number of queued events.

### C. Event log

Store the processed event sequence for deterministic replay and
analysis.

Requirements:

-   Same input produces the same event order.
-   Simultaneous events use stable tie-breaking.
-   Event logs can be inspected through the CLI.
-   Event records are immutable after processing, unless a specific
    correction model is deliberately designed.
-   Event logging must not alter race time or driver state.

### D. Race-state transitions

Define and validate legal state transitions for a driver, for example:

``` text
RACING -> PIT_LANE -> RACING
RACING -> FINISHED
```

Invalid transitions should produce explicit errors or failed checks.

The state machine should be small and deterministic. It should not imply
a complete FIA race-control model.

### Acceptance criteria

-   Priority-queue output matches a reference sort by the same ordering
    keys.
-   Equal-time events are deterministic.
-   Invalid state transitions are rejected.
-   Event logging does not change simulation totals.
-   Existing lap-by-lap simulation and loop closure remain valid.

------------------------------------------------------------------------

## 4.5 Pit-Stop and Stint Analytics

This module analyzes pit-stop decisions and stint structure using
existing simulation and strategy data.

### Features

-   Pit-stop count by driver.
-   Pit-stop lap numbers.
-   Time spent in the pit lane.
-   Total pit loss.
-   Stint length distribution.
-   Compound usage by driver.
-   Average stint length.
-   Longest and shortest stint.
-   Pit-stop timing comparison.
-   Strategy stop-count comparison.
-   Traffic time after a stop, where the simulator records enough data.
-   Pit-window summaries for configured lap intervals.

### Algorithms

-   Sorting for chronological pit-event reports.
-   Hash maps for per-driver aggregation.
-   Interval processing for stint overlap and coverage checks.
-   Prefix sums for cumulative pit and stint statistics.
-   Greedy analysis for explicitly defined pit-window selection
    problems.

### Undercut and overcut scenario analysis

Add a simplified scenario evaluator using the existing lap-time model.

The evaluator may compare:

-   A driver stopping one or more laps earlier.
-   A driver extending the current stint.
-   The resulting clean-air time under the model.
-   The total time difference under configured assumptions.

This must be described as a simplified model. It must not claim to
reproduce real-world traffic, tire warm-up, or full F1 pit strategy.

### Acceptance criteria

-   Pit-stop totals agree with driver stop records.
-   Stints tile the race without gaps or overlaps.
-   Pit-time totals match configured pit loss and recorded stops.
-   Scenario results can be reproduced by independent plan evaluation.
-   Reports clearly state the assumptions used.

------------------------------------------------------------------------

## 4.6 Strategy Comparison and Scenario Analysis

This module brings the optimizer, evaluator, simulator, and analytics
together.

### Features

-   Optimal strategy versus custom strategy.
-   One-stop versus multi-stop comparison.
-   Compound-only strategy comparison.
-   Alternative strategy ranking.
-   Pit-loss sensitivity.
-   Tire degradation sensitivity.
-   Maximum-stint-limit sensitivity.
-   Clean-air versus overtaking-enabled simulation comparison.
-   Strategy time loss and percentage difference.
-   Per-stint contribution to total time.
-   Explanation of why a candidate strategy is infeasible.

### Required output for each strategy

-   Strategy name or identifier.
-   Stint sequence.
-   Number of stops.
-   Compound usage.
-   Green-flag time.
-   Pit time.
-   Total time.
-   Difference from the optimum.
-   Feasibility status.
-   Evaluation details.

### Correctness rule

A strategy comparison must use the same track, tire model, race
distance, and pit-loss assumptions for every candidate in a comparison
set unless the report explicitly identifies a scenario change.

The existing optimizer remains the source of the global optimum for the
supported model. Custom and alternative plans must be evaluated with the
same cost model.

------------------------------------------------------------------------

## 4.7 Algorithm Benchmarking and Complexity Analysis

Create a dedicated benchmark capability to demonstrate the actual
performance of the implemented algorithms.

### Benchmark areas

-   DP optimizer over increasing lap counts.
-   DP optimizer over increasing compound counts.
-   Prefix-sum stint-cost queries versus direct summation.
-   Heap top-K versus full sorting.
-   Segment-tree range queries versus brute-force range scans.
-   Fenwick-tree operations versus direct prefix calculations.
-   Monotonic-deque windows versus brute-force window scans.
-   Priority-queue event processing versus sorting all events.
-   Alternative strategy search under different bounds.

### Requirements

-   Use fixed seeds for generated datasets.
-   Use repeatable input sizes.
-   Separate correctness comparison from timing measurement.
-   Never use benchmark timing as a correctness assertion.
-   Print operation counts and input dimensions.
-   Report the theoretical complexity next to measured results.
-   Avoid claiming that one implementation is faster based on a single
    noisy run.
-   Include small, medium, and large test sizes.
-   Ensure benchmark runs do not change normal CLI output or test
    behavior.

### CLI output example

``` text
Algorithm Benchmark

Operation: Segment Tree Range Sum
Input size: 100000
Queries: 50000
Reference: Brute-force range sum

Correctness: PASS
Theoretical query complexity: O(log N)
Measured implementation time: ...
Reference implementation time: ...
```

The numbers above are placeholders. The program must generate actual
measurements.

------------------------------------------------------------------------

## 5. Data Structures and Algorithms Register

This register is the agreed DSA scope. A structure should only be
considered complete after its implementation, tests, complexity notes,
and a real use case are present.

  -----------------------------------------------------------------------
  DSA / Algorithm         Status                  Planned use
  ----------------------- ----------------------- -----------------------
  Dynamic Programming     Implemented             Optimal stint selection

  Prefix Sums             Implemented             O(1) stint-cost queries

  DAG Shortest Path       Implemented through DP  Minimum-cost lap-count
  formulation                                     path

  Backtracking            Implemented             Reconstruct optimal
                                                  plan

  Custom Binary Min-Heap  Implemented             Live leaderboard

  Sorting                 Existing / extend       Classification and
                                                  analytics

  Brute-force enumeration Existing tests          Independent correctness
                                                  reference

  Hash Maps               Implemented             Driver and event
                          (std::unordered_map)    aggregation

  Segment Tree            Implemented              Dynamic range analytics

  Fenwick Tree            Implemented              Cumulative lap and
                                                  traffic statistics

  Monotonic Deque         Implemented              Sliding-window lap
                                                  analytics

  Custom Event Priority   Implemented              Deterministic event
  Queue                                           scheduling

  Binary Search           Implemented              Monotonic threshold
                                                  queries

  Interval Processing     Planned                 Stint and pit-window
                                                  analysis

  Bounded k-best DAG      Implemented              Alternative strategy
  exploration                                     exploration

  Greedy analysis         Planned                 Explicitly defined
                                                  pit-window scenarios
  -----------------------------------------------------------------------

**Status note (repo audit):** the structures marked Implemented have
headers and sources in `include/boxbox/` and `src/`. Interval Processing
and Greedy analysis were not confirmed in the code and remain Planned.
Deviations from this plan: headers are flat in `include/boxbox/` (no
`analytics/`, `data_structures/`, `events/` subfolders); driver, pit and
event reports are CLI flags (`--driver-stats`, `--top-laps`,
`--lap-window`, `--pit-report`, `--events`) rather than `drivers`, `pit`
and `events` command groups; there is no `benchmark` CLI command, only
`make bench`; tests are split as `test_tire_module.cpp` and
`test_driver_analytics.cpp` instead of `test_data_structures.cpp` and
`test_analytics.cpp`.

### Implementation policy

-   The existing custom min-heap must remain a hand-written
    implementation.
-   The new segment tree, Fenwick tree, monotonic deque, and event
    priority queue should be implemented explicitly by the team.
-   Standard-library containers may be used where the project is not
    specifically demonstrating a custom data structure, but the selected
    container and complexity must be documented.
-   Do not implement a custom version of a data structure without a real
    use case.
-   Do not add Disjoint Set Union, trie, or unrelated textbook
    algorithms solely to increase the DSA count. They are not required
    by the current race model.

------------------------------------------------------------------------

## 6. Backend Architecture and Module Boundaries

The core must remain independent of the CLI and any external runtime.

### Proposed logical structure

``` text
box-box/
|
|-- include/
|   `-- boxbox/
|       |-- Common.hpp
|       |-- Track.hpp
|       |-- TireCompound.hpp
|       |-- PitStrategy.hpp
|       |-- Driver.hpp
|       |-- Leaderboard.hpp
|       |-- RaceSimulator.hpp
|       |
|       |-- analytics/
|       |   |-- TireAnalytics.hpp
|       |   |-- DriverAnalytics.hpp
|       |   |-- PitAnalytics.hpp
|       |   `-- StrategyAnalytics.hpp
|       |
|       |-- data_structures/
|       |   |-- SegmentTree.hpp
|       |   |-- FenwickTree.hpp
|       |   |-- MonotonicDeque.hpp
|       |   `-- EventPriorityQueue.hpp
|       |
|       `-- events/
|           |-- RaceEvent.hpp
|           |-- EventLog.hpp
|           `-- RaceState.hpp
|
|-- src/
|   |-- existing core implementation
|   |-- analytics/
|   |-- data_structures/
|   `-- events/
|
|-- cli/
|   `-- main.cpp
|
|-- tests/
|   |-- test_main.cpp
|   |-- test_data_structures.cpp
|   |-- test_analytics.cpp
|   |-- test_events.cpp
|   `-- test_strategy_analysis.cpp
|
|-- benchmarks/
|   `-- benchmark_main.cpp
|
|-- Makefile
|-- PRD.md
`-- README.md
```

This is a proposed logical organization. The team may adapt file
placement to the current repository, but public interfaces and module
boundaries must remain clear.

### Dependency rules

1.  Data structures must not depend on CLI code.
2.  Analytics modules may depend on core models and data structures.
3.  The simulator may emit events but must not depend on report
    formatting.
4.  Strategy analysis must reuse `PitStrategy` and the canonical plan
    evaluator.
5.  CLI code may call public core APIs and format returned results.
6.  Tests must be able to compile against the core without invoking the
    CLI.
7.  Benchmark code must be separate from normal tests.
8.  No module may silently duplicate the canonical lap-time formula.

### Source-of-truth rules

-   `Track` is the source of track sector configuration and base lap
    time.
-   `TireCompound` is the source of tire parameters.
-   The canonical lap-time model is the source of lap-time calculations.
-   `PitStrategy` is the source of the supported model's optimum.
-   `evaluatePlan` is the source of custom strategy totals.
-   `RaceSimulator` is the source of simulated driver and race records.
-   Analytics modules derive reports from those records.
-   Event logs record state changes; they do not independently alter
    race calculations.

------------------------------------------------------------------------

## 7. CLI Expansion Plan

The CLI remains the project's operational interface. New commands should
be added without breaking existing options.

The exact syntax can be finalized during implementation, but the
following command groups are planned.

  Command group             Purpose
  ------------------------- ----------------------------------------
  `optimize`                Run the optimal strategy calculation
  `simulate`                Run the race simulation
  `strategy evaluate`       Evaluate a custom strategy
  `strategy compare`        Compare multiple strategies
  `strategy alternatives`   Explore bounded alternative strategies
  `strategy sensitivity`    Sweep pit loss or tire parameters
  `tires profile`           Print tire degradation profiles
  `tires range`             Run segment-tree range queries
  `drivers stats`           Print driver statistics
  `drivers top-k`           Query fastest or slowest laps
  `drivers windows`         Print sliding-window statistics
  `pit report`              Print pit-stop and stint analytics
  `events log`              Print deterministic race event history
  `benchmark`               Run algorithm benchmarks

### CLI design requirements

-   Preserve all current command-line options and behavior.
-   Validate all numeric input and reject non-finite floating-point
    values.
-   Print useful error messages.
-   Keep parsing and formatting separate from algorithms.
-   Make output deterministic for identical inputs.
-   Use explicit units in output, especially seconds and laps.
-   Provide help text for every new command.
-   Return non-zero exit codes for invalid input or failed validation.
-   Do not hide infeasible strategies or silently substitute defaults.

------------------------------------------------------------------------

## 8. Four-Person Responsibility and Contribution Plan

Each person owns a major backend module. Ownership means responsibility
for design, implementation, unit tests, complexity documentation,
integration, and a short explanation of the algorithms used.

All members must review one another's interfaces and participate in
final integration. Module ownership is not a reason to work in
isolation.

## 8.1 Maulshree --- Tire Intelligence and Range-Query Data Structures

**Primary ownership:** Tire analytics, degradation analysis, Segment
Tree, Fenwick Tree.

### Responsibilities

1.  Study and document the existing tire model and its age/degradation
    rules.
2.  Build the tire degradation profile API.
3.  Expose lap-time and cumulative-degradation arrays for analytics.
4.  Implement a hand-written Segment Tree.
5.  Support range-sum and range-minimum queries.
6.  Add point updates only where the selected analytics use case needs
    them.
7.  Implement a hand-written Fenwick Tree.
8.  Add cumulative and range statistics for lap-time deltas or traffic
    loss.
9.  Implement threshold and compound-crossover queries.
10. Use binary search only for proven monotonic properties.
11. Add tests for cliff boundaries, zero degradation, maximum stint
    limits, and range-query edge cases.
12. Add benchmark comparisons against brute-force scans.

### Expected deliverables

-   `SegmentTree.hpp/.cpp`
-   `FenwickTree.hpp/.cpp`
-   `TireAnalytics.hpp/.cpp`
-   Tire profile and threshold-query CLI integration.
-   Segment-tree and Fenwick-tree tests.
-   Benchmark cases for range-query structures.
-   Complexity documentation and usage examples.

### Required correctness checks

-   Segment-tree results match brute-force range calculations.
-   Fenwick-tree results match direct prefix/range sums.
-   Point updates change only the expected ranges.
-   Tire profiles match the canonical lap-time formula.
-   Cliff activation is correct at the boundary and immediately after
    it.
-   Static stint-cost queries continue to use prefix sums.

### Collaboration

-   Coordinate with Sarthak on compound sensitivity and optimizer
    inputs.
-   Coordinate with Mantra on lap-indexed driver statistics.
-   Coordinate with Pratyaksh if event records need cumulative tire or
    lap analytics.

------------------------------------------------------------------------

## 8.2 Pratyaksh --- Race Event Engine and Pit-Stop Analytics

**Primary ownership:** Race events, event priority queue, event log,
pit-stop and stint reporting.

### Responsibilities

1.  Design a typed race-event model.
2.  Define deterministic event ordering and stable tie-breaking.
3.  Implement a hand-written min-priority queue.
4.  Support event insertion, peek, pop, and processing through a
    supplied time.
5.  Add a deterministic event log.
6.  Record lap completion, pit entry, pit exit, compound changes,
    overtaking, traffic holds, and race finish where those events are
    available from the existing simulator.
7.  Define legal driver race-state transitions.
8.  Add pit-stop and stint analytics.
9.  Calculate stop counts, pit loss, stint lengths, and compound usage.
10. Add pit-window summaries.
11. Implement simplified undercut/overcut scenario evaluation using the
    canonical lap-time model and existing plan evaluator.
12. Test event ordering, simultaneous events, invalid transitions, and
    event-log determinism.

### Expected deliverables

-   `RaceEvent.hpp/.cpp`
-   `EventPriorityQueue.hpp/.cpp`
-   `EventLog.hpp/.cpp`
-   `RaceState.hpp/.cpp`
-   `PitAnalytics.hpp/.cpp`
-   Event-log and pit-report CLI integration.
-   Priority-queue, event, and pit-analytics tests.
-   Complexity documentation and usage examples.

### Required correctness checks

-   Priority-queue order matches a reference sort.
-   Equal-time events are ordered deterministically.
-   Every recorded pit stop corresponds to a valid stint transition.
-   Pit totals match the configured and recorded values.
-   Event logging does not change driver race times.
-   Illegal state transitions are rejected.
-   Existing loop closure remains unchanged.

### Collaboration

-   Coordinate with Mantra on the driver and lap records needed for
    event summaries.
-   Coordinate with Sarthak on strategy scenario evaluation.
-   Coordinate with Maulshree if event-derived lap statistics are used
    in range queries.

------------------------------------------------------------------------

## 8.3 Sarthak --- Strategy Optimization, Comparison and Algorithm Integration

**Primary ownership:** Existing optimizer stewardship, strategy
analysis, sensitivity sweeps, alternative strategy exploration,
integration standards.

### Responsibilities

1.  Audit and preserve the existing DP optimizer and prefix-sum
    implementation.
2.  Maintain the canonical lap-time and stint-cost source of truth.
3.  Add custom strategy validation and evaluation reporting.
4.  Add pit-loss sensitivity sweeps using repeated calls to the existing
    optimizer.
5.  Add tire-parameter sensitivity analysis.
6.  Design and implement bounded k-best alternative strategy exploration
    over the lap-count DAG, or document a more suitable exact/bounded
    method before implementation.
7.  Ensure alternative strategies are distinct and independently
    evaluated.
8.  Add strategy comparison reports.
9.  Add per-stint time contribution and time-loss calculations.
10. Add clean-air versus overtaking-enabled scenario comparisons using
    existing simulator behavior.
11. Maintain deterministic tie-breaking and floating-point tolerance
    rules.
12. Coordinate module interfaces and integrate all members' work into
    the core and CLI.
13. Maintain regression tests for the existing optimizer and
    loop-closure invariant.
14. Review complexity claims across the project for accuracy.

### Expected deliverables

-   Updated `PitStrategy` APIs where required.
-   `StrategyAnalytics.hpp/.cpp`
-   Custom strategy validation and comparison.
-   Pit-loss and tire-parameter sensitivity sweeps.
-   Bounded alternative strategy exploration.
-   Strategy-analysis CLI integration.
-   Regression tests and brute-force comparisons.
-   Integration notes and final complexity audit.

### Required correctness checks

-   Existing DP fixtures return the same optimal strategy.
-   Custom strategy totals match independent direct calculations.
-   Sensitivity results match repeated optimizer invocations.
-   Alternative strategies are valid, distinct, ordered, and
    reproducible.
-   The best alternative matches the established DP optimum.
-   Loop closure remains within `kClosureTolerance`.
-   No algorithm uses floating-point equality where a named tolerance is
    required.

### Collaboration

-   Coordinate with Maulshree on tire-parameter sensitivity.
-   Coordinate with Pratyaksh on pit-window and undercut/overcut
    scenario assumptions.
-   Coordinate with Mantra on driver analytics and race-record
    interfaces.
-   Own the final integration checklist, but all members must review and
    test integrated changes.

------------------------------------------------------------------------

## 8.4 Mantra --- Driver Analytics and Lap-Performance Data Structures

**Primary ownership:** Driver statistics, lap records, hash-map
aggregation, monotonic deque, ranking and sector analytics.

### Responsibilities

1.  Audit the existing `Driver` and `RaceSimulator` data available after
    each lap.
2.  Define or extend a stable lap-record structure.
3.  Build per-driver aggregation using hash maps.
4.  Calculate average, fastest, slowest, and total lap times.
5.  Calculate green-flag, pit, and traffic time contributions where
    recorded.
6.  Add fastest-K and slowest-K lap queries using heap-based selection.
7.  Implement a hand-written monotonic deque for sliding-window minimum
    and maximum lap times.
8.  Add rolling pace and consistency statistics.
9.  Add deterministic driver ranking with explicit tie-breakers.
10. Add sector-level analytics only where the simulator has sufficient
    sector records.
11. If sector records are not available, clearly distinguish track
    configuration values from simulated driver sector performance.
12. Add tests comparing optimized queries with brute-force calculations
    and full sorting.

### Expected deliverables

-   `DriverAnalytics.hpp/.cpp`
-   `LapRecord.hpp` or equivalent shared record definition.
-   `MonotonicDeque.hpp/.cpp`
-   Driver aggregation and ranking APIs.
-   Fastest-K and slowest-K query APIs.
-   Sliding-window analytics.
-   Sector analytics where supported by recorded data.
-   Driver-analytics CLI integration.
-   Tests and complexity documentation.

### Required correctness checks

-   Aggregated values match direct calculations from lap records.
-   Top-K results match full sorting.
-   Sliding-window results match brute-force scans.
-   Window boundary cases are correct.
-   Ranking is deterministic under ties.
-   Traffic loss and pit time are not double-counted.
-   Missing sector records are not presented as measured sector
    performance.

### Collaboration

-   Coordinate with Pratyaksh on event and pit-stop records.
-   Coordinate with Maulshree on Fenwick-tree use for cumulative driver
    statistics.
-   Coordinate with Sarthak on shared simulation output and strategy
    comparison records.

------------------------------------------------------------------------

## 9. Shared Team Responsibilities

The four ownership areas are distinct, but the following
responsibilities are shared.

### Every member must

-   Work in C++17.
-   Follow the existing naming and formatting conventions.
-   Add unit tests for every new public operation.
-   Document time and space complexity.
-   Validate edge cases and invalid inputs.
-   Avoid unnecessary dependencies.
-   Preserve deterministic behavior.
-   Avoid duplicate implementations of the lap-time model.
-   Write meaningful comments explaining non-obvious algorithmic
    decisions.
-   Review at least one other member's module.
-   Participate in integration testing.
-   Be able to explain the algorithm, its invariant, complexity, and
    tests during the project demonstration.

### Shared code-review checklist

Before merging a feature:

-   Does it solve a real project requirement?
-   Is the algorithm implemented in C++?
-   Is the data structure appropriate for the operation?
-   Is the complexity claim accurate?
-   Are empty and boundary inputs handled?
-   Are floating-point comparisons tolerance-based?
-   Is behavior deterministic?
-   Are tests independent enough to catch implementation errors?
-   Does the feature preserve existing behavior?
-   Is the CLI only coordinating input and output?

------------------------------------------------------------------------

## 10. Integration Contracts

The team must agree on shared records and API contracts before parallel
implementation.

### A. Lap record

A lap record should contain, where available:

-   Driver or car identifier.
-   Lap number.
-   Lap time.
-   Tire compound.
-   Tire age.
-   Pit-time contribution, if applicable.
-   Traffic-time contribution, if applicable.
-   Sector times, only if actually recorded.
-   Race position or completed-lap count, if available.

Use one canonical record definition. Do not create incompatible copies
in different modules.

### B. Strategy result

A strategy result should contain:

-   Ordered stints.
-   Compound per stint.
-   Start and end lap.
-   Stint duration.
-   Number of stops.
-   Green-flag time.
-   Pit time.
-   Total time.
-   Feasibility status.
-   Any validation message.

### C. Race event

A race event should contain:

-   Event type.
-   Driver or car ID.
-   Lap.
-   Simulation time.
-   Stable sequence number.
-   Event-specific payload.

### D. Driver summary

A driver summary should contain:

-   Driver ID and display label.
-   Completed laps.
-   Total race time.
-   Green-flag time.
-   Pit time.
-   Traffic time.
-   Stop count.
-   Best and average lap time.
-   Tire/stint summary.
-   Optional sector statistics when available.

### Integration rule

Public headers and record definitions must be agreed upon before
separate modules depend on them. If a shared structure changes, the
owner must notify all dependent members and update tests.

------------------------------------------------------------------------

## 11. Testing and Quality Plan

The existing 3,311 checks and all current regression tests must continue
to pass.

### Unit tests

Each new data structure must have tests for:

-   Empty input.
-   Single-element input.
-   Minimum and maximum valid indices.
-   Invalid indices.
-   Repeated updates.
-   Boundary ranges.
-   Duplicate values.
-   Deterministic tie cases.
-   Large but reasonable inputs.

### Differential tests

Optimized algorithms should be checked against simple reference
implementations:

  Optimized implementation   Reference
  -------------------------- -----------------------------------------
  Segment tree               Brute-force range scan
  Fenwick tree               Direct prefix/range summation
  Monotonic deque            Brute-force window min/max
  Priority queue             Full sort by event ordering
  Top-K heap                 Full sort
  DP optimizer               Brute-force enumeration for small cases
  Strategy evaluator         Direct lap-by-lap summation
  Driver aggregation         Direct traversal of lap records

### Property-based seeded tests

Use fixed-seed generation for:

-   Different race lengths.
-   Different compound counts.
-   Different degradation values.
-   Different maximum stint lengths.
-   Different pit-loss values.
-   Different driver counts.
-   Different event times, including equal-time events.
-   Different lap-record sequences.
-   Different range-query boundaries.

### Regression invariants

1.  Stints cover exactly the requested race distance.
2.  No stint exceeds its compound's legal maximum.
3.  No invalid pit loss is charged before the first stint or after the
    final stint.
4.  Tire age resets after a stop.
5.  The optimizer result is no worse than every feasible plan in small
    brute-force cases.
6.  The optimizer's plan closes against simulation within
    `kClosureTolerance`.
7.  Traffic time is never negative.
8.  Recorded traffic loss matches the additional simulated time
    attributed to traffic.
9.  Leaderboard ordering is deterministic.
10. Event ordering is deterministic.
11. Analytics agree with the underlying records.
12. Identical input produces identical non-timing output.

### Build quality

-   C++17 compilation.
-   Zero compiler warnings under the agreed warning flags.
-   Existing `make` / `mingw32-make` workflow remains usable.
-   `make test` runs the test suite.
-   Benchmark targets remain separate from normal tests.
-   No benchmark timing thresholds are used as correctness tests.

------------------------------------------------------------------------

## 12. Milestones and Execution Order

The schedule below is organized by dependency and completion gates
rather than fixed calendar dates. The team can assign dates after
agreeing on the available project duration.

### Milestone 0 --- Baseline freeze

**Owner:** All members; Sarthak coordinates.

Tasks:

-   Confirm the current repository builds.
-   Run the existing test suite.
-   Record the baseline test output.
-   Identify existing public interfaces.
-   Agree on shared record types and naming.
-   Create feature branches and agree on merge rules.

**Exit criteria:** Existing CLI and tests work; shared module contracts
are documented.

### Milestone 1 --- Independent data structures and records

**Owners:** - Maulshree: Segment Tree and Fenwick Tree. - Pratyaksh:
Event model and priority queue. - Mantra: Lap records and monotonic
deque. - Sarthak: Strategy interfaces and regression protection.

**Exit criteria:** - Each structure has standalone tests. - Public
headers compile independently where practical. - Complexity notes are
present. - No existing functionality is broken.

### Milestone 2 --- Analytics modules

**Owners:** - Maulshree: Tire profiles and range analytics. - Pratyaksh:
Pit and event analytics. - Mantra: Driver and lap analytics. - Sarthak:
Custom strategy evaluation and comparison.

**Exit criteria:** - Each analytics module consumes canonical core
records. - Differential tests pass. - Reports have clear definitions and
units.

### Milestone 3 --- Strategy and scenario expansion

**Owner:** Sarthak, with contributions from Maulshree and Pratyaksh.

Tasks:

-   Pit-loss sensitivity sweep.
-   Tire parameter sensitivity.
-   Alternative strategy exploration.
-   Pit-window analysis.
-   Simplified undercut/overcut comparison.
-   Clean-air versus overtaking-enabled scenario comparison.

**Exit criteria:** All scenarios are deterministic and independently
evaluated.

### Milestone 4 --- CLI integration

**Owner:** Sarthak coordinates; all module owners provide command
handlers or public APIs.

Tasks:

-   Add command groups.
-   Preserve existing flags.
-   Add help text and input validation.
-   Add readable reports.
-   Add exit-status handling for invalid and infeasible cases.

**Exit criteria:** All agreed capabilities are accessible through the
CLI and existing usage remains compatible.

### Milestone 5 --- Benchmarks and full validation

**Owners:** All members; Sarthak coordinates integration.

Tasks:

-   Add benchmarks for each new structure and algorithm.
-   Run the full regression suite.
-   Run seeded differential tests.
-   Verify deterministic repeated runs.
-   Review all complexity claims.
-   Perform a clean build from the documented instructions.

**Exit criteria:** No regression failures; benchmark and test outputs
are reproducible.

### Milestone 6 --- Documentation and final demonstration

**Owners:** All members.

Tasks:

-   Update README.
-   Update PRD or technical specification.
-   Add algorithm explanations and complexity tables.
-   Document CLI examples.
-   Document team contributions.
-   Prepare small hand-worked examples.
-   Prepare one full race demonstration.

**Exit criteria:** A new contributor can build the project, run tests,
execute the CLI examples, and understand the major algorithms.

------------------------------------------------------------------------

## 13. Definition of Done

A feature is not complete merely because it compiles.

Every feature must satisfy all applicable conditions:

-   [ ] C++17 implementation is complete.
-   [ ] Public interface is documented.
-   [ ] Real use case in the race-strategy domain is demonstrated.
-   [ ] Time complexity is documented.
-   [ ] Space complexity is documented.
-   [ ] Unit tests are added.
-   [ ] Differential or reference tests are added where possible.
-   [ ] Edge cases are covered.
-   [ ] Determinism is preserved.
-   [ ] Floating-point comparisons use named tolerances where
    applicable.
-   [ ] CLI access is implemented if the feature is user-facing.
-   [ ] Existing regression tests pass.
-   [ ] Code review is complete.
-   [ ] README or technical documentation is updated.

------------------------------------------------------------------------

## 14. Final Acceptance Criteria

The expanded Box Box backend will be considered complete when:

### Existing engine

-   The current DP optimizer works without regression.
-   Prefix-sum stint-cost calculation remains O(1).
-   The simulator remains consistent with the optimizer.
-   Loop closure continues to pass within 1 microsecond.
-   The custom binary min-heap leaderboard works.
-   The deterministic overtaking rule works.
-   The original CLI options remain supported.

### New data structures

-   Segment Tree is implemented and tested.
-   Fenwick Tree is implemented and tested.
-   Monotonic Deque is implemented and tested.
-   Event Priority Queue is implemented and tested.
-   Hash-map-based aggregation is documented and tested.

### New analytics

-   Tire degradation profiles are available.
-   Range analytics are available.
-   Driver statistics are available.
-   Fastest-K and slowest-K queries are available.
-   Sliding-window lap analytics are available.
-   Pit-stop and stint reports are available.
-   Event logs are deterministic.
-   Strategy comparison and sensitivity analysis are available.
-   Bounded alternative strategy exploration is available, with its
    limitations documented.

### Engineering quality

-   All algorithms have honest complexity analysis.
-   All new structures have reference-based tests.
-   Benchmarks are reproducible and separate from correctness tests.
-   The full project builds with the agreed warning flags.
-   The CLI exposes the agreed feature set.
-   Documentation explains how to build, test, and run the project.

------------------------------------------------------------------------

## 15. Final Contribution Summary

  -----------------------------------------------------------------------
  Member                  Primary ownership       Main DSA contribution
  ----------------------- ----------------------- -----------------------
  **Maulshree**           Tire Intelligence and   Segment Tree, Fenwick
                          Range Analytics         Tree, Prefix Sum reuse,
                                                  Binary Search

  **Pratyaksh**           Race Event Engine and   Custom Priority Queue,
                          Pit Analytics           Event Processing,
                                                  Interval Analysis,
                                                  Sorting

  **Sarthak**             Strategy Optimization   Dynamic Programming,
                          and Integration         DAG Path Exploration,
                                                  Backtracking,
                                                  Sensitivity Analysis

  **Mantra**              Driver and Lap          Hash Maps, Monotonic
                          Analytics               Deque, Heap-based
                                                  Top-K, Sliding Windows
  -----------------------------------------------------------------------

### Shared contribution expectation

Every member contributes implementation, tests, code review,
integration, and documentation within their assigned module. All four
members participate in final system validation and must be able to
explain the overall architecture and the algorithms they own.

------------------------------------------------------------------------

## 16. Project Principle

**Box Box must remain an algorithm-first C++17 project.**

Every new feature must answer three questions:

1.  What race-strategy or race-analysis problem does it solve?
2.  Which algorithm or data structure makes the solution efficient or
    useful?
3.  How do we prove that the implementation is correct?

If a feature cannot answer these questions, it should not be added
merely to make the project larger.

The final result should be a deterministic, well-tested C++17
race-strategy and analytics engine with meaningful DSA implementations,
clear complexity analysis, a usable CLI, and independently verifiable
results.
