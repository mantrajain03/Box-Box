# Box Box — Frontend Specification

Offline, dependency-free dashboard over the C++17 engine. **The frontend renders; the C++ engine computes.**

## 1. Principles

1. **No algorithm logic in JS.** No sorting for rankings, no DP, no heap, no overtaking, no lap-time maths. JS may
   only: format numbers, map data to pixels, filter/select among already-exported rows, and animate stored steps.
2. **Offline.** Opens via `file://`. No CDN, no web fonts, no network calls, no server. System font stack.
3. **No 3D, no libraries.** Hand-rolled SVG / `<canvas>` charts. Vanilla JS (ES2017), no bundler, no npm.
4. **No dead controls.** Every button/slider/toggle changes something visible. Things that cannot be done offline
   (e.g. arbitrary recompute) appear as a read-only chip showing the CLI command to regenerate.
5. **Traceable.** Every panel has an `ALGO` badge naming the C++ source that produced its data
   (e.g. `PitStrategy.cpp · DP`). Clicking the badge opens the Algorithms page card for it.
6. **One consistent layout** (section 3) across all eight pages.

## 2. Design system

### Colour tokens (`css/theme.css`)

| Token | Hex | Used for |
|---|---|---|
| `--bg` | `#0b0b10` | page background |
| `--panel` | `#12121a` | panel background |
| `--panel-2` | `#181824` | inputs, hover rows |
| `--border` | `#23232f` | panel/table borders |
| `--text` | `#e8e8ee` | primary text |
| `--muted` | `#8a8a9a` | secondary text, axes |
| `--red` | `#ff2d3d` | primary accent, active nav, SOFT |
| `--blue` | `#3b82f6` | info, pit entries, links |
| `--purple` | `#a855f7` | algorithms / DP / optimum |
| `--green` | `#22c55e` | gains, "optimal", positive delta |
| `--yellow` | `#facc15` | MEDIUM, warnings, cliff markers |
| `--pink` | `#ec4899` | overtakes, events |

Fixed semantic mapping (never changes between pages): SOFT = red, MEDIUM = yellow, HARD = light grey/white,
optimum = purple, time gained = green, time lost = red, pit events = blue, overtakes = pink.
Driver colours: fixed 10-colour palette indexed by `driverIndex`, always paired with the car number (not colour alone).

### Typography and shape
- UI: `system-ui, "Segoe UI", Roboto, sans-serif`. Numbers/labels: `ui-monospace, Consolas, monospace` with
  `font-variant-numeric: tabular-nums`.
- Panel header: uppercase, letter-spaced, subtle diagonal hatch background (screenshot inspiration), 1px border,
  8px radius. Soft neon glow (`box-shadow` 0 0 12px accent @ 25%) only on the active/selected element.

### Components (`css/components.css`)
Panel · StatTile (label, big value, delta chip) · Tabs · Segmented control · Slider (with value readout) ·
Badge/Chip · Table (sticky header, sortable only where the *column order itself is exported*) · Legend ·
Tooltip · Empty state · AlgoBadge.

## 3. Shell layout (same on every page)

```
┌──────────────────────────────────────────────────────────────────────────┐
│ ▣ BOX BOX   [Track · 50 laps]        ● OFFLINE · C++17 ENGINE · export id │  top bar 56px
├────────┬─────────────────────────────────────────────────────────────────┤
│ ▤ Cmd  │  Page title                                    [page controls]  │
│ ▶ Sim  │ ┌─────────────── 12-column panel grid ─────────────────────────┐ │
│ ⚙ Lab  │ │ [Panel][Panel][Panel]                                        │ │
│ ◉ Tire │ │ [   Panel   ][  Panel  ]                                     │ │
│ ☰ Drv  │ └──────────────────────────────────────────────────────────────┘ │
│ ⛽ Pit │                                                                  │
│ ⚔ Bat  │                                                                  │
│ ƒ Algo │                                                                  │
├────────┴─────────────────────────────────────────────────────────────────┤
│ ⏮ ⏯ ⏭  1x 2x 4x   Lap ──●────────── 23/50   (global race clock)         │  footer 48px
└──────────────────────────────────────────────────────────────────────────┘
```

- Left rail 64px (icons) expanding to 200px on hover/focus; active item red with glow.
- The **footer race clock** is shared state: lap index, play/pause, speed. Pages that show race state (Command
  Center, Race Sim, Pit & Events) follow it; other pages hide the footer clock.
