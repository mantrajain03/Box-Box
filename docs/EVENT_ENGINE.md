# Race Event Engine and Pit Analytics

Owner: Pratyaksh. Plan sections 4.4, 4.5 and 8.2. PRD FR-15.

A deterministic event layer over the simulator: typed events, a hand-written priority queue,
an append-only log, a per-driver state machine, and pit and stint reports computed from the
log. Standard library only.

## Files

| File | What it is |
|---|---|
| `include/boxbox/RaceEvent.hpp` | `RaceEvent`, `RaceEventType`, and `eventComesBefore`, the ordering |
| `include/boxbox/EventPriorityQueue.hpp`, `src/EventPriorityQueue.cpp` | Hand-written binary min-heap of events |
| `include/boxbox/EventLog.hpp`, `src/EventLog.cpp` | Append-only record of processed events |
| `include/boxbox/RaceState.hpp`, `src/RaceState.cpp` | `RaceState` (one driver's legal transitions) and `validateEventLog` |
| `include/boxbox/PitAnalytics.hpp`, `src/PitAnalytics.cpp` | Per-driver stops, stints, compounds, pit window; undercut / overcut comparison |
| `tests/test_events.cpp`, `tests/EventTests.hpp` | Checks, run by `make test` |

`RaceSimulator::setEventSink(&queue)` is opt-in. With no sink nothing is emitted and the run
is bit-identical; emitting only reads `Driver`, so logging cannot change a race time.

## Events

| Type | When | Payload |
|---|---|---|
| `LapCompleted` | every lap of every car | `oldCompoundIndex` = compound the lap ran on |
| `PitEntry` | the lap ended a stint that is followed by another | |
| `CompoundChange` | same instant | old and new compound, tire age 0 |
| `PitExit` | same instant | tire age 0 |
| `PositionChange` | after each full lap round, for every car whose running position differs from the previous round (the first round is measured from the grid, car-number order) | `oldPosition`, `newPosition` |
| `OvertakeAttempt` | overtaking on, a car caught the car ahead | `otherDriverNumber` = car caught |
| `Overtake` | the attempt succeeded: the car had the pace to pass | `otherDriverNumber` = car passed |
| `TrafficHold` | the attempt failed: the car was held | `otherDriverNumber`, `holdLossSeconds` |
| `RaceFinish` | the car's last lap | |

Every `OvertakeAttempt` is followed by exactly one `Overtake` or `TrafficHold` for the same
car and lap (checked in `test_events.cpp`). `PositionChange` positions are those of
`RaceSimulator::standings()` once the whole round is done, the same definition
`LapRecord::position` uses; each car's chain of changes starts at its grid place and ends at
its final classification. Emitting position changes costs one `standings()` call per lap,
O(D log D), and only when a sink is set.

**Not modelled:** pit-lane time. Pit entry, tire change and exit share one timestamp, which
includes the pit loss, so the log has no pit-lane duration and no traffic-after-stop figure
(plan sec. 4.5). That would need a deliberate extension of the stop model. Weather, safety
cars and race control are out of scope (PRD §2.2).

## Ordering

`eventComesBefore` is a strict weak order over four keys: timestamp (ascending), event type
(declaration order: `PitEntry`, `CompoundChange`, `PitExit`, `LapCompleted`, `PositionChange`,
`OvertakeAttempt`, `Overtake`, `TrafficHold`, `RaceFinish`), car number, then the sequence id
the queue assigns on insertion. The plan lists time, then lap, then sequence; the lap level is
omitted (the plan says "if required") because within one car time already orders laps.

The timestamp is compared **exactly**. An earlier version compared with a tolerance and
fell through to the next key on a tie; a tolerance comparison is not transitive
(a ≈ b and b ≈ c does not give a ≈ c), so that was not a valid ordering and could misorder
near-equal timestamps in the heap. Identical input produces bit-identical timestamps, so
exact comparison is still deterministic.

## Queue

| Operation | Cost |
|---|---|
| `push` | O(log E); rejects a NaN, infinite or negative timestamp (`std::invalid_argument`) and leaves the queue unchanged |
| `peek` | O(1) |
| `pop` | O(log E) |
| `processUntil(t)` | O(k log E) for k events released (inclusive of `t`, within `kTimeEpsilon`) |
| storage | O(E) |

## State machine

`RaceState` allows `Racing → PitEntryPending → InPit → Racing` and `Racing → Finished`.
`LapCompleted`, `PositionChange`, `OvertakeAttempt`, `Overtake` and `TrafficHold` are legal while `Racing`; nothing is legal after
`Finished`; an event for a different driver is rejected; a rejected event changes nothing.

`validateEventLog(log)` replays a whole log, one `RaceState` per car (hash map, O(n)
expected), and checks it is in canonical order. The CLI runs it after `--events` and
`--pit-report` and exits 3 on failure. A race that has not finished is legal.

## Pit analytics

`summaryFor(driver)` and `allSummaries()` report stop count, stop laps, compound per stint
and stint lengths, **including the first and the final stint**, so stint lengths sum to the
laps the car completed. `allSummaries` is a single pass with a hash map from car number to
slot (O(n) expected); `summaryFor` and `totalPitLoss` are O(n) over the whole log.
`pitWindow()` is the earliest and latest stop lap across all cars.

Plan sec. 4.5, one item per algorithm:

| Item | Function | Method | Cost |
|---|---|---|---|
| Stint statistics | `stintStats`, `fieldStintStats` | count, shortest, longest, mean | O(S) / O(n) |
| Stint distribution | `stintHistogram(binWidth)` | equal-width bins from the shortest stint, empty bins kept, counts sum to the number of stints | O(n + B) |
| Chronological stop report | `chronologicalPitStops` | sort by (time, car, lap), exact comparison | O(n + s log s) |
| Cumulative stop counts | `buildStopIndex(raceLaps)` → `StopLapIndex` | **prefix sums** of stops per lap | O(n + N) build, O(1) per query |
| Configured windows | `windowSummary(from, to, pitLoss)` | scan of the log; cross-checked against the prefix count | O(n) |
| Stop timing comparison | `compareStopTiming(a, b)` | stop-by-stop lap difference | O(n) |
| Stint coverage | `checkStintCoverage` | **interval processing**: sort, then walk, reporting the first gap, overlap or overrun | O(S log S) |
| Fewest pit calls | `minimumStopLaps(windows)` | **greedy interval stabbing**: sort by latest lap, take a window's latest lap when it is not already covered; optimal by the standard exchange argument | O(W log W) |
| Traffic after a stop | `trafficAfterStops(records, n)` | per-car prefix sums of traffic time and a binary search for the n-th lap after the stop | O(R log R) |
| Pit-lane time | `totalPitLoss`, `fieldPitLoss` | stops × pit loss | O(n) |

**Pit-lane time is the pit loss.** The simulator charges a stop as one lump, so entry, tire
change and exit share a timestamp. There is no separate pit-lane duration to report, and the
`--pit-report` output says so. Modelling an entry / service / exit split would be a change to
`Driver` and `RaceSimulator` and is recorded as an open question in the PRD.

The greedy problem, stated exactly: given every car's allowed window of laps in which it may
stop, choose the fewest laps such that each window contains at least one chosen lap. The
`--pit-report` instance gives each logged stop a window of ±2 laps around the lap it was made
on.

`compareUndercutOvercut(strategy, earlierPlan, laterPlan, earlierLap, laterLap)` evaluates
two caller-supplied plans with `PitStrategy::evaluatePlan`. It requires each plan's first
stint to end on its stated lap and `earlierLap < laterLap`, and throws otherwise. It is a
simplified model: no traffic, tire warm-up or real pit-window rules.

`PitAnalytics` holds a reference to the log, so it cannot be built from a temporary.

## CLI

```
boxbox --laps 50 --events           chronological event log (implies --simulate)
boxbox --laps 50 --pit-report       stops, stint statistics and distribution, stops in order,
                                    stint coverage, fewest pit calls, traffic after each stop
boxbox --laps 50 --pit-window 10:25 stops, cars and pit loss in laps 10 to 25 (implies --pit-report)
boxbox --laps 30 --rivals --overtake --min-gap 5 --pass-margin 10 --events
boxbox --laps 50 strategy undercut --earlier-lap 15 --later-lap 19
```

`strategy undercut` is `PitAnalytics::compareUndercutOvercut` driven by
`compareFirstStopLaps`: both plans open on the optimum's first compound and stop after the
given lap, then take the best continuation on fresh tires. It is a simplified model (clean
air, no tire warm-up, no traffic) and says so in its output.

## Tests

The queue against a full sort by the same ordering (60 seeded sets with many equal
timestamps); the strict-weak-order laws over a pool of near-ties; every illegal transition;
hand-timed events of a 6-lap race (stops after laps 3 and 5, finish at 77 s); traffic events
against the simulator's own `trafficLoss()`; pit summaries against each driver's plan; the
undercut hand case (75 s against 71 s); that logging never changes a race time; and
identical event streams run to run. See PRD §8.1.
