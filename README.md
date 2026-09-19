# Box-Box — Undercut

**An F1 pit-stop and race-strategy scheduler in C++17, using nothing but the standard library.**

*"Box, box"* is the radio call that brings a car into the pits. Undercut answers the
question behind that call: **how many stops, on which laps, on which tires?** It finds
the fastest plan by dynamic programming, then replays the race lap by lap to prove the
plan holds up.

This is a Data Structures & Algorithms project. The algorithms are the point; the F1
setting makes them concrete. The full specification is [`PRD.md`](PRD.md).

## What it does

- **Finds the optimal strategy by dynamic programming.** Choosing stints is a
  shortest-path problem over lap counts. A prefix-sum table answers the cost of any
  stint in O(1), so the whole search is O(N·C·M).
- **Simulates the race lap by lap.** Each car's lap time comes from its own compound
  and tire age. At each stop it takes the next compound on fresh tires.
- **Proves the two agree (loop closure).** A car driven by the optimizer's plan must
  finish in the optimizer's predicted time, to within 1 µs. The CLI checks this on every
  simulated run; the test suite checks it on 60 seeded races, plus 30 more with
  overtaking switched on.
- **Keeps live standings in a hand-written binary min-heap**, with complexity comments
  that state what the code really costs.
- **Optionally models overtaking** with a deterministic "pass or hold" rule: a car that
  catches the one ahead is held behind it unless it is clearly quicker.
- **Has no dependencies, compiles with no warnings, and is deterministic:** identical
  input gives byte-identical output.

## Build

You need a C++17 compiler and GNU Make — nothing else. Development uses GCC 13.2 from
MSYS2 (UCRT64) on Windows, where make is called `mingw32-make`:

```
mingw32-make                                  # core library + CLI  ->  bin/undercut.exe
mingw32-make test                             # build and run the test harness
mingw32-make run ARGS="--laps 50 --rivals"    # build, then run the CLI
mingw32-make clean                            # remove build/ and bin/
mingw32-make help                             # list targets; `info` shows the toolchain
```

The same Makefile runs from PowerShell and from bash (it detects which shell it has),
and is written for Linux `make` as well. The default target builds only the core and
the CLI; `make test` builds only the core and the test harness.

## Using the CLI

| Option | What it sets | Default |
| --- | --- | --- |
| `--laps N` | Race distance in laps | 50 |
| `--pit-loss SECONDS` | Time lost per pit stop | 22.0 |
| `--track NAME` | Circuit name | Generic Circuit |
| `--sector SPEC` | A sector, as `SECONDS` or `NAME:SECONDS`. Repeatable; replaces the default sectors | 28.4 / 31.2 / 30.4 |
| `--compound SPEC` | A tire compound, as `NAME:BASEPACE:DEG:MAXSTINT` or `NAME:BASEPACE:DEG:MAXSTINT:CLIFFLAP:CLIFFDEG`. Repeatable; replaces the default set | SOFT / MEDIUM / HARD |
| `--show-table` | Print the stint-cost table, so small cases can be checked by hand | off |
| `--simulate` | Replay the race and run the loop-closure check | off |
| `--rivals` | Add one reference car per compound, each running that compound only | off |
| `--lap-log N` | Print the standings every N laps | 0 (off) |
| `--overtake` | Switch on the overtaking rule | off |
| `--min-gap SECONDS` | Closest a car may follow across the line | 0.5 |
| `--pass-margin SECONDS` | Lap-time advantage needed to pass | 1.0 |

`--rivals`, `--lap-log` and the overtaking flags all imply `--simulate`; `--min-gap` and
`--pass-margin` also imply `--overtake`. `undercut --help` prints the full text.

Exit status: `0` success · `1` bad usage · `2` no feasible strategy · `3` loop-closure
check failed.

### The default race

`bin/undercut` plans a 50-lap race with a 22 s pit loss on the default compounds:

```
Optimal strategy
    #  compound      laps       lap range     stint time
    1  SOFT            17          1 - 17       1532.040
    2  SOFT            17         18 - 34       1532.040
    3  SOFT            16         35 - 50       1441.200

  Stops        2   end of lap 17, 34
  Green time   4505.280 s
  Pit time     44.000 s
  Total time   4549.280 s   = 1:15:49.280
  DP optimum   4549.280 s   delta 0.00e+00 s
  Laps planned 50 / 50   OK
```

### Checking the optimizer by hand

A 4-lap race is small enough to solve on paper:

