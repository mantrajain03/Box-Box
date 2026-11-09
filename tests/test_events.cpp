// Box Box: checks for the race event engine (plan sec. 4.4, 4.5 and 8.2).
//
// Covers RaceEvent ordering, EventPriorityQueue, EventLog, RaceState /
// validateEventLog, PitAnalytics, and the simulator's opt-in event sink. The
// heap is checked against a plain sort by the same ordering, the analytics
// against the simulator's own driver records, and one race is small enough to
// work out by hand. Same harness style as test_main.cpp: a failing check is
// recorded and the run carries on.

#include "EventTests.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "boxbox/Common.hpp"
#include "boxbox/DriverAnalytics.hpp"
#include "boxbox/EventLog.hpp"
#include "boxbox/EventPriorityQueue.hpp"
#include "boxbox/PitAnalytics.hpp"
#include "boxbox/PitStrategy.hpp"
#include "boxbox/RaceEvent.hpp"
#include "boxbox/RaceSimulator.hpp"
#include "boxbox/RaceState.hpp"
#include "boxbox/TireCompound.hpp"
#include "boxbox/Track.hpp"

namespace {

using boxbox::DriverPitSummary;
using boxbox::DriverRaceState;
using boxbox::EventLog;
using boxbox::EventPriorityQueue;
using boxbox::IllegalStateTransition;
using boxbox::OvertakeRule;
using boxbox::PitAnalytics;
using boxbox::PitStrategy;
using boxbox::PlannedStint;
using boxbox::RaceEvent;
using boxbox::RaceEventType;
using boxbox::RaceSimulator;
using boxbox::RaceState;
using boxbox::StrategyResult;
using boxbox::TireCompound;
using boxbox::Track;

// ---------------------------------------------------------------------------
// Harness
// ---------------------------------------------------------------------------

EventTestTally g_tally;
std::string g_section;

void beginSection(const std::string& name) {
    g_section = name;
    std::cout << "\n== " << name << '\n';
}

void reportCheck(bool ok, const char* expression, const char* file, int line,
                 const std::string& detail) {
    ++g_tally.checks;
    if (ok) {
        return;
    }
    ++g_tally.failures;
    std::cout << "   FAIL  " << file << ':' << line << "  in " << g_section << '\n';
    std::cout << "         " << expression << '\n';
    if (!detail.empty()) {
        std::cout << "         " << detail << '\n';
    }
}

std::string describe(double actual, double expected, double tolerance) {
    std::ostringstream out;
    out << std::setprecision(17) << "actual " << actual << "   expected " << expected
        << "   diff " << std::fabs(actual - expected) << "   tol " << tolerance;
    return out.str();
}

#define ECHECK(expr) reportCheck((expr), #expr, __FILE__, __LINE__, std::string())

#define ECHECK_NEAR(actual, expected, tolerance)                                        \
    reportCheck(std::fabs((actual) - (expected)) <= (tolerance),                        \
                #actual " ~= " #expected, __FILE__, __LINE__,                           \
                describe((actual), (expected), (tolerance)))

#define ECHECK_THROWS(statement, ExType)                                                \
    do {                                                                                \
        bool threw = false;                                                             \
        try {                                                                           \
            statement;                                                                  \
        } catch (const ExType&) {                                                       \
            threw = true;                                                               \
        } catch (...) {                                                                 \
        }                                                                               \
        reportCheck(threw, "throws " #ExType ": " #statement, __FILE__, __LINE__,       \
                    std::string());                                                     \
    } while (false)

/// Same seeded LCG as the other test files: inputs vary, the run does not.
class Lcg {
public:
    explicit Lcg(std::uint64_t seed) noexcept : state_(seed) {}

