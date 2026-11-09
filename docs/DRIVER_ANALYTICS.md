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
* Sector times are **modelled, not measured**. The simulator times whole laps only, and the
  sector times on `Track` are configuration. As plan sec. 4.3E prescribes, `LapRecorder`
  attaches a deterministic split of each lap in proportion to the track's configured sector
  durations (`modelledSectorTimes`): sector i gets `lapTime × base(i) / baseLapTime`, and the
  last sector takes the remainder so the parts add back to the lap. `LapRecord::sectorTimes` is
  empty when the track has no sectors. Every sector report says it is a modelled split, and it
  carries no information the lap time does not.

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

## Sector analytics (modelled)

| Function | Gives | Cost |
|---|---|---|
| `sectorSummaries(analytics)` | per car: fastest time and lap in each sector (earliest lap on ties), average per sector, best combined lap (the sum of the fastest sectors) | O(R·S) |
| `fieldBestSectors`, `fieldBestCombinedLap` | the best time in each sector over the field and who set it (ties: earlier lap, then lower car number); the lap the field could post if one car took every best sector | O(R·S) |
| `compareSectors(a, b)` | sector by sector: fastest and average for each car and the average gap | O(R·S) |
| `sectorDistribution(sector, bins)` | count, minimum, maximum, mean, population standard deviation and an equal-width histogram over every recorded lap | O(R + bins) |

R lap records, S sectors. A car whose records carry no sector times is reported with
`available == false`, never with invented numbers. CLI: `--sector-stats`, `--sector-compare A:B`.
Because the split is proportional, the sector statistics are the lap statistics scaled by fixed
fractions; the report says so rather than presenting them as independent evidence.

## Input checks

`DriverAnalytics` rejects, with a message naming the car and lap: a lap out of order, a
non-finite time, negative traffic or pit time, traffic **larger than the lap that contains
it**, pit time on a lap without a stop, a wrong stint number, a compound change inside a
stint, a position outside the field, a sector count that changes from one lap of a car to the
next, a negative or non-finite sector time, and sector times that do not add up to the lap
time (relative 1e-9). `MonotonicDeque` rejects a zero window, a window above `kMaxWindow`
(2^26), and a non-finite value.

## Tests

Aggregates against direct calculation (80 seeded synthetic fields and 40 seeded real races,
with and without overtaking); top-K against a full sort (60 fields); windows against a brute
force scan for every window size; ranking tie-breaks under shuffled input; recorder misuse;
determinism. See PRD §8.1.

## Known limits

* Sector times are a modelled split, not measured (see above); the contract-10D "optional
  sector statistics" are `DriverSectorSummary`, computed on demand rather than stored in
  `DriverSummary`.
* No Fenwick tree for cumulative driver statistics; the Fenwick-backed traffic-loss queries
  live in `RaceLapRecorder` (`docs/TIRE_MODULE.md`).
* `pitTime` is taken as `Track::pitLoss()` when the stop count rises, which is what the
  simulator charges today; per-stop pit losses would need a driver field.