- Breakpoints: ≥1280 full grid; 900–1279 rail collapsed, grid 8 cols; <900 single column.
- Routing: hash (`#/command`, `#/sim`, `#/lab`, `#/tires`, `#/drivers`, `#/pit`, `#/battle`, `#/algo`).
  Keyboard: `1`–`8` switch pages, `Space` play/pause, `←/→` step lap.

## 4. Pages

Each page lists: purpose, panels (with grid span), interactions, data source, algorithm badge.

### 4.1 Command Center (`#/command`)
Race overview — the landing page.
- **Stat tiles (row, 5×):** Laps · Pit loss · Fastest lap (driver, lap) · Average lap · Total stops.
- **Standings (span 5):** position, number, name, compound dot, tire age, gap to leader, laps done. Follows the
  footer clock. Row click → Drivers page with that driver preselected.
- **Compound distribution (span 3):** donut of laps run per compound.
- **Pit-stop distribution (span 4):** bar chart of stops per lap bucket.
- **Strategy overview (span 6):** optimal plan stint bar (coloured by compound) + total time + stop count.
- **Lap-time trend (span 6):** per-lap race average line; hover shows cumulative time.
- **Interactions:** clock-follow, row click, hover tooltips on charts.
- **Source / ALGO:** `Leaderboard.cpp` (binary min-heap), `RaceLapAnalytics` (`LapTimeIndex` Fenwick/prefix sums),
  `DriverAnalytics.cpp`.

### 4.2 Race Simulator (`#/sim`) — centerpiece (2D)
- **Track map (span 8):** SVG closed path generated from the exported track outline points (sectors coloured
  S1/S2/S3 in blue/purple/green), start/finish line, pit lane drawn as an inset side path. Cars = dots with number
  label, ring coloured by compound. Position along the track = exported `progress` (0–1) for the current lap,
  interpolated *visually* between lap snapshots (pure easing; no physics). A car in PIT state is drawn on the pit lane.
- **Leaderboard (span 4):** live standings, flashing arrow on position change (from exported events).
- **Event feed (span 4):** reverse-chronological: PIT ENTRY/EXIT (blue), OVERTAKE (pink), TRAFFIC HOLD (yellow),
  FINISH (green).
- **Controls:** play/pause, step ±1 lap, 1x/2x/4x, lap scrubber (footer), follow-driver select (camera pans the
  SVG viewBox), **Overtake rule: OFF | ON** segmented control — swaps between two *exported* runs and shows the
  final-time difference.
- **Source / ALGO:** `RaceSimulator.cpp`, `RaceEvent`/`EventPriorityQueue`, `RaceState` (pit-stop state machine).

### 4.3 Strategy Lab (`#/lab`) — the DP showcase
- **Scenario picker (span 12):** pit-loss selector (snaps to exported values), race-length selector (exported
  scenarios). A read-only chip shows: `boxbox export --laps N --pit-loss X` to add scenarios.
- **Optimal plan (span 6):** stint timeline bars (compound-coloured, lap labels, pit markers), total time, green/purple
  "OPTIMAL" badge, green/greenTime vs pitTime split.
- **Stop-count comparison (span 6):** 0/1/2/3-stop best totals as bars; Δ vs optimum; infeasible shown greyed.
- **DP visualiser (span 12):** table `dp[0..N]` with step controls (◀ ▶ ⏵ auto-play, speed). Each step highlights the
  cell being filled and the predecessor/compound chosen (from exported fill trace). Final step animates the
  **backtrack path** across cells and into the stint bar above. Shows the prefix-sum stint cost lookup for the
  highlighted transition.
- **Optimality gap (span 6):** DP total vs brute-force total on a small verification case (exported), gap = 0 with
  green check; plus DP-vs-brute-force runtime bars.
- **Pit-loss sensitivity (span 6):** line chart of optimal total time vs pit loss, second axis/step line of optimal
  stop count; hover shows plan at that pit loss; vertical marker at the selected scenario.
- **Source / ALGO:** `PitStrategy.cpp` (DP + backtrack + prefix sums), `StrategyAnalytics.cpp`
  (alternatives, sensitivity sweep, stops).

### 4.4 Tire Intelligence (`#/tires`)
- **Degradation curves (span 8):** lapDelta vs tire age for each compound; cliff lap marked with yellow ▲;
  legend toggles show/hide a compound.