    std::uint64_t next() noexcept {
        state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
        return state_ >> 33U;
    }
    int nextInt(int low, int high) noexcept {
        const std::uint64_t span = static_cast<std::uint64_t>(high - low + 1);
        return low + static_cast<int>(next() % span);
    }

private:
    std::uint64_t state_;
};

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

RaceEvent eventAt(double t, RaceEventType type, int driver, int lap = 1) {
    RaceEvent e;
    e.type = type;
    e.timestamp = t;
    e.driverNumber = driver;
    e.lap = lap;
    return e;
}

const RaceEventType kAllTypes[] = {
    RaceEventType::PitEntry,       RaceEventType::CompoundChange, RaceEventType::PitExit,
    RaceEventType::LapCompleted,   RaceEventType::PositionChange, RaceEventType::OvertakeAttempt,
    RaceEventType::Overtake,       RaceEventType::TrafficHold,    RaceEventType::RaceFinish};
constexpr int kTypeCount = static_cast<int>(sizeof(kAllTypes) / sizeof(kAllTypes[0]));

RaceEventType randomType(Lcg& rng) { return kAllTypes[rng.nextInt(0, kTypeCount - 1)]; }

/// The hand-worked pit race. One 10 s sector, 6 laps, 5 s pit loss.
///   compound A (index 0): +0 s, 1 s/lap wear, stints of up to 3 laps
///   compound B (index 1): +2 s, no wear, stints of up to 6 laps
///   car 7 runs A for 3 laps, B for 2, A for 1: stops after laps 3 and 5.
///   car 9 runs B for all 6 laps: no stops.
struct HandRace {
    Track track{"Hand", 6, 5.0};
    std::vector<TireCompound> compounds;
    HandRace() {
        track.addSector("S1", 10.0);
        compounds = {TireCompound("A", 0.0, 1.0, 3), TireCompound("B", 2.0, 0.0, 6)};
    }
};

RaceSimulator buildHandRace(const HandRace& h, EventPriorityQueue* sink) {
    const PitStrategy strategy(h.track, h.compounds);
    RaceSimulator sim(h.track, h.compounds);
    if (sink != nullptr) {
        sim.setEventSink(sink);
    }
    sim.addDriver(7, "SEVEN",
                  strategy.evaluatePlan({PlannedStint{0, 3}, PlannedStint{1, 2}, PlannedStint{0, 1}}));
    sim.addDriver(9, "NINE", strategy.evaluatePlan({PlannedStint{1, 6}}));
    return sim;
}

EventLog drain(EventPriorityQueue& queue) {
    EventLog log;
    while (!queue.empty()) {
        log.record(queue.pop());
    }
    return log;
}

std::size_t countOf(const EventLog& log, RaceEventType type) {
    return log.entriesOfType(type).size();
}

/// A 30-lap field on the default compound trio, with the optimal plan and one
/// single-compound rival per compound, so plenty of cars bunch up.
struct Field {
    Track track{"Race", 30, 21.0};
    std::vector<TireCompound> compounds;
    Field() {
        track.addSector("S1", 90.0);
        compounds = {TireCompound("SOFT", -0.6, 0.09, 22, 16, 0.18),
                     TireCompound("MEDIUM", 0.0, 0.055, 32, 26, 0.11),
                     TireCompound("HARD", 0.55, 0.035, 45, 38, 0.07)};
    }
};

RaceSimulator buildField(const Field& f, EventPriorityQueue* sink, const OvertakeRule& rule) {
    const PitStrategy strategy(f.track, f.compounds);
    RaceSimulator sim(f.track, f.compounds);
    if (sink != nullptr) {
        sim.setEventSink(sink);
    }
    sim.setOvertakeRule(rule);
    sim.addDriver(1, "OPTIMAL", strategy.optimize());
    int number = 2;
    for (std::size_t c = 0; c < f.compounds.size(); ++c) {
        sim.addDriver(number++, "ALL-" + f.compounds[c].name(), strategy.singleCompoundPlan(c));
    }
    return sim;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

void testEventOrdering() {
    beginSection("eventComesBefore is a strict weak order, with exact timestamps");

    // Time first, then type (declaration order), then driver, then sequence.
    ECHECK(boxbox::eventComesBefore(eventAt(1.0, RaceEventType::RaceFinish, 9),
                                    eventAt(2.0, RaceEventType::PitEntry, 1)));
    ECHECK(!boxbox::eventComesBefore(eventAt(2.0, RaceEventType::PitEntry, 1),
                                     eventAt(1.0, RaceEventType::RaceFinish, 9)));
    ECHECK(boxbox::eventComesBefore(eventAt(1.0, RaceEventType::PitEntry, 5),
                                    eventAt(1.0, RaceEventType::LapCompleted, 1)));
    ECHECK(boxbox::eventComesBefore(eventAt(1.0, RaceEventType::LapCompleted, 1),
                                    eventAt(1.0, RaceEventType::LapCompleted, 2)));
    RaceEvent first = eventAt(1.0, RaceEventType::LapCompleted, 1);
    RaceEvent second = first;
    first.sequenceId = 3;
    second.sequenceId = 4;
    ECHECK(boxbox::eventComesBefore(first, second));
    ECHECK(!boxbox::eventComesBefore(second, first));
    ECHECK(!boxbox::eventComesBefore(first, first));

    // Timestamps a hair apart are NOT treated as ties: that was the old
    // tolerance-based behaviour, which is not transitive.
    ECHECK(boxbox::eventComesBefore(eventAt(1.0, RaceEventType::RaceFinish, 9),
                                    eventAt(1.0 + 1e-12, RaceEventType::PitEntry, 1)));

    // Strict weak order laws over a pool full of near-ties and exact ties.
    Lcg rng(4242ULL);
    const double times[] = {1.0, 1.0 + 1e-12, 1.0 + 2e-12, 2.0};
    std::vector<RaceEvent> pool;
    for (int i = 0; i < 28; ++i) {
        RaceEvent e = eventAt(times[rng.nextInt(0, 3)], randomType(rng), rng.nextInt(1, 3));
        e.sequenceId = static_cast<std::uint64_t>(rng.nextInt(0, 3));
        pool.push_back(e);
    }
    const auto less = [](const RaceEvent& a, const RaceEvent& b) {
        return boxbox::eventComesBefore(a, b);
    };
    bool irreflexive = true, asymmetric = true, transitive = true, eqTransitive = true;
    for (const RaceEvent& a : pool) {
        irreflexive = irreflexive && !less(a, a);
        for (const RaceEvent& b : pool) {
            asymmetric = asymmetric && !(less(a, b) && less(b, a));
            for (const RaceEvent& c : pool) {
                if (less(a, b) && less(b, c) && !less(a, c)) transitive = false;
                const bool abTie = !less(a, b) && !less(b, a);
                const bool bcTie = !less(b, c) && !less(c, b);
                if (abTie && bcTie && (less(a, c) || less(c, a))) eqTransitive = false;
            }
        }
    }
    ECHECK(irreflexive);
    ECHECK(asymmetric);
    ECHECK(transitive);
    ECHECK(eqTransitive);
}

void testQueueBasics() {
    beginSection("EventPriorityQueue: empty, peek, pop, invalid times");

    EventPriorityQueue q;
    ECHECK(q.empty());
    ECHECK(q.size() == 0U);
    ECHECK_THROWS(q.pop(), std::out_of_range);
    ECHECK_THROWS(q.peek(), std::out_of_range);
    ECHECK(q.processUntil(100.0).empty());

    q.push(eventAt(5.0, RaceEventType::LapCompleted, 1));
    q.push(eventAt(2.0, RaceEventType::LapCompleted, 2));
    q.push(eventAt(9.0, RaceEventType::LapCompleted, 3));
    ECHECK(q.size() == 3U);
    ECHECK(q.peek().driverNumber == 2);
    ECHECK(q.size() == 3U);  // peek does not remove
    ECHECK(q.pop().driverNumber == 2);
    ECHECK(q.pop().driverNumber == 1);
    ECHECK(q.pop().driverNumber == 3);
    ECHECK(q.empty());

    // The queue assigns sequence ids in push order.
    EventPriorityQueue seq;
    seq.push(eventAt(1.0, RaceEventType::LapCompleted, 1));
    seq.push(eventAt(1.0, RaceEventType::LapCompleted, 1));
    const RaceEvent a = seq.pop();
    const RaceEvent b = seq.pop();
    ECHECK(a.sequenceId == 0U);
    ECHECK(b.sequenceId == 1U);

    // Invalid timestamps are rejected and leave the queue unchanged.
    EventPriorityQueue bad;
    bad.push(eventAt(1.0, RaceEventType::LapCompleted, 1));
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    ECHECK_THROWS(bad.push(eventAt(nan, RaceEventType::LapCompleted, 1)), std::invalid_argument);
    ECHECK_THROWS(bad.push(eventAt(inf, RaceEventType::LapCompleted, 1)), std::invalid_argument);
    ECHECK_THROWS(bad.push(eventAt(-inf, RaceEventType::LapCompleted, 1)), std::invalid_argument);
    ECHECK_THROWS(bad.push(eventAt(-0.5, RaceEventType::LapCompleted, 1)), std::invalid_argument);
    ECHECK(bad.size() == 1U);
    bad.push(eventAt(0.0, RaceEventType::LapCompleted, 2));  // zero is legal
    ECHECK(bad.size() == 2U);

    bad.clear();
    ECHECK(bad.empty());
}

void testQueueAgainstSort() {
    beginSection("EventPriorityQueue pops in the same order as a full sort, 60 seeded sets");

    Lcg rng(777ULL);
    const double times[] = {0.0, 1.5, 1.5, 3.0, 4.25, 4.25};
    for (int trial = 0; trial < 60; ++trial) {
        const int n = rng.nextInt(0, 80);
        std::vector<RaceEvent> reference;
        EventPriorityQueue q;
        for (int i = 0; i < n; ++i) {
            RaceEvent e = eventAt(times[rng.nextInt(0, 5)], randomType(rng), rng.nextInt(1, 4),
                                  rng.nextInt(1, 9));
            q.push(e);
            e.sequenceId = static_cast<std::uint64_t>(i);  // what the queue assigns
            reference.push_back(e);
        }
        std::sort(reference.begin(), reference.end(),
                  [](const RaceEvent& a, const RaceEvent& b) {
                      return boxbox::eventComesBefore(a, b);
                  });

        bool same = q.size() == reference.size();
        for (std::size_t i = 0; same && i < reference.size(); ++i) {
            const RaceEvent got = q.pop();
            same = got.sequenceId == reference[i].sequenceId &&
                   got.driverNumber == reference[i].driverNumber &&
                   got.type == reference[i].type && got.lap == reference[i].lap &&
                   got.timestamp == reference[i].timestamp;
        }
        ECHECK(same);
        ECHECK(q.empty());
    }

    // Determinism: the same pushes give the same pop order, twice.
    for (int run = 0; run < 2; ++run) {
        EventPriorityQueue q;
        for (int d = 5; d >= 1; --d) {
            q.push(eventAt(2.0, RaceEventType::LapCompleted, d));
        }
        for (int d = 1; d <= 5; ++d) {
            ECHECK(q.pop().driverNumber == d);  // equal time + type: driver number decides
        }
    }
}

void testQueueInterleavedAndLarge() {
    beginSection("EventPriorityQueue: single event, interleaved push / pop / peek, and 100,000 events");

    // One event.
    EventPriorityQueue one;
    one.push(eventAt(3.0, RaceEventType::RaceFinish, 4));
    ECHECK(one.size() == 1U && one.peek().driverNumber == 4);
    ECHECK(one.pop().driverNumber == 4);
    ECHECK(one.empty());
    ECHECK_THROWS(one.pop(), std::out_of_range);

    // Interleaved: 20,000 mixed operations against a vector scanned for the earliest event.
    Lcg rng(2025ULL);
    EventPriorityQueue queue;
    std::vector<RaceEvent> reference;       // events with the sequence id the queue assigned
    std::uint64_t nextSequence = 0;
    const double times[] = {0.0, 1.0, 1.0, 2.5, 7.0, 7.0, 9.0};
    bool agree = true;
    for (int step = 0; step < 20000; ++step) {
        const int action = rng.nextInt(0, 9);
        if (action < 6 || reference.empty()) {
            RaceEvent e = eventAt(times[rng.nextInt(0, 6)], randomType(rng), rng.nextInt(1, 5),
                                  rng.nextInt(1, 30));
            queue.push(e);
            e.sequenceId = nextSequence++;
            reference.push_back(e);
        } else {
            std::size_t best = 0;
            for (std::size_t i = 1; i < reference.size(); ++i) {
                if (boxbox::eventComesBefore(reference[i], reference[best])) best = i;
            }
            if (action < 9) {
                const RaceEvent popped = queue.pop();
                agree = agree && popped.sequenceId == reference[best].sequenceId;
                reference.erase(reference.begin() + static_cast<std::ptrdiff_t>(best));
            } else {
                agree = agree && queue.peek().sequenceId == reference[best].sequenceId;
            }
        }
        agree = agree && queue.size() == reference.size();
    }
    ECHECK(agree);

    // 100,000 events pushed then popped come out in exactly the sorted order.
    EventPriorityQueue big;
    std::vector<RaceEvent> sorted;
    for (int i = 0; i < 100000; ++i) {
        RaceEvent e = eventAt(static_cast<double>(rng.nextInt(0, 3000)), randomType(rng),
                              rng.nextInt(1, 20), rng.nextInt(1, 60));
        big.push(e);
        e.sequenceId = static_cast<std::uint64_t>(i);
        sorted.push_back(e);
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const RaceEvent& a, const RaceEvent& b) { return boxbox::eventComesBefore(a, b); });
    bool inOrder = big.size() == sorted.size();
    for (std::size_t i = 0; inOrder && i < sorted.size(); ++i) {
        inOrder = big.pop().sequenceId == sorted[i].sequenceId;
    }
    ECHECK(inOrder);
    ECHECK(big.empty());

    // processUntil on the big queue's worth of data: everything up to t, in order, nothing after.
    EventPriorityQueue partial;
    for (int i = 0; i < 5000; ++i) {
        partial.push(eventAt(static_cast<double>(rng.nextInt(0, 100)), randomType(rng), rng.nextInt(1, 9)));
    }
    const std::vector<RaceEvent> early = partial.processUntil(40.0);
    bool ordered = true;
    for (std::size_t i = 0; i < early.size(); ++i) {
        ordered = ordered && early[i].timestamp <= 40.0 + boxbox::kTimeEpsilon;
        if (i > 0) ordered = ordered && !boxbox::eventComesBefore(early[i], early[i - 1]);
    }
    ECHECK(ordered && !early.empty());
    ECHECK(partial.peek().timestamp > 40.0);
    ECHECK(early.size() + partial.size() == 5000U);
}

void testProcessUntil() {
    beginSection("EventPriorityQueue::processUntil");

    EventPriorityQueue q;
    q.push(eventAt(3.0, RaceEventType::LapCompleted, 2));
    q.push(eventAt(1.0, RaceEventType::LapCompleted, 1));
    q.push(eventAt(4.0, RaceEventType::LapCompleted, 1));
    q.push(eventAt(3.0, RaceEventType::LapCompleted, 1));
    q.push(eventAt(2.0, RaceEventType::LapCompleted, 1));

    ECHECK(q.processUntil(0.5).empty());
    ECHECK(q.size() == 5U);

    const std::vector<RaceEvent> got = q.processUntil(3.0);  // inclusive of t itself
    ECHECK(got.size() == 4U);
    bool ordered = true;
    for (std::size_t i = 1; i < got.size(); ++i) {
        ordered = ordered && !boxbox::eventComesBefore(got[i], got[i - 1]);
    }
    ECHECK(ordered);
    ECHECK(got.back().timestamp == 3.0);
    ECHECK(q.size() == 1U);
    ECHECK(q.peek().timestamp == 4.0);

    ECHECK(q.processUntil(1000.0).size() == 1U);
    ECHECK(q.empty());
}

void testEventLog() {
    beginSection("EventLog: record, filter, clear");

    EventLog log;
    ECHECK(log.empty());
    log.record(eventAt(1.0, RaceEventType::LapCompleted, 1));
    log.record(eventAt(1.0, RaceEventType::LapCompleted, 2));
    log.record(eventAt(2.0, RaceEventType::PitEntry, 1));
    ECHECK(log.size() == 3U);
    ECHECK(log.entries()[2].type == RaceEventType::PitEntry);  // insertion order kept
    ECHECK(log.entriesForDriver(1).size() == 2U);
    ECHECK(log.entriesForDriver(2).size() == 1U);
    ECHECK(log.entriesForDriver(99).empty());
    ECHECK(log.entriesOfType(RaceEventType::LapCompleted).size() == 2U);
    ECHECK(log.entriesOfType(RaceEventType::RaceFinish).empty());
    log.clear();
    ECHECK(log.empty());
}

void testRaceState() {
    beginSection("RaceState: legal and illegal transitions");

    const auto ev = [](RaceEventType t) { return eventAt(1.0, t, 5); };

    // A full pit stop and the finish.
    RaceState s(5);
    ECHECK(s.current() == DriverRaceState::Racing);
    ECHECK(s.driverNumber() == 5);
    s.apply(ev(RaceEventType::LapCompleted));
    s.apply(ev(RaceEventType::Overtake));
    s.apply(ev(RaceEventType::TrafficHold));
    ECHECK(s.current() == DriverRaceState::Racing);
    s.apply(ev(RaceEventType::PitEntry));
    ECHECK(s.current() == DriverRaceState::PitEntryPending);
    s.apply(ev(RaceEventType::CompoundChange));
    ECHECK(s.current() == DriverRaceState::InPit);
    s.apply(ev(RaceEventType::PitExit));
    ECHECK(s.current() == DriverRaceState::Racing);
    s.apply(ev(RaceEventType::RaceFinish));
    ECHECK(s.current() == DriverRaceState::Finished);

    // Nothing is legal after the finish.
    for (RaceEventType t : kAllTypes) {
        RaceState done(5);
        done.apply(ev(RaceEventType::RaceFinish));
        ECHECK_THROWS(done.apply(ev(t)), IllegalStateTransition);
    }

    // Illegal from Racing.
    ECHECK_THROWS(RaceState(5).apply(ev(RaceEventType::PitExit)), IllegalStateTransition);
    ECHECK_THROWS(RaceState(5).apply(ev(RaceEventType::CompoundChange)), IllegalStateTransition);

    // Illegal from the pit sequence.
    {
        RaceState p(5);
        p.apply(ev(RaceEventType::PitEntry));
        ECHECK_THROWS(p.apply(ev(RaceEventType::PitExit)), IllegalStateTransition);  // skipped tyre fit
        ECHECK_THROWS(p.apply(ev(RaceEventType::LapCompleted)), IllegalStateTransition);
        ECHECK_THROWS(p.apply(ev(RaceEventType::RaceFinish)), IllegalStateTransition);
        ECHECK(p.current() == DriverRaceState::PitEntryPending);  // a rejected event changes nothing
        p.apply(ev(RaceEventType::CompoundChange));
        ECHECK_THROWS(p.apply(ev(RaceEventType::Overtake)), IllegalStateTransition);
        ECHECK_THROWS(p.apply(ev(RaceEventType::PitEntry)), IllegalStateTransition);
        ECHECK(p.current() == DriverRaceState::InPit);
    }

    // An event for another driver is rejected.
    ECHECK_THROWS(RaceState(5).apply(eventAt(1.0, RaceEventType::LapCompleted, 6)),
                  IllegalStateTransition);
}

void testValidateEventLog() {
    beginSection("validateEventLog");

    EventLog empty;
    ECHECK(boxbox::validateEventLog(empty).valid);

    // A hand-built legal log (in canonical order).
    EventLog good;
    good.record(eventAt(1.0, RaceEventType::LapCompleted, 1));
    good.record(eventAt(1.0, RaceEventType::LapCompleted, 2));
    good.record(eventAt(2.0, RaceEventType::PitEntry, 1));
    good.record(eventAt(2.0, RaceEventType::CompoundChange, 1));
    good.record(eventAt(2.0, RaceEventType::PitExit, 1));
    good.record(eventAt(2.0, RaceEventType::LapCompleted, 1));
    good.record(eventAt(3.0, RaceEventType::RaceFinish, 1));
    const boxbox::EventLogValidation ok = boxbox::validateEventLog(good);
    ECHECK(ok.valid);
    ECHECK(ok.message.empty());

    // Illegal transition: PitExit with no PitEntry.
    EventLog illegal;
    illegal.record(eventAt(1.0, RaceEventType::LapCompleted, 1));
    illegal.record(eventAt(2.0, RaceEventType::PitExit, 1));
    const boxbox::EventLogValidation bad = boxbox::validateEventLog(illegal);
    ECHECK(!bad.valid);
    ECHECK(bad.badIndex == 1U);
    ECHECK(!bad.message.empty());

    // Out of canonical order.
    EventLog unordered;
    unordered.record(eventAt(2.0, RaceEventType::LapCompleted, 1));
    unordered.record(eventAt(1.0, RaceEventType::LapCompleted, 1));
    const boxbox::EventLogValidation late = boxbox::validateEventLog(unordered);
    ECHECK(!late.valid);
    ECHECK(late.badIndex == 1U);
}

void testSimulatorEvents() {
    beginSection("Simulator event sink: hand race, counts and ordering");

    const HandRace h;
    EventPriorityQueue queue;
    RaceSimulator sim = buildHandRace(h, &queue);
    sim.runToFinish();
    const EventLog log = drain(queue);

    // 2 cars x 6 laps; 2 stops for car 7; both cars finish.
    ECHECK(countOf(log, RaceEventType::LapCompleted) == 12U);
    ECHECK(countOf(log, RaceEventType::PitEntry) == 2U);
    ECHECK(countOf(log, RaceEventType::CompoundChange) == 2U);
    ECHECK(countOf(log, RaceEventType::PitExit) == 2U);
    ECHECK(countOf(log, RaceEventType::RaceFinish) == 2U);
    ECHECK(countOf(log, RaceEventType::Overtake) == 0U);  // overtaking is off
    ECHECK(countOf(log, RaceEventType::TrafficHold) == 0U);
    ECHECK(boxbox::validateEventLog(log).valid);

    // Hand-worked timeline for car 7. Lap times: A ages 0,1,2 = 10 11 12,
    // B ages 0,1 = 12 12, A age 0 = 10; pit loss 5 after laps 3 and 5.
    //   lap1 10, lap2 21, lap3 33 (+5 = 38), lap4 50, lap5 62 (+5 = 67), lap6 77
    const std::vector<RaceEvent> seven = log.entriesForDriver(7);
    const std::vector<RaceEvent> pits = [&] {
        std::vector<RaceEvent> v;
        for (const RaceEvent& e : seven) {
            if (e.type == RaceEventType::PitEntry) v.push_back(e);
        }
        return v;
    }();
    ECHECK(pits.size() == 2U);
    if (pits.size() == 2U) {
        ECHECK(pits[0].lap == 3);
        ECHECK_NEAR(pits[0].timestamp, 38.0, 1e-9);
        ECHECK(pits[1].lap == 5);
        ECHECK_NEAR(pits[1].timestamp, 67.0, 1e-9);
    }
    const std::vector<RaceEvent> finishes = log.entriesOfType(RaceEventType::RaceFinish);
    bool sevenFinish = false;
    for (const RaceEvent& e : finishes) {
        if (e.driverNumber == 7) {
            sevenFinish = true;
            ECHECK(e.lap == 6);
            ECHECK_NEAR(e.timestamp, 77.0, 1e-9);
        }
    }
    ECHECK(sevenFinish);

    // The compound change records old -> new.
    const std::vector<RaceEvent> changes = log.entriesOfType(RaceEventType::CompoundChange);
    ECHECK(changes.size() == 2U);
    if (changes.size() == 2U) {
        ECHECK(changes[0].oldCompoundIndex == 0 && changes[0].newCompoundIndex == 1);
        ECHECK(changes[1].oldCompoundIndex == 1 && changes[1].newCompoundIndex == 0);
        ECHECK(changes[0].tireAgeAtEvent == 0);
    }

    // The log comes out in canonical order.
    bool ordered = true;
    for (std::size_t i = 1; i < log.entries().size(); ++i) {
        ordered = ordered && !boxbox::eventComesBefore(log.entries()[i], log.entries()[i - 1]);
    }
    ECHECK(ordered);
}

void testEventsDoNotChangeRace() {
    beginSection("Logging events never changes race times (overtaking off and on)");

    const Field f;
    OvertakeRule off;
    OvertakeRule on;
    on.enabled = true;
    on.minGap = 5.0;
    on.passMargin = 10.0;

    for (const OvertakeRule& rule : {off, on}) {
        EventPriorityQueue queue;
        RaceSimulator logged = buildField(f, &queue, rule);
        RaceSimulator quiet = buildField(f, nullptr, rule);
        logged.runToFinish();
        quiet.runToFinish();

        bool same = logged.drivers().size() == quiet.drivers().size();
        for (std::size_t i = 0; same && i < logged.drivers().size(); ++i) {
            same = logged.drivers()[i].cumulativeTime() == quiet.drivers()[i].cumulativeTime() &&
                   logged.drivers()[i].trafficLoss() == quiet.drivers()[i].trafficLoss() &&
                   logged.drivers()[i].stopsMade() == quiet.drivers()[i].stopsMade();
        }
        ECHECK(same);
        ECHECK(!queue.empty());
    }
}

void testTrafficEvents() {
    beginSection("Overtake / TrafficHold / RaceFinish events match the simulator's records");

    const Field f;

    // Hold-heavy: big gap, impossible pass margin.
    {
        OvertakeRule rule;
        rule.enabled = true;
        rule.minGap = 5.0;
        rule.passMargin = 10.0;
        EventPriorityQueue queue;
        RaceSimulator sim = buildField(f, &queue, rule);
        sim.runToFinish();
        const EventLog log = drain(queue);

        ECHECK(countOf(log, RaceEventType::TrafficHold) > 0U);
        ECHECK(countOf(log, RaceEventType::Overtake) == 0U);
        ECHECK(countOf(log, RaceEventType::RaceFinish) == sim.drivers().size());
        ECHECK(boxbox::validateEventLog(log).valid);

        for (const boxbox::Driver& driver : sim.drivers()) {
            double held = 0.0;
            bool sane = true;
            for (const RaceEvent& e : log.entriesForDriver(driver.number())) {
                if (e.type == RaceEventType::TrafficHold) {
                    held += e.holdLossSeconds;
                    sane = sane && e.holdLossSeconds > 0.0 && e.otherDriverNumber >= 1 &&
                           e.otherDriverNumber != driver.number();
                }
            }
            ECHECK(sane);
            ECHECK_NEAR(held, driver.trafficLoss(), 1e-9);
        }
    }

    // Pass-heavy: any caught car with at least the pace of the car ahead passes.
    {
        OvertakeRule rule;
        rule.enabled = true;
        rule.minGap = 5.0;
        rule.passMargin = 0.0;
        EventPriorityQueue queue;
        RaceSimulator sim = buildField(f, &queue, rule);
        sim.runToFinish();
        const EventLog log = drain(queue);

        ECHECK(countOf(log, RaceEventType::Overtake) > 0U);
        ECHECK(boxbox::validateEventLog(log).valid);
        for (const RaceEvent& e : log.entriesOfType(RaceEventType::Overtake)) {
            ECHECK(e.otherDriverNumber >= 1 && e.otherDriverNumber != e.driverNumber);
        }
    }

    // Each driver's RaceFinish is its last event, on its last lap, at its final time.
    {
        OvertakeRule off;
        EventPriorityQueue queue;
        RaceSimulator sim = buildField(f, &queue, off);
        sim.runToFinish();
        const EventLog log = drain(queue);
        for (const boxbox::Driver& driver : sim.drivers()) {
            const std::vector<RaceEvent> mine = log.entriesForDriver(driver.number());
            ECHECK(!mine.empty());
            if (mine.empty()) continue;
            ECHECK(mine.back().type == RaceEventType::RaceFinish);
            ECHECK(mine.back().lap == f.track.totalLaps());
            ECHECK(mine.back().timestamp == driver.cumulativeTime());
        }
    }
}

void testPositionAndAttemptEvents() {
    beginSection("PositionChange and OvertakeAttempt events");

    const Field f;
    OvertakeRule busy;
    busy.enabled = true;
    busy.minGap = 5.0;
    busy.passMargin = 0.0;  // a caught car with any pace edge passes: many attempts and passes
    OvertakeRule held;
    held.enabled = true;
    held.minGap = 5.0;
    held.passMargin = 10.0;  // nobody can pass: attempts all become holds

    for (const OvertakeRule& rule : {OvertakeRule{}, busy, held}) {
        EventPriorityQueue queue;
        RaceSimulator sim = buildField(f, &queue, rule);
        sim.runToFinish();
        const EventLog log = drain(queue);
        ECHECK(boxbox::validateEventLog(log).valid);

        // Every attempt is resolved by exactly one pass or hold, on the same car and lap.
        const std::size_t attempts = countOf(log, RaceEventType::OvertakeAttempt);
        ECHECK(attempts == countOf(log, RaceEventType::Overtake) + countOf(log, RaceEventType::TrafficHold));
        if (!rule.enabled) {
            ECHECK(attempts == 0U);
        }
        if (&rule == &held) {
            ECHECK(countOf(log, RaceEventType::Overtake) == 0U);
        }

        // Position chains: each change starts where the car's last place ended, the
        // chain begins at the grid (car-number order), and it ends at the final standings.
        std::vector<int> gridOrder;
        for (const boxbox::Driver& d : sim.drivers()) gridOrder.push_back(d.number());
        std::sort(gridOrder.begin(), gridOrder.end());
        const std::vector<boxbox::Standing> finalStandings = sim.standings();

        std::size_t changes = 0;
        for (const boxbox::Driver& driver : sim.drivers()) {
            int expectedFrom = static_cast<int>(
                std::find(gridOrder.begin(), gridOrder.end(), driver.number()) - gridOrder.begin()) + 1;
            int last = expectedFrom;
            bool chained = true;
            bool differs = true;
            for (const RaceEvent& e : log.entriesForDriver(driver.number())) {
                if (e.type != RaceEventType::PositionChange) continue;
                ++changes;
                chained = chained && e.oldPosition == last;
                differs = differs && e.newPosition != e.oldPosition;
                last = e.newPosition;
            }
            ECHECK(chained);
            ECHECK(differs);
            int finalPosition = 0;
            for (const boxbox::Standing& row : finalStandings) {
                if (sim.drivers()[row.driverIndex].number() == driver.number()) finalPosition = row.position;
            }
            ECHECK(last == finalPosition);
        }
        ECHECK(changes == countOf(log, RaceEventType::PositionChange));
    }

    // Hand case: two cars, the one that stops once loses the lead and regains it.
    const HandRace h;
    EventPriorityQueue queue;
    RaceSimulator sim = buildHandRace(h, &queue);
    sim.runToFinish();
    const EventLog log = drain(queue);
    // Car 7 stops after laps 3 and 5 (5 s each); car 9 (12 s laps, no stops) is 12 s/lap.
    // After lap 1: 7 at 10 s, 9 at 12 s -> 7 leads. After lap 3: 7 at 33+5 = 38, 9 at 36 -> 9 leads.
    std::vector<RaceEvent> moves;
    for (const RaceEvent& e : log.entriesOfType(RaceEventType::PositionChange)) moves.push_back(e);
    ECHECK(!moves.empty());
    bool nineTakesLead = false;
    for (const RaceEvent& e : moves) {
        if (e.driverNumber == 9 && e.lap == 3 && e.oldPosition == 2 && e.newPosition == 1) nineTakesLead = true;
    }
    ECHECK(nineTakesLead);

    // reset() forgets the baseline, so a rerun reproduces the same events.
    EventPriorityQueue again;
    RaceSimulator rerun = buildHandRace(h, &again);
    rerun.runToFinish();
    const EventLog first = drain(again);
    rerun.reset();
    rerun.runToFinish();
    const EventLog second = drain(again);
    ECHECK(first.size() == second.size());
}

void testPitAnalyticsHandCase() {
    beginSection("PitAnalytics: hand-worked race (3 stints, then 1 stint)");

    const HandRace h;
    EventPriorityQueue queue;
    RaceSimulator sim = buildHandRace(h, &queue);
    sim.runToFinish();
    const EventLog log = drain(queue);
    const PitAnalytics analytics(log);

    const DriverPitSummary seven = analytics.summaryFor(7);
    ECHECK(seven.driverNumber == 7);
    ECHECK(seven.stopCount == 2);
    ECHECK((seven.stopLaps == std::vector<int>{3, 5}));
    ECHECK((seven.stintLengthsLaps == std::vector<int>{3, 2, 1}));  // first AND last stint included
    ECHECK((seven.compoundIndicesUsed == std::vector<int>{0, 1, 0}));
    ECHECK_NEAR(analytics.totalPitLoss(7, 5.0), 10.0, 1e-9);

    const DriverPitSummary nine = analytics.summaryFor(9);
    ECHECK(nine.stopCount == 0);
    ECHECK(nine.stopLaps.empty());
    ECHECK((nine.stintLengthsLaps == std::vector<int>{6}));       // a no-stop race is one full stint
    ECHECK((nine.compoundIndicesUsed == std::vector<int>{1}));
    ECHECK_NEAR(analytics.totalPitLoss(9, 5.0), 0.0, 0.0);

    // allSummaries: first-appearance order, and each entry equals summaryFor.
    const std::vector<DriverPitSummary> all = analytics.allSummaries();
    ECHECK(all.size() == 2U);
    if (all.size() == 2U) {
        ECHECK(all[0].driverNumber == 7);  // driver 7 appears first in the log (tie on time -> lower number)
        ECHECK(all[1].driverNumber == 9);
        ECHECK(all[0].stintLengthsLaps == seven.stintLengthsLaps);
        ECHECK(all[1].stintLengthsLaps == nine.stintLengthsLaps);
        ECHECK(all[0].compoundIndicesUsed == seven.compoundIndicesUsed);
        ECHECK(all[0].stopLaps == seven.stopLaps);
    }

    const PitAnalytics::PitWindow window = analytics.pitWindow();
    ECHECK(window.hasStops);
    ECHECK(window.earliestStopLap == 3);
    ECHECK(window.latestStopLap == 5);

    // Unknown driver: an empty, zero summary rather than a failure.
    const DriverPitSummary ghost = analytics.summaryFor(404);
    ECHECK(ghost.stopCount == 0);
    ECHECK(ghost.stintLengthsLaps.empty());

    // Empty log.
    const EventLog empty;
    const PitAnalytics none(empty);
    ECHECK(none.allSummaries().empty());
    ECHECK(!none.pitWindow().hasStops);
}

void testStintStatsAndHistogram() {
    beginSection("PitAnalytics: stint statistics, histogram and field pit loss");

    const HandRace h;
    EventPriorityQueue queue;
    RaceSimulator sim = buildHandRace(h, &queue);
    sim.runToFinish();
    const EventLog log = drain(queue);
    const PitAnalytics analytics(log);

    // Car 7 ran stints of 3, 2, 1 laps; car 9 one of 6.
    const boxbox::StintStats seven = PitAnalytics::stintStats(analytics.summaryFor(7));
    ECHECK(seven.count == 3 && seven.shortestLaps == 1 && seven.longestLaps == 3);
    ECHECK_NEAR(seven.meanLaps, 2.0, 1e-12);
    const boxbox::StintStats nine = PitAnalytics::stintStats(analytics.summaryFor(9));
    ECHECK(nine.count == 1 && nine.shortestLaps == 6 && nine.longestLaps == 6);
    const boxbox::StintStats field = analytics.fieldStintStats();
    ECHECK(field.count == 4 && field.shortestLaps == 1 && field.longestLaps == 6);
    ECHECK_NEAR(field.meanLaps, 3.0, 1e-12);

    const boxbox::StintStats none = PitAnalytics::stintStats(DriverPitSummary{});
    ECHECK(none.count == 0 && none.shortestLaps == 0 && none.longestLaps == 0);
    ECHECK_NEAR(none.meanLaps, 0.0, 0.0);

    // Lengths 3 2 1 6. Width 1: six bins 1..6. Width 2: [1-2]=2, [3-4]=1, [5-6]=1.
    const std::vector<boxbox::StintHistogramBin> one = analytics.stintHistogram(1);
    ECHECK(one.size() == 6U);
    if (one.size() == 6U) {
        const int expected[] = {1, 1, 1, 0, 0, 1};
        for (std::size_t i = 0; i < 6U; ++i) {
            ECHECK(one[i].count == expected[i]);
            ECHECK(one[i].firstLaps == static_cast<int>(i) + 1 && one[i].lastLaps == one[i].firstLaps);
        }
    }
    const std::vector<boxbox::StintHistogramBin> two = analytics.stintHistogram(2);
    ECHECK(two.size() == 3U);
    if (two.size() == 3U) {
        ECHECK(two[0].count == 2 && two[0].firstLaps == 1 && two[0].lastLaps == 2);
        ECHECK(two[1].count == 1 && two[1].firstLaps == 3 && two[1].lastLaps == 4);
        ECHECK(two[2].count == 1 && two[2].firstLaps == 5 && two[2].lastLaps == 6);
    }
    ECHECK(analytics.stintHistogram(100).size() == 1U);
    ECHECK_THROWS(analytics.stintHistogram(0), std::invalid_argument);
    ECHECK_THROWS(analytics.stintHistogram(-3), std::invalid_argument);
    const EventLog empty;
    ECHECK(PitAnalytics(empty).stintHistogram(3).empty());
    ECHECK(PitAnalytics(empty).fieldStintStats().count == 0);

    // The whole field: two stops at 5 s each.
    ECHECK_NEAR(analytics.fieldPitLoss(5.0), 10.0, 1e-12);
    ECHECK_NEAR(PitAnalytics(empty).fieldPitLoss(5.0), 0.0, 0.0);

    // Seeded field: histogram counts add up, and the stats agree with a direct pass.
    const Field f;
    for (const int width : {1, 3, 7}) {
        OvertakeRule off;
        EventPriorityQueue q;
        RaceSimulator s = buildField(f, &q, off);
        s.runToFinish();
        const EventLog fl = drain(q);
        const PitAnalytics a(fl);
        int total = 0;
        for (const boxbox::StintHistogramBin& b : a.stintHistogram(width)) total += b.count;
        ECHECK(total == a.fieldStintStats().count);
        int stints = 0;
        for (const boxbox::Driver& d : s.drivers()) stints += static_cast<int>(d.plan().size());
        ECHECK(total == stints);
    }
}

void testChronologicalAndWindows() {
    beginSection("PitAnalytics: chronological stops, prefix-sum index and window summaries");

    const HandRace h;
    EventPriorityQueue queue;
    RaceSimulator sim = buildHandRace(h, &queue);
    sim.runToFinish();
    const EventLog log = drain(queue);
    const PitAnalytics analytics(log);

    const std::vector<boxbox::PitStopRecord> stops = analytics.chronologicalPitStops();
    ECHECK(stops.size() == 2U);
    if (stops.size() == 2U) {
        ECHECK(stops[0].driverNumber == 7 && stops[0].lap == 3);
        ECHECK_NEAR(stops[0].time, 38.0, 1e-9);
        ECHECK(stops[0].fromCompound == 0 && stops[0].toCompound == 1);
        ECHECK(stops[1].driverNumber == 7 && stops[1].lap == 5);
        ECHECK_NEAR(stops[1].time, 67.0, 1e-9);
        ECHECK(stops[1].fromCompound == 1 && stops[1].toCompound == 0);
    }

    // Prefix sums: stops at the end of laps 3 and 5 of 6.
    const boxbox::StopLapIndex index = analytics.buildStopIndex(6);
    ECHECK(index.raceLaps() == 6);
    const int cumulative[] = {0, 0, 0, 1, 1, 2, 2};
    for (int lap = 0; lap <= 6; ++lap) {
        ECHECK(index.cumulativeStops(lap) == cumulative[lap]);
    }
    ECHECK(index.stopsInLaps(1, 6) == 2);
    ECHECK(index.stopsInLaps(3, 3) == 1);
    ECHECK(index.stopsInLaps(4, 5) == 1);
    ECHECK(index.stopsInLaps(1, 2) == 0);
    ECHECK_THROWS(index.stopsInLaps(0, 3), std::out_of_range);
    ECHECK_THROWS(index.stopsInLaps(4, 3), std::out_of_range);
    ECHECK_THROWS(index.stopsInLaps(1, 7), std::out_of_range);
    ECHECK_THROWS(index.cumulativeStops(-1), std::out_of_range);
    ECHECK_THROWS(index.cumulativeStops(7), std::out_of_range);
    ECHECK_THROWS(analytics.buildStopIndex(4), std::invalid_argument);   // a stop on lap 5
    ECHECK_THROWS(analytics.buildStopIndex(-1), std::invalid_argument);
    const EventLog empty;
    ECHECK(PitAnalytics(empty).buildStopIndex(0).cumulativeStops(0) == 0);
    ECHECK(PitAnalytics(empty).buildStopIndex(5).stopsInLaps(1, 5) == 0);

    // Window summaries.
    const boxbox::PitWindowSummary all = analytics.windowSummary(1, 6, 5.0);
    ECHECK(all.stops == 2 && all.drivers == 1 && all.earliestLap == 3 && all.latestLap == 5);
    ECHECK_NEAR(all.pitLossTotal, 10.0, 1e-12);
    const boxbox::PitWindowSummary quiet = analytics.windowSummary(4, 4, 5.0);
    ECHECK(quiet.stops == 0 && quiet.drivers == 0 && quiet.earliestLap == 0);
    ECHECK(analytics.windowSummary(5, 100, 5.0).stops == 1);
    ECHECK_THROWS(analytics.windowSummary(0, 3, 5.0), std::invalid_argument);
    ECHECK_THROWS(analytics.windowSummary(5, 4, 5.0), std::invalid_argument);

    // Seeded field: the O(1) prefix answer, the scanning summary and a direct count agree.
    const Field f;
    OvertakeRule off;
    EventPriorityQueue q;
    RaceSimulator s = buildField(f, &q, off);
    s.runToFinish();
    const EventLog fl = drain(q);
    const PitAnalytics a(fl);
    const boxbox::StopLapIndex idx = a.buildStopIndex(f.track.totalLaps());
    std::vector<int> stopLaps;
    for (const boxbox::DriverPitSummary& d : a.allSummaries()) {
        stopLaps.insert(stopLaps.end(), d.stopLaps.begin(), d.stopLaps.end());
    }
    Lcg rng(909ULL);
    for (int trial = 0; trial < 200; ++trial) {
        const int from = rng.nextInt(1, f.track.totalLaps());
        const int to = rng.nextInt(from, f.track.totalLaps());
        int direct = 0;
        for (const int lap : stopLaps) direct += (lap >= from && lap <= to) ? 1 : 0;
        ECHECK(idx.stopsInLaps(from, to) == direct);
        const boxbox::PitWindowSummary w = a.windowSummary(from, to, f.track.pitLoss());
        ECHECK(w.stops == direct);
        ECHECK_NEAR(w.pitLossTotal, static_cast<double>(direct) * f.track.pitLoss(), 1e-9);
        ECHECK(w.drivers <= static_cast<int>(s.drivers().size()));
    }
    ECHECK(idx.stopsInLaps(1, f.track.totalLaps()) == static_cast<int>(stopLaps.size()));

    // Chronological order: nondecreasing in time, and every stop is present.
    const std::vector<boxbox::PitStopRecord> fieldStops = a.chronologicalPitStops();
    ECHECK(fieldStops.size() == stopLaps.size());
    bool sorted = true;
    bool compoundsKnown = true;
    for (std::size_t i = 0; i < fieldStops.size(); ++i) {
        if (i > 0) sorted = sorted && !(fieldStops[i].time < fieldStops[i - 1].time);
        compoundsKnown = compoundsKnown && fieldStops[i].fromCompound >= 0 && fieldStops[i].toCompound >= 0;
    }
    ECHECK(sorted);
    ECHECK(compoundsKnown);
}

void testCoverageAndGreedyWindows() {
    beginSection("PitAnalytics: interval coverage and the greedy stop-window problem");

    using boxbox::StintInterval;
    const auto tiles = [](std::vector<StintInterval> s, int laps) {
        return PitAnalytics::checkStintCoverage(std::move(s), laps);
    };
    ECHECK(tiles({{1, 3}, {4, 5}}, 5).tiles);
    ECHECK(tiles({{4, 5}, {1, 3}}, 5).tiles);          // order does not matter
    ECHECK(tiles({}, 0).tiles);                         // no race, no stints
    ECHECK(tiles({{1, 1}}, 1).tiles);
    ECHECK(tiles({{1, 5}}, 5).message.empty());
    boxbox::CoverageReport r = tiles({{1, 2}, {4, 5}}, 5);
    ECHECK(!r.tiles && r.message.find("3-3") != std::string::npos);        // gap
    r = tiles({{1, 3}, {3, 5}}, 5);
    ECHECK(!r.tiles && r.message.find("overlaps") != std::string::npos);   // overlap
    r = tiles({{1, 3}}, 5);
    ECHECK(!r.tiles && r.message.find("4-5") != std::string::npos);        // short
    r = tiles({{1, 6}}, 5);
    ECHECK(!r.tiles && r.message.find("past the end") != std::string::npos);
    r = tiles({{2, 5}}, 5);
    ECHECK(!r.tiles && r.message.find("1-1") != std::string::npos);        // starts late
    ECHECK(!tiles({}, 3).tiles);
    ECHECK(!tiles({{0, 3}}, 3).tiles);                  // laps are 1-based
    ECHECK(!tiles({{3, 2}}, 3).tiles);                  // reversed interval

    // Every car's reported stints tile the race.
    const Field f;
    for (const OvertakeRule& rule : {OvertakeRule{}}) {
        EventPriorityQueue q;
        RaceSimulator s = buildField(f, &q, rule);
        s.runToFinish();
        const EventLog log = drain(q);
        const PitAnalytics a(log);
        for (const boxbox::DriverPitSummary& d : a.allSummaries()) {
            ECHECK(tiles(PitAnalytics::stintIntervals(d), f.track.totalLaps()).tiles);
        }
    }

    // Greedy stabbing. Hand case: [1,3] [2,5] [4,6] need two laps, e.g. 3 and 6.
    using boxbox::StopWindow;
    const std::vector<int> hand = PitAnalytics::minimumStopLaps({{1, 1, 3}, {2, 2, 5}, {3, 4, 6}});
    ECHECK((hand == std::vector<int>{3, 6}));
    ECHECK(PitAnalytics::minimumStopLaps({}).empty());
    ECHECK((PitAnalytics::minimumStopLaps({{1, 4, 4}}) == std::vector<int>{4}));
    ECHECK((PitAnalytics::minimumStopLaps({{1, 2, 9}, {2, 3, 8}, {3, 5, 6}}) == std::vector<int>{6}));
    ECHECK_THROWS(PitAnalytics::minimumStopLaps({{1, 5, 4}}), std::invalid_argument);
    ECHECK_THROWS(PitAnalytics::minimumStopLaps({{1, 0, 4}}), std::invalid_argument);

    // Against brute force over every subset of laps 1..9, 150 seeded window sets.
    Lcg rng(515ULL);
    for (int trial = 0; trial < 150; ++trial) {
        std::vector<StopWindow> windows;
        const int count = rng.nextInt(1, 7);
        for (int i = 0; i < count; ++i) {
            const int lo = rng.nextInt(1, 9);
            windows.push_back(StopWindow{i + 1, lo, rng.nextInt(lo, 9)});
        }
        const std::vector<int> greedy = PitAnalytics::minimumStopLaps(windows);
        const auto stabs = [&](unsigned mask) {
            for (const StopWindow& w : windows) {
                bool hit = false;
                for (int lap = w.earliestLap; lap <= w.latestLap; ++lap) {
                    hit = hit || (mask >> (lap - 1)) & 1U;
                }
                if (!hit) return false;
            }
            return true;
        };
        int best = 99;
        for (unsigned mask = 0; mask < (1U << 9); ++mask) {
            if (stabs(mask)) {
                int bits = 0;
                for (unsigned m = mask; m != 0; m >>= 1) bits += static_cast<int>(m & 1U);
                best = std::min(best, bits);
            }
        }
        ECHECK(static_cast<int>(greedy.size()) == best);
        unsigned chosenMask = 0;
        for (const int lap : greedy) chosenMask |= 1U << (lap - 1);
        ECHECK(stabs(chosenMask));                                  // and it really covers everyone
        ECHECK(std::is_sorted(greedy.begin(), greedy.end()));
    }
}

void testStopTimingAndTrafficAfterStops() {
    beginSection("PitAnalytics: stop timing comparison and traffic after a stop");

    const HandRace h;
    EventPriorityQueue queue;
    RaceSimulator sim = buildHandRace(h, &queue);
    sim.runToFinish();
    const EventLog log = drain(queue);
    const PitAnalytics analytics(log);
    const boxbox::StopTimingComparison c = analytics.compareStopTiming(7, 9);
    ECHECK(c.rows.empty() && c.extraStopsA == 2 && c.extraStopsB == 0);
    const boxbox::StopTimingComparison self = analytics.compareStopTiming(7, 7);
    ECHECK(self.rows.size() == 2U && self.extraStopsA == 0 && self.extraStopsB == 0);
    if (self.rows.size() == 2U) {
        ECHECK(self.rows[0].lapDifference == 0 && self.rows[1].stopIndex == 2);
    }
    ECHECK(analytics.compareStopTiming(404, 9).rows.empty());

    // Two cars that stop on different laps.
    const Field f;
    OvertakeRule off;
    EventPriorityQueue q;
    RaceSimulator s = buildField(f, &q, off);
    s.runToFinish();
    const EventLog fl = drain(q);
    const PitAnalytics a(fl);
    const boxbox::DriverPitSummary one = a.summaryFor(1);
    const boxbox::DriverPitSummary two = a.summaryFor(2);
    const boxbox::StopTimingComparison cmp = a.compareStopTiming(1, 2);
    const std::size_t shared = std::min(one.stopLaps.size(), two.stopLaps.size());
    ECHECK(cmp.rows.size() == shared);
    for (std::size_t i = 0; i < cmp.rows.size(); ++i) {
        ECHECK(cmp.rows[i].lapA == one.stopLaps[i] && cmp.rows[i].lapB == two.stopLaps[i]);
        ECHECK(cmp.rows[i].lapDifference == cmp.rows[i].lapA - cmp.rows[i].lapB);
    }
    ECHECK(cmp.extraStopsA == static_cast<int>(one.stopLaps.size() - shared));

    // Traffic after a stop, hand worked. Car 5 stops after laps 2 and 4 of 6.
    const auto lap = [](int car, int number, double traffic, bool stop) {
        boxbox::LapRecord r;
        r.driverNumber = car;
        r.lap = number;
        r.trafficTime = traffic;
        r.lapTime = 90.0;
        r.pitStop = stop;
        return r;
    };
    const std::vector<boxbox::LapRecord> records{
        lap(5, 1, 0.0, false), lap(8, 1, 0.1, true),  lap(5, 2, 0.0, true),  lap(8, 2, 0.2, false),
        lap(5, 3, 0.5, false), lap(8, 3, 0.3, false), lap(5, 4, 0.25, true), lap(8, 4, 0.4, false),
        lap(5, 5, 0.0, false), lap(5, 6, 1.0, false)};
    const std::vector<boxbox::TrafficAfterStop> after = PitAnalytics::trafficAfterStops(records, 2);
    ECHECK(after.size() == 3U);
    if (after.size() == 3U) {
        // Ordered by car, then stop lap: (5, lap 2), (5, lap 4), (8, lap 1).
        ECHECK(after[0].driverNumber == 5 && after[0].stopLap == 2 && after[0].lapsConsidered == 2);
        ECHECK_NEAR(after[0].trafficSeconds, 0.75, 1e-12);      // laps 3 and 4: 0.5 + 0.25
        ECHECK(after[1].driverNumber == 5 && after[1].stopLap == 4 && after[1].lapsConsidered == 2);
        ECHECK_NEAR(after[1].trafficSeconds, 1.0, 1e-12);       // laps 5 and 6: 0 + 1.0
        ECHECK(after[2].driverNumber == 8 && after[2].stopLap == 1 && after[2].lapsConsidered == 2);
        ECHECK_NEAR(after[2].trafficSeconds, 0.5, 1e-12);       // laps 2 and 3: 0.2 + 0.3
    }
    const std::vector<boxbox::TrafficAfterStop> longer = PitAnalytics::trafficAfterStops(records, 5);
    ECHECK(longer.size() == 3U && longer[1].lapsConsidered == 2);   // the race ends first
    ECHECK(PitAnalytics::trafficAfterStops({}, 3).empty());
    ECHECK_THROWS(PitAnalytics::trafficAfterStops(records, 0), std::invalid_argument);

    // Real race with traffic: against a direct sum over the recorded laps.
    OvertakeRule busy;
    busy.enabled = true;
    busy.minGap = 5.0;
    busy.passMargin = 10.0;
    RaceSimulator live = buildField(f, nullptr, busy);
    boxbox::LapRecorder recorder(live);
    while (live.advanceLap()) recorder.observe(live);
    for (const int window : {1, 3, 8}) {
        const std::vector<boxbox::TrafficAfterStop> got =
            PitAnalytics::trafficAfterStops(recorder.records(), window);
        std::size_t stopsSeen = 0;
        for (const boxbox::LapRecord& r : recorder.records()) stopsSeen += r.pitStop ? 1U : 0U;
        ECHECK(got.size() == stopsSeen);
        for (const boxbox::TrafficAfterStop& t : got) {
            double direct = 0.0;
            int counted = 0;
            for (const boxbox::LapRecord& r : recorder.records()) {
                if (r.driverNumber == t.driverNumber && r.lap > t.stopLap && r.lap <= t.stopLap + window) {
                    direct += r.trafficTime;
                    ++counted;
                }
            }
            ECHECK_NEAR(t.trafficSeconds, direct, 1e-9);
            ECHECK(t.lapsConsidered == counted);
            ECHECK(t.trafficSeconds >= 0.0);
        }
    }
}

void testPitAnalyticsAgainstSimulator() {
    beginSection("PitAnalytics agrees with the simulator's drivers and plans, with and without traffic");

    const Field f;
    OvertakeRule off;
    OvertakeRule on;
    on.enabled = true;
    on.minGap = 5.0;
    on.passMargin = 10.0;

    for (const OvertakeRule& rule : {off, on}) {
        EventPriorityQueue queue;
        RaceSimulator sim = buildField(f, &queue, rule);
        sim.runToFinish();
        const EventLog log = drain(queue);
        const PitAnalytics analytics(log);

        ECHECK(analytics.allSummaries().size() == sim.drivers().size());
        for (const boxbox::Driver& driver : sim.drivers()) {
            const DriverPitSummary s = analytics.summaryFor(driver.number());
            ECHECK(s.stopCount == driver.stopsMade());
            ECHECK(s.stintLengthsLaps.size() == driver.plan().size());  // stints tile the race
            ECHECK(s.compoundIndicesUsed.size() == driver.plan().size());
            int total = 0;
            for (int laps : s.stintLengthsLaps) total += laps;
            ECHECK(total == f.track.totalLaps());

            bool match = s.stintLengthsLaps.size() == driver.plan().size() &&
                         s.compoundIndicesUsed.size() == driver.plan().size();
            int lapsSoFar = 0;
            for (std::size_t i = 0; match && i < driver.plan().size(); ++i) {
                match = s.stintLengthsLaps[i] == driver.plan()[i].laps &&
                        s.compoundIndicesUsed[i] == static_cast<int>(driver.plan()[i].compoundIndex);
                lapsSoFar += driver.plan()[i].laps;
                if (i + 1 < driver.plan().size()) {
                    match = match && s.stopLaps.size() > i && s.stopLaps[i] == lapsSoFar;
                }
            }
            ECHECK(match);
            ECHECK_NEAR(analytics.totalPitLoss(driver.number(), f.track.pitLoss()),
                        static_cast<double>(driver.stopsMade()) * f.track.pitLoss(), 1e-9);
        }
    }
}

void testUndercutOvercut() {
    beginSection("PitAnalytics::compareUndercutOvercut: hand case and argument checks");

    // One 10 s sector, 6 laps, 5 s pit loss; compound A adds 1 s per lap of age.
    Track track("U", 6, 5.0);
    track.addSector("S1", 10.0);
    const std::vector<TireCompound> compounds{TireCompound("A", 0.0, 1.0, 6)};
    const PitStrategy strategy(track, compounds);

    // earlier: 1 lap then 5 laps  = 10 + (10+11+12+13+14) + 5 = 75
    // later:   3 laps then 3 laps = 2 * (10+11+12)        + 5 = 71
    const std::vector<PlannedStint> earlier{PlannedStint{0, 1}, PlannedStint{0, 5}};
    const std::vector<PlannedStint> later{PlannedStint{0, 3}, PlannedStint{0, 3}};
    const boxbox::UndercutOvercutResult r =
        PitAnalytics::compareUndercutOvercut(strategy, earlier, later, 1, 3);
    ECHECK(r.earlierStopLap == 1);
    ECHECK(r.laterStopLap == 3);
    ECHECK_NEAR(r.earlierPlanTotalTime, 75.0, 1e-9);
    ECHECK_NEAR(r.laterPlanTotalTime, 71.0, 1e-9);
    ECHECK_NEAR(r.timeDeltaSeconds, -4.0, 1e-9);  // negative: the later stop is faster

    // Each result matches an independent evaluatePlan call.
    ECHECK_NEAR(r.earlierPlanTotalTime, strategy.evaluatePlan(earlier).totalTime, 1e-9);
    ECHECK_NEAR(r.laterPlanTotalTime, strategy.evaluatePlan(later).totalTime, 1e-9);

    // Argument checks.
    ECHECK_THROWS(PitAnalytics::compareUndercutOvercut(strategy, earlier, later, 3, 1),
                  std::invalid_argument);  // wrong order
    ECHECK_THROWS(PitAnalytics::compareUndercutOvercut(strategy, earlier, later, 3, 3),
                  std::invalid_argument);  // not distinct
    ECHECK_THROWS(PitAnalytics::compareUndercutOvercut(strategy, earlier, later, 2, 3),
                  std::invalid_argument);  // earlier plan does not stop at lap 2
    ECHECK_THROWS(PitAnalytics::compareUndercutOvercut(strategy, {}, later, 1, 3),
                  std::invalid_argument);

    // An infeasible plan (a stint longer than the compound allows) is reported
    // as unreachable, never as a number.
    const std::vector<PlannedStint> tooLong{PlannedStint{0, 3}, PlannedStint{0, 9}};
    const boxbox::UndercutOvercutResult inf =
        PitAnalytics::compareUndercutOvercut(strategy, earlier, tooLong, 1, 3);
    ECHECK(!boxbox::isReachable(inf.laterPlanTotalTime));
    ECHECK(!boxbox::isReachable(inf.timeDeltaSeconds));
}

void testDeterminism() {
    beginSection("Event streams are identical run to run");

    const Field f;
    OvertakeRule on;
    on.enabled = true;
    on.minGap = 5.0;
    on.passMargin = 1.0;

    std::vector<RaceEvent> first;
    for (int run = 0; run < 2; ++run) {
        EventPriorityQueue queue;
        RaceSimulator sim = buildField(f, &queue, on);
        sim.runToFinish();
        const EventLog log = drain(queue);
        if (run == 0) {
            first = log.entries();
        } else {
            bool same = log.entries().size() == first.size();
            for (std::size_t i = 0; same && i < first.size(); ++i) {
                const RaceEvent& a = first[i];
                const RaceEvent& b = log.entries()[i];
                same = a.type == b.type && a.timestamp == b.timestamp &&
                       a.driverNumber == b.driverNumber && a.lap == b.lap &&
                       a.sequenceId == b.sequenceId && a.otherDriverNumber == b.otherDriverNumber &&
                       a.holdLossSeconds == b.holdLossSeconds;
            }
            ECHECK(same);
        }
    }
}

}  // namespace

EventTestTally runEventEngineTests() {
    g_tally = EventTestTally();

    testEventOrdering();
    testQueueBasics();
    testQueueAgainstSort();
    testQueueInterleavedAndLarge();
    testProcessUntil();
    testEventLog();
    testRaceState();
    testValidateEventLog();
    testSimulatorEvents();
    testEventsDoNotChangeRace();
    testTrafficEvents();
    testPositionAndAttemptEvents();
    testPitAnalyticsHandCase();
    testStintStatsAndHistogram();
    testChronologicalAndWindows();
    testCoverageAndGreedyWindows();
    testStopTimingAndTrafficAfterStops();
    testPitAnalyticsAgainstSimulator();
    testUndercutOvercut();
    testDeterminism();

    return g_tally;
}
