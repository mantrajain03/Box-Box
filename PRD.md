# PRD — Undercut

**F1 Pit-Stop & Race Strategy Scheduler**
Repository: `Box-Box` · Language: C++17 · Status: in development

---

## 1. Overview

Undercut computes the *optimal pit-stop schedule* for a single-class motor race and
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
  the named tolerances in `include/undercut/Common.hpp`.

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
| `Driver` | Number, name, assigned plan, current compound, tire age, cumulative time. |
| `Leaderboard` | Min-heap over cumulative race time; ordered standings. |
| `RaceSimulator` | Advances the race one lap at a time over all drivers. |

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
| **FR-9** | **Overtaking (optional).** A deterministic gap-threshold overtake rule. | Fully deterministic — identical input gives identical results across runs; disabled by default; the exact rule is agreed before implementation. | Not started (rule undecided) |

### 4.1 CLI requirements

| ID | Requirement | Status |
| --- | --- | --- |
| CLI-1 | Configure race length, pit loss, track name and sector times from the command line. | **Done** |
| CLI-2 | Configure the compound set from the command line, replacing the built-in default set. | **Done** |
| CLI-3 | Print the optimal plan: stints, compounds, stop laps, green/pit/total time. | **Done** |
| CLI-4 | Print the stint-cost table on request, so a small case can be checked by hand. | **Done** |
| CLI-5 | Reject malformed input with a clear message and a non-zero exit status. | **Done** |
| CLI-6 | Replay the race from the optimizer's plan and report the FR-8 check, exiting 3 on failure. | **Done** |
| CLI-7 | Add reference cars on single-compound plans, and log standings every N laps. | **Done** |

---

## 5. Non-functional requirements

| ID | Requirement |
| --- | --- |
| **NFR-1** | **Zero dependencies.** Core and CLI use only the C++17 standard library. No external libraries, no package manager, no vendored code in the default build. A GUI may vendor source under `third_party/`, but only the separate `make gui` target may touch it — the default build and `make test` must never depend on it. |
| **NFR-2** | **Honest complexity comments.** Every complexity annotation states what the code actually costs. A leaderboard rebuilt each lap is documented at its true per-lap cost, not at the cost of one heap operation. |
| **NFR-3** | **Independently usable core types.** `Track`, `PitStrategy` and `Leaderboard` are each constructible, usable and testable without a `RaceSimulator` existing. |
| **NFR-4** | **Zero warnings.** Clean compile under `-std=c++17 -Wall -Wextra`. |
| **NFR-5** | **Determinism.** Identical input produces byte-identical output. Nothing consults the clock, the environment, an uninitialised value, or an unseeded RNG. DP ties break by a documented, stable rule. |
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
| Lap advance | `RaceSimulator::advanceLap` | `O(D)` per lap, `O(N·D)` for a race. Each driver's lap is one `lapDelta` call, `O(1)`. |
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
bin/undercut --laps 50 --pit-loss 22.0
bin/undercut --laps 50 --rivals --lap-log 10
bin/undercut --laps 4 --pit-loss 5 --sector 10 \
             --compound A:0:1:3 --compound B:2:0:4 --show-table --simulate
```

Exit status: `0` success, `1` bad usage, `2` no feasible strategy, `3` FR-8 failed.

---

## 8. Delivery phases

| Phase | Content | State |
| --- | --- | --- |
| 0 | Repository scaffold, PRD, Makefile, `Track`, `TireCompound`, prefix-sum table, CLI. | Complete |
| 1 | FR-1 optimizer: DP fill and backtrack. | Complete |
| 2 | FR-5, FR-6 simulation fidelity: `Driver`, `RaceSimulator`. | Complete |
| 3 | FR-7, FR-8 loop closure, verified non-vacuous by fault injection. | Complete |
| 4 | FR-2 min-heap leaderboard and NFR-4 zero warnings done. **FR-9 overtaking deliberately not implemented** — the rule needs approval first. | Partial |
| 5 | `make test` C++ assert harness: 1664 checks, brute-force DP reference, fault-injection verified. | Complete |
| 6 | GUI. | Pending |

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
| Edge cases | 0 laps, 1 lap, no planned stops, a stop before the final lap, single-sector track, zero-lap stints rejected at both entry points. |
| FR-2 | Heap order matches a brute-force sort over 30 seeded fields, via both heapify and repeated push; `takeTop` is a prefix of the full order; empty-heap access throws. |
| NFR-5 | Repeated `optimize()` and repeated races are identical; `reset()` restores the grid exactly. |

The harness was itself checked by fault injection: removing the right-child
comparison from `siftDown` produced 108 failures, and charging a pit stop after
the final stint produced 295. Both were reverted.

## 9. Open questions

| # | Question | State |
| --- | --- | --- |
| 1 | Does `compound.basePace` replace or add to the track's base lap time? | **Resolved — additive** (§3.2). Per-track recalibration deferred. |
| 2 | What exactly is the FR-9 overtake rule? | **Open.** To be proposed and approved before Phase 4. |
| 3 | GUI route: Dear ImGui + GLFW vendored under `third_party/`, or an ANSI-escape terminal UI? | **Open.** Machine probed 2026-09-18: **GLFW absent** (no headers, no `libglfw3.a`, no DLL). **OpenGL present** — `GL/gl.h`, `GL/glext.h`, `KHR/khrplatform.h` in `ucrt64`, and `-lopengl32 -lgdi32` links. So GLFW would have to be vendored as source, or an ImGui Win32 backend used instead. |
| 4 | Should the cliff model stay, or should degradation be purely linear? | **Open.** Cliff implemented (§3.2); set `cliffLap < 0` to disable per compound. |