- **Compound comparison table (span 4):** base pace, degradation/lap, max stint, cliff lap, fastest age, best
  average stint time.
- **Lap-time progression (span 6):** selected compound's lap time vs age.
- **Binary search demo (span 6):** slider "max allowed degradation" → exported bisection steps (lo/mid/hi per
  iteration) animated, result = first age exceeding the threshold.
- **Range query (span 6):** drag a range over the age axis → exported prefix-sum answers (sum, average, min).
  The export contains a full matrix for every (from, to) pair up to the max stint length, so every drag position
  shows a real C++ answer.
- **Sliding window (span 6):** window-size selector (exported sizes 3/5/10) → moving average line and
  monotonic-deque min/max band.
- **Source / ALGO:** `TireAnalytics.cpp`, `MonotonicDeque.hpp`, prefix sums, binary search.

### 4.5 Driver Analytics (`#/drivers`)
- **Driver picker (span 12):** up to 3 drivers (chips, colour-coded).
- **Lap-time comparison (span 8):** multi-line chart over laps; pit laps marked; hover = lap detail.
- **Summary cards (span 4):** per selected driver: best lap, worst lap, mean, consistency score, traffic loss.
- **Sector comparison (span 6):** grouped bars S1/S2/S3 per driver (+ delta to best).
- **Pace ranking (span 6):** exported ranking table (order from C++ — JS does not sort).
- **Top-K laps (span 6):** exported heap results; K selector over exported K ∈ {3,5,10}.
- **Range query visualiser (span 6):** drag lap range → exported Fenwick/segment-tree answers (sum, min, min lap)
  for every range (matrix for the race length), with the touched tree nodes highlighted (exported node ids).
- **Source / ALGO:** `DriverAnalytics.cpp`, `SegmentTree.hpp`, `FenwickTree.hpp`, heaps, hash maps.

### 4.6 Pit & Events (`#/pit`)
- **Pit timeline (span 12):** Gantt: one row per driver, stints coloured by compound, pit stops as blue ticks,
  clock marker following the footer lap.
- **Pit-window histogram (span 4):** stops per lap bucket; bucket size selector (exported 1/3/5).
- **Pit loss summary (span 4):** per driver stops, total pit time, avg loss.
- **Undercut/overcut (span 4):** exported comparison for chosen first-stop lap pair; result as Δ time with colour.
- **Event log (span 8):** table (timestamp, lap, driver, type, sequence id), filter chips by event type.
- **Priority-queue animation (span 4):** "drain in order" steps showing events popped by timestamp from the
  exported heap trace.
- **Source / ALGO:** `PitAnalytics.cpp`, `EventLog`, `EventPriorityQueue`, `StopLapIndex`.

### 4.7 Strategy Battle (`#/battle`)
- **Plan catalogue (span 7):** table of exported evaluated plans (0–3 stops, compound sequences, stint lengths on a
  5-lap grid): stops, compounds, stints, total time, Δ vs optimum. Row click = add to compare (max 4). Column
  header filters (stops, compound) filter rows only; **row order is the exported ranking**.
- **Comparison (span 5):** selected plans stacked as stint bars on a shared time axis + optimum plan on top.
- **Metrics (span 6):** time lost vs optimum, pit tradeoff (pit time vs tyre time), compound utilisation,
  stint efficiency — all exported per plan.
- **Ranking chart (span 6):** horizontal bars total time, optimum purple, others by Δ.
- **Pit-loss robustness (span 12):** for selected plans, total time at each exported pit loss (lines), showing
  where ranking flips.
- **Source / ALGO:** `PitStrategy::evaluatePlan`, `StrategyAnalytics::compare/alternatives`.

### 4.8 Algorithms (`#/algo`)
- **Card grid:** DP + backtracking · Binary min-heap leaderboard · Prefix sums · Segment tree · Fenwick tree ·
  Monotonic deque · Event priority queue · Binary search · Overtake rule (deterministic gap threshold).
- Each card: one-line purpose, **where used** (links to pages), complexity (copied from the C++ complexity
  audit — real per-lap costs, no inflated claims), source file path, and a mini animated demo from an exported trace.
- **Benchmarks (span 12):** exported timings of implementation vs reference (e.g. DP vs brute force, heap vs
  re-sort) as bar chart; shows n and ms. Disclaimer chip: "timings from export machine".
- **Source / ALGO:** `docs/COMPLEXITY_AUDIT`, `benchmarks/bench_strategy.cpp`, all data structure headers.

