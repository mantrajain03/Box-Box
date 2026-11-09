# PRD — Box Box

**F1 Pit-Stop & Race Strategy Scheduler**
Repository: `Box-Box` · Language: C++17 · Status: in development

---

## 1. Overview

Box Box computes the *optimal pit-stop schedule* for a single-class motor race and
then replays that race lap by lap to confirm the schedule is achievable.

A race is `N` laps. A car's lap time depends on the circuit, on which tire compound it
is running, and on how many laps old that tire is. Tires get slower as they age, so a
car wants fresh rubber — but every pit stop costs a fixed amount of time in the pit
lane. The strategic question is therefore: **how many stops, on which lap, on which
compound?**

That question is a shortest-path problem over lap counts, which this project solves
with dynamic programming over a precomputed prefix-sum table of tire-age costs.

The project is a Data Structures & Algorithms exercise. The algorithms are the point;
the F1 setting is what makes them concrete.

### 1.1 Two halves that must agree

| Half | What it does |
| --- | --- |
| **Optimizer** (`PitStrategy`) | Given a track and a tire set, computes the minimum-time stint plan by DP. |
| **Simulator** (`RaceSimulator`) | Given drivers and their plans, replays the race lap by lap and produces standings. |

These two are written independently but **must produce the same number**. Driving one
simulated car from the optimizer's own plan, with no other cars to interfere, must
reproduce the optimizer's total time exactly (within tolerance). That cross-check is
FR-8 and it is the single most important correctness property in the project: it is
the only thing that proves the cost model the optimizer minimises is the cost model
the race actually obeys.

---

## 2. Scope

### 2.1 In scope

- Deterministic tire-degradation and pit-loss modelling.
- Optimal stint planning by dynamic programming.
- Lap-by-lap race simulation with multiple drivers.
- Live standings via a min-heap leaderboard.
- An optional, deterministic overtaking rule.
- A command-line frontend, and later a GUI frontend, both thin.

### 2.2 Non-goals (explicit)

The following are **out of scope** and must not be added:

- Weather, changing track conditions, wet compounds.
- Safety cars, virtual safety cars, red flags, penalties.
- Telemetry ingestion or any real-world data feed.
- Networking of any kind.
- Persistence — no databases, no save files.
- Stochastic race modelling. Nothing may be random. Any future probabilistic
  feature must take an explicit caller-supplied seed.
- Fuel load, ERS/battery deployment, DRS trains, tire warm-up/blankets.

---

## 3. Domain model

### 3.1 Units and conventions

- **All times are in seconds**, as `double`, throughout core, CLI and GUI.
- **Laps are 1-based** in anything user-facing (`lap 1` is the first racing lap).
- **Tire age is 0-based**: the first lap on a fresh set is run at age `0`, the second
  at age `1`. A stint of length `L` therefore runs ages `0 … L-1`.
- A pit stop is recorded as *"at the end of lap k"*. A stop at the end of lap `k`
  means laps `1…k` were the outgoing stint and lap `k+1` starts the next one.
- Floating-point values are **never** compared with `==`. All comparisons go through
  the named tolerances in `include/boxbox/Common.hpp`.

### 3.2 The lap-time cost model

This is the single most important definition in the document, because the optimizer
and the simulator must both implement it *identically* or FR-8 can never pass.

```
lapTime(compound, tireAge) = track.baseLapTime()
                           + compound.basePace
                           + compound.degradationPerLap * tireAge
                           + cliffPenalty(compound, tireAge)
```

where

```
cliffPenalty(c, age) = 0                                          if age <= c.cliffLap
                     = c.cliffDegradationPerLap * (age - c.cliffLap)   otherwise
```

and a compound with `cliffLap < 0` has no cliff.

**Decision — `basePace` is additive, not a replacement.** `track.baseLapTime()` is the
sum of the track's sector base times and represents a notional reference lap.
`compound.basePace` is a **signed per-lap offset** on top of it: the soft tire is
faster than reference (`basePace < 0`), the hard tire slower (`basePace > 0`). The
consequence is that a compound set is track-independent and can be reused across
circuits. Re-calibrating compound figures per circuit is deferred to a later scale-up
and is **not** a requirement now.

