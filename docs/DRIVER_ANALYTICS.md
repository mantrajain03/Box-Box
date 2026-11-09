# Driver and Lap Analytics

Owner: Mantra. Plan sections 4.3 and 8.4. PRD FR-15.

Statistics computed from what the simulator actually ran. Nothing here re-derives a lap
time or invents a race outcome, and `RaceSimulator` knows nothing about this module.

## Files

| File | What it is |
|---|---|
| `include/boxbox/LapRecord.hpp` | The shared lap record (plan sec. 10A): car, lap, lap time, traffic, pit, compound, tire age, stint, position |
| `include/boxbox/DriverAnalytics.hpp`, `src/DriverAnalytics.cpp` | `LapRecorder`, `DriverAnalytics`, `selectLaps`, `trailingWindows`, `summarizeWindows`, `rankDrivers` |
| `include/boxbox/MonotonicDeque.hpp`, `src/MonotonicDeque.cpp` | Hand-written ring-buffer sliding-window min / max |
| `cli/DriverReport.hpp`, `cli/DriverReport.cpp` | The `--driver-stats`, `--top-laps`, `--lap-window` reports: printing only |
| `tests/test_driver_analytics.cpp`, `tests/DriverAnalyticsTests.hpp` | Checks, run by `make test` |

## What a lap record means

* `lapTime` is the lap as run: the car's own pace plus any time lost behind another car. It
  excludes pit loss. It is the same number as `Driver::lastLapTime()`.
* `trafficTime` is the part of `lapTime` lost to traffic. It is **inside** `lapTime`.
* `pitTime` is the pit loss charged after the lap. It is **outside** `lapTime`.
* So a car's race time is the sum of `lapTime + pitTime`, and `lapTime - trafficTime` is the lap
  it would have run in clean air. Nothing is counted twice.
* Compound, tire age and stint describe the tires the lap was **run on**; after a stop the
  driver already shows the new set, so the recorder reads them before the lap.
* There are no sector times. The simulator times whole laps only, and the sector times on
  `Track` are configuration, not a measurement of any car (plan sec. 4.3E).

## Structures and costs

D cars, N laps, R records, K requested, W window.

| Piece | Structure | Cost |
|---|---|---|
| Per-car summaries | `std::unordered_map` from car number to slot; never iterated, so output order is deterministic | O(D + R) average; O(R·D) worst case under pathological hashing |
| Fastest / slowest K | Hand-written bounded binary heap with the worst kept lap at the root | O(R log K) time, O(K) space |
| Sliding window min / max | Hand-written ring-buffer monotonic deque | O(1) amortised per value, O(L) over L laps, O(W) space |
| Rankings | Sort with explicit tie-breaks (car number last) | O(D log D) |
| Recording | One pass per lap plus `standings()` | O(D log D) per lap |

Lap times are compared **exactly** (plain `<`), like `Leaderboard::isAhead` and the segment
tree, so a selection returns the very lap a full sort or a scan would; ties fall to explicit
tie-breakers (lap, then car number). Window averages are computed sums and are compared with
`definitelyLess` and `kTimeEpsilon`.

## Reports

```
--driver-stats          time split (green / pit / traffic), best / worst / average lap,
                        range, mean absolute deviation, stints, positions, rankings
--top-laps K            the K fastest and K slowest laps and the fastest lap per compound
--top-laps-car N        with --top-laps: only car N's laps (rejected if N is not in the race)
--top-laps-range F:T    with --top-laps: only laps F to T, inclusive
--lap-window W          each car's quickest and slowest run of W laps, drop-off, widest spread
```

Metrics are project-defined statistics on the simplified model, not real-world performance
scores. `W = 1`, `W = L` and `W > L` are handled explicitly; a car with fewer than W laps shows
its whole race as the one window and the report marks it.

## Input checks

`DriverAnalytics` rejects, with a message naming the car and lap: a lap out of order, a
non-finite time, negative traffic or pit time, traffic **larger than the lap that contains
it**, pit time on a lap without a stop, a wrong stint number, a compound change inside a
stint, and a position outside the field. `MonotonicDeque` rejects a zero window, a window
above `kMaxWindow` (2^26), and a non-finite value.

## Tests

Aggregates against direct calculation (80 seeded synthetic fields and 40 seeded real races,
with and without overtaking); top-K against a full sort (60 fields); windows against a brute
force scan for every window size; ranking tie-breaks under shuffled input; recorder misuse;
determinism. See PRD §8.1.

## Known limits

* No sector analytics (no measured sector data exists).
* No Fenwick tree for cumulative driver statistics; the Fenwick-backed traffic-loss queries
  live in `RaceLapRecorder` (`docs/TIRE_MODULE.md`).
* `pitTime` is taken as `Track::pitLoss()` when the stop count rises, which is what the
  simulator charges today; per-stop pit losses would need a driver field.