```
bin/undercut --laps 4 --pit-loss 5 --sector 10 --compound A:0:1:3 --compound B:2:0:4 --show-table --simulate
```

Compound A costs nothing extra on fresh tires but loses 1 s per lap of age and lasts at
most 3 laps. Compound B is 2 s slower but never degrades. The table gives each stint's
cost:

```
Stint cost table   green-flag seconds, pit loss excluded
   laps             A             B
      1        10.000        12.000
      2        21.000        24.000
      3        33.000        36.000
      4             -        48.000
```

Working the DP forward: dp[1] = 10, dp[2] = 21 and dp[3] = 33, each a single stint on A.
For four laps, the candidates include A×2 then A×2 (21 + 21 + 5 = **47**), B×4 (48),
A×3 then A×1 (33 + 10 + 5 = 48) and A×1 then A×3 (48), so dp[4] = 47. The program agrees,
and the simulated car matches it exactly:

```
Optimal strategy
    #  compound      laps       lap range     stint time
    1  A                2           1 - 2         21.000
    2  A                2           3 - 4         21.000

  Stops        1   end of lap 2
  ...
Loop closure check   FR-8
  Optimizer total time                    47.000000000 s
  Driver 1 simulated total time           47.000000000 s
  Absolute difference                         0.00e+00 s
  Tolerance kClosureTolerance                 1.00e-06 s
  RESULT                                          PASS
```

### Racing rivals, with and without overtaking

`bin/undercut --laps 50 --rivals` adds one car per compound. On their own, the cars never
interact, and the optimal plan wins. (ALL-SOFT happens to run the same three-stint plan,
so it ties exactly and the lower car number takes P1.)

```
   pos   #  driver          compound    age  stops       race time          gap
     1   1  OPTIMAL         SOFT         16      2        4549.280            -
     2   2  ALL-SOFT        SOFT         16      2        4549.280        0.000
     3   3  ALL-MEDIUM      MEDIUM       25      1        4555.000        5.720
     4   4  ALL-HARD        HARD         25      1        4570.500       21.220
```

Add `--overtake` and traffic changes the result. The optimal car rejoins from its last
stop 7.2 s behind ALL-MEDIUM and closes in, but its advantage shrinks as its tires age.
It drops below the 1.0 s pass margin on lap 38, three laps before the car catches up.
From lap 41 it is held 0.5 s behind to the flag, and loses 6.62 s:

```
   pos   #  driver          compound    age  stops       race time          gap   traffic
     1   3  ALL-MEDIUM      MEDIUM       25      1        4555.400            -     0.400
     2   1  OPTIMAL         SOFT         16      2        4555.900        0.500     6.620
     3   2  ALL-SOFT        SOFT         16      2        4556.400        1.000     7.120
     4   4  ALL-HARD        HARD         25      1        4570.850       15.450     0.350
```

That is expected: the optimizer plans for a car in clean air. The loop-closure check
still passes, because it always replays the optimal plan on its own.

## How it works

### The lap-time model

Every lap time in the project comes from one formula (PRD §3.2):

```
lapTime(compound, age) = track.baseLapTime()
                       + compound.basePace
                       + compound.degradationPerLap * age
                       + cliffPenalty(compound, age)

cliffPenalty = compound.cliffDegradationPerLap * (age - compound.cliffLap)   once age > cliffLap
             = 0                                                             before that
```

- `baseLapTime()` is the sum of the track's sector times.
- `basePace` is a signed offset: negative for soft tires, positive for hard ones. That
  makes a compound set reusable on any circuit.
- Tire age starts at 0 on a fresh set. A stop "at the end of lap k" means lap k + 1
  starts the next stint.
- The cliff models the sharp fall-off of worn tires. A negative `cliffLap` turns it off.

### The optimizer (`PitStrategy`)

```
dp[0] = 0
dp[l] = min over compounds c and stint lengths k <= min(l, maxStintLaps(c)) of
        dp[l - k] + stintCost(c, k) + (l - k > 0 ? pitLoss : 0)
```

`stintCost(c, k)` is `k × baseLapTime` plus the sum of the compound's per-lap deltas
over ages 0 … k−1, read in O(1) from a prefix-sum table built once per compound. Walking
the recorded choices back from `dp[N]` rebuilds the plan. Candidates are tried in
compound order, then by stint length, and a new candidate wins only if it is better by
more than 1e-9 s — so ties always resolve the same way.

### The simulator (`RaceSimulator`, `Driver`)

The simulator advances every car one lap at a time using the same lap-time model. The
optimizer totals a race through prefix sums and the simulator adds it up lap by lap, so
when the two agree (FR-8), it shows the cost model the optimizer minimises is the one
the race actually obeys.