The term `compound.basePace + degradation*age + cliffPenalty` — everything except the
track's contribution — is what the code calls `TireCompound::lapDelta(age)`.

**Cliff rationale.** Real tires do not degrade linearly; past a threshold they fall
away sharply. Modelling this keeps the prefix-sum table load-bearing (a purely linear
model has a closed form and would make the table decorative) and it gives `maxStintLaps`
real meaning. The cliff is deterministic and adds two numbers per compound.

### 3.3 Stint cost

A stint of `L` laps on compound `c`, excluding pit loss, costs

```
stintCost(c, L) = L * track.baseLapTime() + SUM(a = 0 .. L-1) lapDelta_c(a)
```

The sum is answered in **O(1)** from a prefix-sum table built once per `PitStrategy`:

```
ageCostPrefix[c][L] = SUM(a = 0 .. L-1) lapDelta_c(a)      for L = 0 … maxStintLaps(c)
```

`stintCost(c, L)` is defined as unreachable (`+inf`) when `L > c.maxStintLaps`.

### 3.4 Race cost

For a plan of stints `s1 … sm` covering exactly `N` laps:

```
totalTime = SUM(stintCost(s_i)) + (m - 1) * track.pitLoss()
```

A plan with `m` stints makes `m - 1` stops. There is no stop before the first stint
(the car starts on its first set) and none after the last (the race has ended).

### 3.5 Entities

| Entity | Responsibility |
| --- | --- |
| `Track` | Name, lap count, pit-lane time loss, ordered sectors. `baseLapTime()` is the sum of sector base times. |
| `TireCompound` | Name, `basePace` offset, degradation per lap, cliff lap + cliff rate, `maxStintLaps`. Provides `lapDelta(age)`. |
| `PitStrategy` | Owns the prefix-sum table and the DP optimizer. Produces a `StrategyResult`. |
| `StrategyResult` | Feasibility, stint list, pit laps, green time, pit time, total time, raw `dp[N]`. |
| `Driver` | Number, name, assigned plan, current compound, tire age, cumulative time, time lost to traffic (FR-9). |
| `Leaderboard` | Min-heap over cumulative race time; ordered standings. |
| `RaceSimulator` | Advances the race one lap at a time over all drivers, optionally under the FR-9 overtaking rule. |
| `OvertakeRule` | FR-9 settings: on/off, minimum following gap, pass margin (§3.6). Off by default. |

### 3.6 Overtaking (FR-9)

Agreed 2026-09-19. The rule is **off by default**; with it off, cars are independent and
never interact, exactly as in §3.2–§3.4.

| Parameter | Default | Meaning |
| --- | --- | --- |
| `minGap` (δ) | 0.5 s | Closest a car may cross the line behind the car ahead. Must be finite and at least `kMinOvertakeGap` (1 ms). |
| `passMargin` (Δ) | 1.0 s | Lap-time advantage over the car ahead needed to get past it. Must be finite and not negative. |

Each lap:

1. Take the running order at the start of the lap from the `Leaderboard` heap. At the
   start every car's time is 0, so the grid is in car-number order.
2. Compute each car's own lap time `L` from §3.2, unchanged. The rule never changes a
   car's pace, only when it crosses the line.
3. Walk that order from P1 down. Each car B is compared with the car A directly ahead of
   it at the start of the lap, whose crossing time `C_A` has already been settled:

   ```
   arrival = T_B + L_B                     T_B = B's time at the start of the lap
   if arrival < C_A + δ                    B has caught A
       if L_A - L_B >= Δ   pass:   C_B = arrival
       else                held:   C_B = C_A + δ
   else                    clear:  C_B = arrival
   ```

   Comparisons go through `definitelyLess`, never `==`: a car arriving exactly δ behind
   is clear, and an advantage of exactly Δ is enough to pass.
4. Pit loss is added after the crossing is settled. The pit lane is not raced.

Properties:

- **Deterministic** — a fixed processing order, tolerance-based comparisons, no randomness.
- **Traffic only costs time.** A held car's extra time is recorded as
  `Driver::trafficLoss()`, and its race time is its own plan's time plus that loss,
  to within `kClosureTolerance`.
