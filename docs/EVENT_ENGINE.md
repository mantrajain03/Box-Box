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
| `Overtake` | overtaking on, car caught the car ahead and had the pace to pass | `otherDriverNumber` = car passed |
| `TrafficHold` | overtaking on, car caught the car ahead and was held | `otherDriverNumber`, `holdLossSeconds` |
| `RaceFinish` | the car's last lap | |

**Not modelled:** pit-lane time. Pit entry, tire change and exit share one timestamp, which
includes the pit loss, so the log has no pit-lane duration and no traffic-after-stop figure
(plan sec. 4.5). That would need a deliberate extension of the stop model. Weather, safety
cars and race control are out of scope (PRD §2.2).

## Ordering

`eventComesBefore` is a strict weak order over four keys: timestamp (ascending), event type
(declaration order: `PitEntry`, `CompoundChange`, `PitExit`, `LapCompleted`, `Overtake`,
`TrafficHold`, `RaceFinish`), car number, then the sequence id the queue assigns on insertion.

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
`LapCompleted`, `Overtake` and `TrafficHold` are legal while `Racing`; nothing is legal after
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

`compareUndercutOvercut(strategy, earlierPlan, laterPlan, earlierLap, laterLap)` evaluates
two caller-supplied plans with `PitStrategy::evaluatePlan`. It requires each plan's first
stint to end on its stated lap and `earlierLap < laterLap`, and throws otherwise. It is a
simplified model: no traffic, tire warm-up or real pit-window rules.

`PitAnalytics` holds a reference to the log, so it cannot be built from a temporary.

## CLI

```
boxbox --laps 50 --events           chronological event log (implies --simulate)
boxbox --laps 50 --pit-report       per-driver stops and stints, overall pit window
boxbox --laps 30 --rivals --overtake --min-gap 5 --pass-margin 10 --events
```

## Tests

The queue against a full sort by the same ordering (60 seeded sets with many equal
timestamps); the strict-weak-order laws over a pool of near-ties; every illegal transition;
hand-timed events of a 6-lap race (stops after laps 3 and 5, finish at 77 s); traffic events
against the simulator's own `trafficLoss()`; pit summaries against each driver's plan; the
undercut hand case (75 s against 71 s); that logging never changes a race time; and
identical event streams run to run. See PRD §8.1.