## 5. Data contract

Produced by `boxbox export --out frontend/data/ [existing race options]` → `bundle.js`
(`window.BOXBOX = {...}`) and `bundle.json` (same content). Deterministic, byte-identical for identical inputs.

```
meta        { version, engine:"boxbox", laps, pitLoss, tolerance, command }
track       { name, sectors:[{name, baseTime}], outline:[[x,y]...], pitLane:[[x,y]...] }
compounds   [{ name, code, basePace, degPerLap, maxStint, cliffLap, cliffDeg }]
strategy    { optimal:{ stints:[{compound,startLap,laps,time}], pitLaps, greenTime, pitTime, totalTime },
              dpTrace:[{ i, best, from, compound, cost }], backtrack:[...],
              stops:[{ stops, feasible, totalTime }], alternatives:[...],
              sweep:[{ pitLoss, stops, totalTime, plan }],
              verification:{ dp, bruteForce, gap, dpMs, bruteMs } }
race        { runs:{ overtakeOff:{...}, overtakeOn:{...} } }   // each run:
              { drivers:[{index, number, name, plan}],
                laps:[ { lap, standings:[{driver, position, gap, compound, age, progress, state}] } ],
                events:[{type, t, lap, driver, seq}], finalTimes:[...] }
analytics   { drivers:[{ index, lapTimes[], sectorTimes[][], best, worst, mean, consistency, traffic,
                         rangeMatrix:{sum,min,minIdx}, topLaps:{3:[],5:[],10:[]} }],
              ranking:[...], pit:{...}, undercut:[...], histogram:{1:[],3:[],5:[]} }
tires       { curves:[{compound, delta:[...]}], thresholds:[{compound,max,steps:[{lo,mid,hi}],age}],
              windows:{3:[],5:[],10:[]}, rangeMatrix:[...] }
battle      { plans:[{id, compounds, stints, stops, totalTime, deltaVsOptimal, pitTime, efficiency,
                       byPitLoss:[...]}] }
algorithms  [{ id, name, file, complexity, usedIn:[...], trace:[...] }]
bench       [{ name, n, impl, ref, implMs, refMs }]
```

Invariant tested in C++: `strategy.optimal.totalTime` equals the `StrategyResult::totalTime` (FR-8 closure) within
`kClosureTolerance`; stint laps sum to `laps`.

## 6. File structure

```
frontend/
  FRONTEND.md
  index.html
  css/ theme.css  layout.css  components.css
  js/
    store.js            clock (lap, playing, speed), selection state; plain pub/sub
    router.js           hash routing + keyboard shortcuts
    charts.js           line, bar, donut, gantt, stint-bar, table helpers (SVG/canvas)
    algobadge.js        badge + tooltip
    pages/ command.js racesim.js strategy.js tires.js drivers.js pit.js battle.js algorithms.js
  data/ bundle.js bundle.json   (generated)
```
Each page module exports `render(root, data, store)` and `destroy()`.

## 7. Build and run

```
make            # core + CLI, unchanged, dependency-free
make test       # unchanged + new export tests
make export     # runs boxbox export → frontend/data/bundle.{js,json}
# then double-click frontend/index.html (works offline via file://)
```

## 8. Limits (stated honestly)
- Strategy Lab and Battle explore **precomputed** scenarios; arbitrary new inputs require re-running
  `boxbox export` (chip shows the command). No live recompute: no server/WASM by design.
- Track outline is a stylised export shape, not surveyed circuit geometry.
- Car motion between laps is visual easing only; positions/progress are from the engine.
- Out of scope (PRD §2.2): weather, safety cars, telemetry ingestion, networking, persistence, stochastic modelling.

## 9. Build order
A. This spec → B. C++ `JsonWriter` + `export` + tests → C. Shell, design system, Command Center →
D. Race Simulator → E. Strategy Lab → F. Tires → G. Drivers → H. Pit & Events → I. Battle → J. Algorithms →
K. Polish (consistency, empty states, keyboard, no-dead-controls audit).

## 10. Open items needing the project owner
- Update CLAUDE.md rule 2 / GUI section and PRD Phase 6 / §9 Q3 (ImGui → web frontend). Not edited by Claude.
- Commit `frontend/data/` or gitignore it (generated).
- The DP fill trace, brute-force verification, and range matrices need small new **read-only accessors** or
  export-side recomputation in C++; any change to an existing public class interface will be proposed first.