- **A lone car is never held up**, so FR-8 still closes with the rule on.
- **Cars on different lap counts never interact**, so lapped traffic is not modelled.
- **Known simplification:** a car that passes is not re-checked against cars further up
  the road on the same lap.

Why δ has a 1 ms floor: a held car crosses exactly δ behind the car ahead. A gap small
enough to round away at race-length times would leave a dead heat, which the car-number
tie-break could settle in the held car's favour.

---

## 4. Functional requirements

Status values: **Done** · **In progress** · **Not started**.

| ID | Requirement | Acceptance criteria | Status |
| --- | --- | --- | --- |
| **FR-1** | **Strategy optimizer.** DP over lap count with backtracking, using the prefix-sum table, returning the minimum-time stint plan. | `dp[N]` is finite for any solvable race; returned stint lengths sum to **exactly** `N`; the result matches a 3–5 lap case computed by hand. | **Done** |
| **FR-2** | **Leaderboard.** Live standings backed by a real min-heap keyed on cumulative race time, not a sort call on a vector. | Ordering matches a brute-force sort on the same data for randomised-but-seeded inputs; `pop` is O(log n) and the code comment says so truthfully. | **Done** |
| **FR-3** | **Track model.** Named circuit with an ordered sector list, a derived base lap time, a lap count and a pit-lane loss. | `baseLapTime()` equals the sum of sector base times; a single-sector track behaves correctly; usable with no `RaceSimulator` in existence. | **Done** |
| **FR-4** | **Tire compound model.** Per-compound pace offset, degradation, cliff and maximum stint length, exposing `lapDelta(age)`. | `lapDelta` matches §3.2 by hand at ages either side of the cliff; `maxStintLaps` is enforced by the optimizer. | **Done** |
| **FR-5** | **Lap-time fidelity.** The simulator computes each driver's lap from that driver's *actual* compound and *actual* tire age, per §3.2 — never a flat track lap time. | Two drivers on different compounds, over identical laps, diverge in cumulative time. | **Done** |
| **FR-6** | **Compound switching.** At each scheduled stop a driver takes on the next stint's compound and tire age resets to 0. | After a stop the driver reports the new compound and age 0; the pit loss is applied exactly once. | **Done** |
| **FR-7** | **Plan-driven simulation.** At least one simulated driver is driven from a `StrategyResult` rather than hard-coded stop laps. | The driver's stops occur on the laps the optimizer chose, with no stop laps written by hand anywhere. | **Done** |
| **FR-8** | **Loop closure.** Optimizer and simulator agree. | In a single-driver, no-overtake race, that driver's final cumulative time equals `StrategyResult::totalTime` within `kClosureTolerance`. **The tolerance must not be loosened to make this pass** — a failure here is a real bug. | **Done** |
| **FR-9** | **Overtaking (optional).** A deterministic gap-threshold overtake rule. | Fully deterministic — identical input gives identical results across runs; disabled by default; the exact rule is agreed before implementation. Agreed rule: §3.6. | **Done** |

### 4.0 Backend expansion (agreed in `PROJECT DEVELOPMENT PLAN.md`)

Added after FR-1 to FR-9 shipped. Each reuses `PitStrategy`, the shared lap-time model and
the simulator's records; none re-implements the cost model.

