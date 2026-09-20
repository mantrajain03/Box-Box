# Project: Box Box — F1 Pit-Stop & Race Strategy Scheduler

You are working in an existing C++ repository. Before touching any code, read `PRD.md`
in full — it is the specification and it overrides any assumption you make. Then read
`README.md` and `PROJECT_REPORT.md` if present, and inspect every existing source file
to see what is already implemented. Several requirements are already marked Done in the
PRD; do not rewrite working code.

## Absolute rules

1. **Never commit.** Do not run `git add`, `git commit`, `git push`, `git pull`,
   `git checkout`, `git switch`, `git merge`, `git rebase`, `git reset`, `git stash`,
   `git tag`, or anything else that writes to git state, history, branches, or hooks.
   I make every commit myself. Read-only commands (`git status`, `git diff`, `git log`)
   are fine.
2. **C++ is the only language.** No Python, Bash, JavaScript, or any other scripts —
   not for building, not for testing, not for utilities. The single build entry point is
   the existing Makefile. All tests are C++ programs.
3. **Core and CLI depend on nothing outside the C++17 standard library.** STL only, no
   external libraries. This is NFR-1 in the PRD and a claim in my submitted synopsis —
   do not break it under any circumstances.
4. **Stay in scope.** No weather, safety cars, telemetry ingestion, networking,
   persistence, or stochastic race modelling. These are explicit non-goals.
5. **Small, reviewable diffs.** Since I commit by hand, do not reformat, rename, or
   restructure files you weren't asked to touch, and never run a repo-wide formatting pass.
6. **Stop after each phase**, report, and wait for me. Do not run ahead.
7. Do not edit `PRD.md`, `README.md`, or `PROJECT_REPORT.md` unless I ask. If the code
   and the PRD disagree, tell me — don't quietly change either one.

## Engineering standards

- C++17, clean compile under `-Wall -Wextra`, zero warnings.
- Deterministic: identical inputs produce byte-identical output. Anything probabilistic
  must take an explicit seed.
- `Track`, `PitStrategy`, and `Leaderboard` must each be usable and testable without
  constructing `RaceSimulator` (NFR-3).
- No algorithm logic in `main.cpp` or in any GUI code. Both are thin frontends over the core.
- Complexity comments must match what the code actually does. Do not write "O(log n)"
  above a leaderboard that is rebuilt every lap — state the real per-lap cost (NFR-2).
- Never compare floating-point values with `==`; use a named tolerance constant.

## Phases — stop and report after each one

**Phase 1 — Optimizer (FR-1).** DP fill loop and backtrack over the existing prefix-sum
table. Acceptance: `dp[totalLaps]` finite, returned stint lengths sum to exactly
`totalLaps`, and the result matches a 3–5 lap case I can verify by hand. Show me that
small case worked through before running 50 laps.

**Phase 2 — Simulation fidelity (FR-5, FR-6).** Lap time from the driver's actual
compound and tire age instead of flat track time; compound switches at each stop.
Acceptance: two drivers on different compounds diverge in cumulative time over identical laps.

**Phase 3 — Loop closure (FR-7, FR-8).** Drive at least one simulated driver from
`StrategyResult` rather than hard-coded stop laps. Acceptance: in a single-driver,
no-overtake configuration, that driver's final cumulative time equals
`StrategyResult::totalTime` within tolerance. This is the most important check in the
project — if it fails, the bug is real, so do not loosen the tolerance to make it pass.

**Phase 4 — Data structures and polish.** Real min-heap `Leaderboard` (FR-2). Optional
overtake rule (FR-9) using a deterministic gap threshold — propose the rule and wait for
my approval before implementing. Zero-warning build (NFR-4).

**Phase 5 — Tests.** A `make test` target running a C++ assert-based harness you write
yourself (no test framework — that would violate rule 3). Cover: DP vs hand computation;
stint lengths summing to `totalLaps`; zero pit loss favouring many short stints on the
softest compound; very high pit loss collapsing to one stop or none; the FR-8 loop
closure; and edge cases — 0 laps, 1 lap, single driver, driver with no planned stops,
stop on lap 0, stop on the final lap, single-sector track.

**Phase 6 — GUI.** See below.

## GUI

Wanted, but never at the cost of rule 3.

- Keep the core as a dependency-free library that has no awareness of any GUI. The CLI
  target and the GUI target both link against it.
- Default `make` builds core + CLI only and stays dependency-free. The GUI is a separate
  `make gui` target; its absence must never break the default build or `make test`.
- Preferred route: Dear ImGui with a GLFW + OpenGL3 backend, vendored as source under
  `third_party/` and compiled by the Makefile, so there is no package manager step.
  Before writing any GUI code, check whether GLFW and OpenGL development headers exist
  on this machine and report what you found.
- Fallback if they don't: a pure-C++ terminal UI using ANSI escape codes (no ncurses) —
  a standings table redrawn each lap, colour-coded compounds, and a per-driver stint
  timeline. Ask me which route to take before starting.

GUI features in priority order:
1. Strategy explorer — inputs for race length, pit loss, and per-compound pace and
   degradation; run the optimizer; display the stint plan and total race time.
2. Sensitivity view — sweep pit loss across a slider range and show how optimal stop
   count and total time change (`ImGui::PlotLines` is enough; no plotting library).
3. Race playback — lap-by-lap standings with play/pause/step, showing each driver's
   compound, tire age, and gap to the leader.
4. Strategy comparison — a plan I enter versus the computed optimum, with the time lost.

## Reporting after each phase

Tell me: which files changed and why; how you verified it, with the actual commands and
their real output rather than a claim that it passed; anything in the PRD you could not
satisfy; and a suggested commit message. Then stop — I will review and commit.

## Ask instead of guessing

Ask me before deciding any of these:
- Whether a compound's `basePace` replaces or adds to the track's lap base time. The
  optimizer and the simulator must treat this identically or FR-8 can never pass, and
  the PRD does not settle it.
- The FR-9 overtake rule, which the PRD leaves open.
- Any change to the public interface of an existing class.