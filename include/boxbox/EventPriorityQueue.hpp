#ifndef BOXBOX_EVENTPRIORITYQUEUE_HPP
#define BOXBOX_EVENTPRIORITYQUEUE_HPP

#include <cstddef>
#include <cstdint>
#include <vector>
#include "boxbox/RaceEvent.hpp"

namespace boxbox {

/// Hand-written binary min-heap of RaceEvents, ordered by eventComesBefore.
///
/// Complexity for E queued events: push O(log E), peek O(1), pop O(log E),
/// processUntil O(k log E) for k events released. Storage O(E).
class EventPriorityQueue {
public:
    EventPriorityQueue() = default;

    /// Insert an event; the queue assigns its sequenceId. O(log E).
    /// \throws std::invalid_argument if the timestamp is NaN, infinite or
    ///         negative (the queue is left unchanged).
    void push(RaceEvent event);

    /// Remove and return the earliest event. O(log E).
    /// \throws std::out_of_range if empty.
    RaceEvent pop();

    /// The earliest event without removing it. O(1).
    /// \throws std::out_of_range if empty.
    const RaceEvent& peek() const;

    bool empty() const noexcept { return heap_.empty(); }
    std::size_t size() const noexcept { return heap_.size(); }
    void clear() noexcept { heap_.clear(); }

    /// Remove and return, in order, every event whose timestamp is not
    /// definitely after `t` (tolerance kTimeEpsilon). O(k log E) for k events.
    std::vector<RaceEvent> processUntil(double t);

private:
    void siftUp(std::size_t index) noexcept;
    void siftDown(std::size_t index) noexcept;

    std::vector<RaceEvent> heap_;
    std::uint64_t nextSequenceId_ = 0;
};

}  // namespace boxbox

#endif  // BOXBOX_EVENTPRIORITYQUEUE_HPP