| ID | Requirement | Acceptance criteria | Status |
| --- | --- | --- | --- |
| **FR-10** | **Custom strategy evaluation.** Cost a plan entered by the user, with a reason when it is invalid. | `validatePlan` names the first failing condition; totals match a lap-by-lap sum over every plan of 60 seeded races; the distance from the optimum is reported in seconds and per cent. | **Done** |
| **FR-11** | **Sensitivity sweeps.** Optimal plan as pit loss, or one compound parameter, varies. | Each row equals an independent `optimize()` call; bad ranges are rejected; sweeps are capped at 1000 points. | **Done** |
| **FR-12** | **Alternative strategies.** The K cheapest distinct plans, cheapest first. | Equal to exhaustive enumeration on small races; entry 0 is the optimizer's plan; distinct; ordered; reproducible; K capped at 50 and the cap is stated to the user. | **Done** |
| **FR-13** | **Strategy comparison.** Optimum, single-compound plans, best plan per exact stop count and user plans under one cost model, including infeasible ones with a reason. | `bestForStopCount` equals enumeration; the minimum over stop counts equals the optimum. | **Done** |
| **FR-14** | **Clean air versus traffic.** The same field run with overtaking off and on. | With the rule off every car's time equals its plan's total within `kClosureTolerance`; traffic loss is never negative; positions are a permutation. | **Done** |
| **FR-15** | **Analytics modules.** Tire profiles and range queries (segment and Fenwick trees), driver analytics (hash map, bounded heap, monotonic deque), the race-event engine and pit analytics. | Each optimized query matches a brute-force reference in `make test`. See `docs/TIRE_MODULE.md`, `docs/DRIVER_ANALYTICS.md`, `docs/EVENT_ENGINE.md`. | **Done** |

### 4.1 CLI requirements

| ID | Requirement | Status |
| --- | --- | --- |
| CLI-1 | Configure race length, pit loss, track name and sector times from the command line. | **Done** |
| CLI-2 | Configure the compound set from the command line, replacing the built-in default set. | **Done** |
| CLI-3 | Print the optimal plan: stints, compounds, stop laps, green/pit/total time. | **Done** |
| CLI-4 | Print the stint-cost table on request, so a small case can be checked by hand. | **Done** |
| CLI-5 | Reject malformed input with a clear message and a non-zero exit status. | **Done** |
| CLI-6 | Replay the race from the optimizer's plan and report the FR-8 check, exiting 3 on failure. The check always runs on a solo replay, so rivals and traffic cannot affect it. | **Done** |
| CLI-7 | Add reference cars on single-compound plans, and log standings every N laps. | **Done** |
| CLI-8 | Switch on and tune the FR-9 overtaking rule (`--overtake`, `--min-gap`, `--pass-margin`), and show each driver's time lost to traffic. Output is unchanged when the rule is off. | **Done** |
| CLI-9 | `tires` command group: degradation profile, range and threshold queries. | **Done** |
| CLI-10 | Race analytics flags: `--events`, `--pit-report`, `--race-laps`, `--driver-stats`, `--top-laps` (with `--top-laps-car` and `--top-laps-range`), `--lap-window`. Each implies `--simulate`; output is unchanged without them. | **Done** |
| CLI-11 | `strategy` command group: `evaluate`, `compare`, `alternatives`, `sensitivity`, `stops`, `traffic`. Numbers validated and finite; an unrunnable user plan exits 2 with its reason; nothing is printed before the input is accepted. | **Done** |

---

## 5. Non-functional requirements

| ID | Requirement |
| --- | --- |
| **NFR-1** | **Zero dependencies.** Core and CLI use only the C++17 standard library. No external libraries, no package manager, no vendored code in the default build. A GUI may vendor source under `third_party/`, but only the separate `make gui` target may touch it — the default build and `make test` must never depend on it. |
| **NFR-2** | **Honest complexity comments.** Every complexity annotation states what the code actually costs. A leaderboard rebuilt each lap is documented at its true per-lap cost, not at the cost of one heap operation. |
| **NFR-3** | **Independently usable core types.** `Track`, `PitStrategy` and `Leaderboard` are each constructible, usable and testable without a `RaceSimulator` existing. |
| **NFR-4** | **Zero warnings.** Clean compile under `-std=c++17 -Wall -Wextra`. |
| **NFR-5** | **Determinism.** Identical input produces byte-identical output. Nothing consults the clock, the environment, an uninitialised value, or an unseeded RNG. DP ties break by a documented, stable rule. **One exception, approved 2026-11-06:** the benchmark binary built by `make bench` reads `std::chrono::steady_clock` to time runs and for nothing else (no seeding, no code path chosen by time); the core, CLI and tests never read the clock, and a timing is never a pass or fail check. |
| **NFR-6** | **One build entry point.** The `Makefile` is the only way the project is built. No helper scripts in any language — every executable the project produces is a C++ program. |