### Standings (`Leaderboard`)

A binary min-heap written by hand, ordered by laps completed, then race time, then car
number. Producing the full classification is O(D log D) per call — no cheaper than
sorting, and the code says so. The heap earns its place on the top-k query
(O(D + k log D)) and on the running order the overtaking rule rebuilds every lap.

### Overtaking (optional)

Each lap, walking the running order from the leader down, a car that would cross the
line within `minGap` of the car directly ahead is held exactly `minGap` behind it —
unless its own lap time is at least `passMargin` quicker, in which case it passes.

- **The pit lane is not raced:** pit loss is added after the crossing is settled.
- **Lapped traffic is not modelled:** cars on different laps never interact.
- **A lone car is never held up**, so the loop-closure check still holds with the rule on.

Every car's time lost to traffic is recorded and shown in the `traffic` column. The
exact rule is in PRD §3.6.

## Complexity

N = laps, C = compounds, M = longest stint allowed, D = drivers, S = stints in a plan.

| Operation | Cost |
| --- | --- |
| Build the prefix-sum table | O(C·M) |
| `stintCost` | O(1) |
| `optimize` | O(N·C·M) time, O(N) space |
| Backtrack | O(N) |
| `evaluatePlan` | O(S) |
| `advanceLap`, overtaking off | O(D) |
| `advanceLap`, overtaking on | O(D log D) |
| `standings` | O(D log D) |
| `topK(k)` | O(D + k log D) |

## Tests

`mingw32-make test` builds `tests/test_main.cpp` against the core alone and runs it:

```
== FR-9: properties over seeded fields

---------------------
3311 checks, 0 failed
OK
```

The harness is written by hand (a test framework would be a dependency). A failing
check is recorded and the run carries on, so one run reports every problem; any failure
exits non-zero. It covers, among other things:

- the DP against a hand-worked case, and against brute-force enumeration on 40 seeded
  configurations;
- stints tiling the race exactly, zero pit loss (one stint per lap on the quickest
  tire), and a huge pit loss (the fewest legal stops);
- compound switching, tire-age reset, and loop closure on 60 seeded races;
- edge cases: 0 laps, 1 lap, no stops, a stop on lap 0 or after the final lap, a
  single-sector track;
- the heap against a brute-force sort, and determinism across repeated runs;
- overtaking: six hand-computed races, plus 30 seeded fields checking that traffic only
  ever costs time, and that each car's extra time matches the traffic loss it records.

The tests are themselves checked by fault injection: deliberately broken copies of the
code must make them fail, and they do. PRD §8.1 lists the mutants and their failure
counts.

## Project layout

```
include/undercut/     public headers: Common, Track, TireCompound, PitStrategy,
                      Driver, Leaderboard, RaceSimulator
src/                  core library: standard library only, no knowledge of any frontend
cli/main.cpp          command-line frontend: parsing and printing, no algorithms
tests/test_main.cpp   hand-written assert harness
Makefile              the only build entry point
PRD.md                the specification
```

## Design rules

- **No dependencies.** The core, CLI and tests use only the C++17 standard library.
- **Deterministic.** Nothing reads the clock, the environment or an unseeded random
  generator. The tests vary their inputs with a fixed-seed generator.
- **No `==` on floating point.** Comparisons go through named tolerances in
  `Common.hpp`: `kTimeEpsilon` (1e-9 s) and `kClosureTolerance` (1e-6 s). The loop-closure
  tolerance is never loosened to make a check pass.
- **Honest complexity comments.** Every cost annotation states what the code does.
- **Thin frontends.** The CLI only parses and prints; the GUI will follow the same rule.

## Status

| Phase | Content | State |
| --- | --- | --- |
| 0–3 | Scaffold, optimizer, simulation fidelity, loop closure | Done |
| 4 | Min-heap leaderboard, overtaking rule, zero warnings | Done |
| 5 | Test harness — 3311 checks | Done |
| 6 | GUI | Next |

The GUI will use Dear ImGui with its Win32 + OpenGL3 backends, vendored as source under
`third_party/`, so it will be Windows-only. It will be a separate `make gui` target: the
default build and `make test` will never depend on it. Planned views: a strategy
explorer, a pit-loss sensitivity sweep, race playback, and your own plan against the
optimum.

Deliberately out of scope: weather, safety cars, telemetry, networking, save files,
randomness, and fuel, ERS or DRS modelling (PRD §2.2).
