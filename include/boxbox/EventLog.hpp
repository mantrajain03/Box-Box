#ifndef BOXBOX_EVENTLOG_HPP
#define BOXBOX_EVENTLOG_HPP

#include <vector>
#include "boxbox/RaceEvent.hpp"

/// Deterministic, append-only record of every RaceEvent that occurred
/// during a race (owned by Pratyaksh).
///
/// EventLog is a pure "record and retrieve" structure. It does not alter
/// driver race times, does not validate transitions (RaceState does that),
/// and does not interpret events (PitAnalytics does that). It exists so a
/// completed race can be inspected, printed, or handed to PitAnalytics
/// after the fact.
///
/// Knows nothing about Driver, RaceSimulator or Track, so it is
/// constructible and testable on its own (NFR-3).
namespace boxbox {

class EventLog {
public:
    EventLog() = default;

    /// Append one event. O(1) amortised.
    /// Events should be appended in the order EventPriorityQueue released
    /// them, so the log's own order is already the canonical processing
    /// order - the log does not re-sort.
    void record(const RaceEvent& event);

    /// All logged events, in the order they were recorded. O(1) (returns a
    /// const reference, no copy).
    const std::vector<RaceEvent>& entries() const noexcept { return entries_; }

    std::size_t size() const noexcept { return entries_.size(); }
    bool empty() const noexcept { return entries_.empty(); }
    void clear() noexcept { entries_.clear(); }

    /// All entries for one driver, in log order. O(n).
    std::vector<RaceEvent> entriesForDriver(int driverNumber) const;

    /// All entries of one type, in log order. O(n).
    std::vector<RaceEvent> entriesOfType(RaceEventType type) const;

private:
    std::vector<RaceEvent> entries_;
};

}  // namespace boxbox

#endif  // BOXBOX_EVENTLOG_HPP