---

## 6. Data structures and complexity

Let `N` = race laps, `C` = number of compounds, `M` = largest `maxStintLaps`,
`D` = number of drivers.

| Structure | Where | Cost |
| --- | --- | --- |
| Prefix-sum table `ageCostPrefix[c][L]` | `PitStrategy` | Build `O(C·M)` time, `O(C·M)` space. Query `stintCost` `O(1)`. |
| DP array `dp[0..N]` + `choice[0..N]` | `PitStrategy::optimize` | `O(N·C·M)` time, `O(N)` space. |
| Backtrack | `PitStrategy::optimize` | `O(N)` worst case (every stint one lap). |
| Binary min-heap | `Leaderboard` | `peek` `O(1)`, `push`/`pop` `O(log D)`, heapify constructor `O(D)`, `takeTop(k)` `O(D + k log D)`, `drainOrdered` `O(D log D)`. |
| Lap advance | `RaceSimulator::advanceLap` | `O(D)` per lap, `O(N·D)` for a race, with overtaking off. Each driver's lap is one `lapDelta` call, `O(1)`, and `Driver::finished()` is `O(1)` because the plan's lap total is summed once, at construction. |
| Lap advance, FR-9 on | `RaceSimulator::advanceLapWithTraffic` | **`O(D log D)` per lap** — `O(D)` heapify and `D` pops for the start-of-lap order, then one `O(D)` walk — so `O(N·D log D)` for a race. The order is rebuilt every lap because every car's key changes every lap. |
| Standings | `RaceSimulator::standings` | **`O(D log D)` per call** — `O(D)` heapify then `D` pops. Called once per lap that is `O(N·D log D)` over a race. A heap does **not** make a *full* ordering cheaper than sorting, and the comment says so. |
| Top-k standings | `RaceSimulator::topK` | `O(D + k log D)`. This is where the heap actually beats a sort: the podium needs no full ordering. Every driver's key changes every lap, so no incremental scheme would help. |
| Plan costing | `PitStrategy::buildResult` | `O(S)` for an `S`-stint plan. Shared by `optimize()` and `evaluatePlan()`. |

### 6.1 DP recurrence

```
dp[0] = 0
dp[l] = min over compounds c,
        over stint lengths k in [1, min(l, maxStintLaps(c))],
        of   dp[l-k] + stintCost(c, k) + (l-k > 0 ? pitLoss : 0)
dp[l] = +inf if no transition is reachable
```

`choice[l]` records the `(c, k)` that achieved `dp[l]`, which is what backtracking
walks to rebuild the plan.

**Tie-breaking (NFR-5).** Candidates are enumerated in compound declaration order,
then ascending stint length, and a candidate replaces the incumbent only if it is
better by more than `kTimeEpsilon`. The first candidate among near-equal ones
therefore wins, which makes the chosen plan stable across runs and platforms.

---

## 7. Build and run

```
make            # core library + CLI (default; dependency-free)
make test       # build and run the C++ assert harness
make gui        # optional GUI frontend                   [Phase 6]
make clean
make help
```

`make` does **not** build the tests, and `make test` does **not** build the CLI or
the GUI: the harness links against the core objects only.

On Windows with MSYS2 the make binary is `mingw32-make`. The Makefile detects both
cmd.exe and POSIX shells and asks the compiler for its target triple rather than
trusting `%OS%`, so the same file works from PowerShell, MSYS2 bash and Linux.

Example:

```
bin/boxbox --laps 50 --pit-loss 22.0
bin/boxbox --laps 50 --rivals --lap-log 10
bin/boxbox --laps 50 --rivals --overtake --lap-log 10
bin/boxbox --laps 4 --pit-loss 5 --sector 10 \
             --compound A:0:1:3 --compound B:2:0:4 --show-table --simulate
```

Exit status: `0` success, `1` bad usage, `2` no feasible strategy (or a user-entered
`strategy` plan that cannot be run), `3` FR-8 failed (or the event log failed its
state-machine check).

---

## 8. Delivery phases

| Phase | Content | State |
| --- | --- | --- |
| 0 | Repository scaffold, PRD, Makefile, `Track`, `TireCompound`, prefix-sum table, CLI. | Complete |
| 1 | FR-1 optimizer: DP fill and backtrack. | Complete |
| 2 | FR-5, FR-6 simulation fidelity: `Driver`, `RaceSimulator`. | Complete |
| 3 | FR-7, FR-8 loop closure, verified non-vacuous by fault injection. | Complete |
| 4 | FR-2 min-heap leaderboard, FR-9 "pass or hold" overtaking (rule agreed 2026-09-19, §3.6) and NFR-4 zero warnings. NFR-2 audit fix: `Driver::finished()` re-summed the plan on every call (`O(S)`), which made the `O(1)` / `O(D)` claims above untrue; it is now `O(1)`. | Complete |
| 5 | `make test` C++ assert harness: 3311 checks, brute-force DP reference, fault-injection verified. | Complete |
| 5a | Backend expansion (§4.0): tire intelligence, driver analytics, event engine, strategy lab (FR-10 to FR-15), `make bench`. | Complete |
| 6 | GUI: Dear ImGui with its Win32 + OpenGL3 backends (§9, question 3). | Pending |

---

## 8.1 Test coverage

`make test` builds `tests/test_main.cpp` against the core objects and runs it.
The harness is hand-written — no framework, since that would breach NFR-1 — and a
failing check records itself and continues, so one run reports every problem
rather than only the first. A non-zero exit fails the target.

| Area | What is checked |
| --- | --- |
| `Track` | Sector sum, single sector, no sectors, zero laps, mutators, every validation path. |
| `TireCompound` | `lapDelta` either side of the cliff, negative-age clamping, validation, default-set ordering. |
| Prefix table | Table agrees with summing `lapDelta` the slow way for every compound and length; past `maxStintLaps` is unreachable, not expensive. |
| FR-1 vs hand | The 4-lap worked case in §7, stint by stint. |
| FR-1 vs brute force | 40 seeded configurations compared against exhaustive enumeration written without reference to the DP. |
| FR-1 invariants | 60 configurations: stints tile the race exactly, stops = stints − 1, pit laps strictly inside the race. |
| FR-1 extremes | Zero pit loss gives one stint per lap on the quickest fresh compound; an unaffordable stop collapses to the fewest legal stints, or none. |
| FR-5 / FR-6 | Lap times track the fitted compound and age; compound switches and age resets at the stop. |
| FR-7 / FR-8 | 60 seeded races driven from `StrategyResult`, each closing within `kClosureTolerance`. |
| Edge cases | 0 laps, 1 lap, no planned stops, a stop before the final lap, single-sector track, zero-lap stints rejected at both entry points. A stop on lap 0 and a stop after the final lap, each with a correct lap total so that only the zero-lap guard can reject it, at all three entry points. |
| FR-2 | Heap order matches a brute-force sort over 30 seeded fields, via both heapify and repeated push; `takeTop` is a prefix of the full order; empty-heap access throws. |
| NFR-5 | Repeated `optimize()` and repeated races are identical; `reset()` restores the grid exactly. |
| FR-9 settings | Off by default, with the documented defaults; every invalid `minGap` / `passMargin` rejected, and a rejected rule leaves the previous one in place. |
| FR-9 hand cases | Six 3-lap races on flat compounds: equal pace from the grid, held by a slower car, a clear pass, exactly the pass margin, the pit lane not raced, cars on different laps. |
| FR-9 properties | 30 seeded fields: with the rule off every car runs exactly its own plan; with it on, traffic never makes a car faster and the extra time equals `trafficLoss()` within `kClosureTolerance`; results repeat exactly, including after `reset()`; a lone car still closes FR-8. |

The expansion modules add their own files (`test_tire_module.cpp`,
`test_driver_analytics.cpp`, `test_events.cpp`, `test_strategy_analysis.cpp`), run by the
same `make test` and totalled together: 254,839 checks at the time of writing.

| Area | What is checked |
| --- | --- |
| FR-10 | Every validation reason; the 4-lap hand case (A2+B2 = 50, B4 = 48, A2+A2 = 47); totals equal a lap-by-lap sum over every plan of 60 seeded races. |
| FR-11 | 20 seeded pit-loss sweeps and a compound sweep, each row against an independent `optimize()`; zero and huge pit loss; every rejected range. |
| FR-12 | 60 seeded races: the K cheapest plans against exhaustive enumeration for K = 1, 4, 12, 50; distinct, ordered, reproducible; rank 0 is the optimizer's plan. |
| FR-13 | `bestForStopCount` against enumeration for every stop count, including infeasible ones; `compoundOnlyBest` against a one-compound `PitStrategy`. |
| FR-14 | Clean-air totals close against the plans (FR-8 per car); traffic only costs time; a disabled rule changes nothing; a lone car is never held. |
| Events | Queue pop order equals a full sort; ordering is a strict weak order; every illegal state transition rejected; logging never changes race times. |
| Drivers | Aggregates against direct calculation; top-K against a full sort; windows against brute force; deterministic ties. |
| Tires | Both trees against brute force; every cliff boundary; binary search only on a profile that never falls. |

Fault injection on the strategy lab (2026-11-10, throwaway copies, none in the repo):
reversing the K-best order, adding one second to the swept pit loss, dropping the
single-stint guard from the stop-count DP, keeping the optimum's duplicate in the
alternatives, an off-by-one in the maximum stint, flipping the sign of the delta and
zeroing the traffic loss were each caught (the first and third by aborting the run, the
others by 5 to 2,139 failed checks). One mutant, charging the pit loss on a plan's single
stint in the stop-count DP, survived: it adds the same constant to every plan with the
same stop count, so it changes no choice and no reported time. It is equivalent, not a gap.

The harness was itself checked by fault injection: removing the right-child
comparison from `siftDown` produced 108 failures, and charging a pit stop after
the final stint produced 295. Both were reverted.

A second round on 2026-09-19 built nine one-line mutants in throwaway copies, and
all nine were caught. Deleting the zero-lap guard from `buildResult`, which the
suite had previously missed, now fails 2 checks. Removing the FR-9 same-lap
guard, requiring more than `passMargin` to pass, and letting pit loss leak into
the crossing time each fail 2 checks, in the hand case aimed at them. Holding a
car without the gap failed 127 checks, not clearing traffic loss in `reset()` 106,
dropping the traffic delay from race time 120, applying the rule while switched
off 347, and never summing the cached lap total 691.

## 9. Open questions

| # | Question | State |
| --- | --- | --- |
| 1 | Does `compound.basePace` replace or add to the track's base lap time? | **Resolved — additive** (§3.2). Per-track recalibration deferred. |
| 2 | What exactly is the FR-9 overtake rule? | **Resolved 2026-09-19 — "pass or hold"** (§3.6). |
| 3 | GUI route: Dear ImGui + GLFW vendored under `third_party/`, or an ANSI-escape terminal UI? | **Resolved 2026-09-19 — Dear ImGui with its own Win32 + OpenGL3 backends**, vendored as source under `third_party/imgui/`; no GLFW. Probed 2026-09-18 and again 2026-09-19: **GLFW absent** (no headers, no `libglfw3.a`, no DLL, no package). **OpenGL present**, and `-lopengl32 -lgdi32 -ldwmapi -limm32` link with GCC 13.2. This route is all C++, so the build needs no C compiler rule. The GUI will be Windows-only; the core, CLI and tests stay portable. |
| 4 | Should the cliff model stay, or should degradation be purely linear? | **Open.** Cliff implemented (§3.2); set `cliffLap < 0` to disable per compound. |
| 5 | Should `RaceLapRecorder` keep a Fenwick tree of per-lap traffic loss? | **Open.** Added 2026-11-10 to meet plan sec. 8.1 item 8; an earlier agreement with Mantra (2026-11-06) preferred none. Additive; nothing depends on it. |
| 6 | Pit-lane time is not modelled: pit entry, tyre change and exit share one timestamp, so no pit-lane duration or traffic-after-stop figure exists (plan sec. 4.5). | **Open.** Would need a deliberate extension to the simulator's stop model. |
