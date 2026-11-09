#ifndef BOXBOX_EVENTPRIORITYQUEUE_HPP
#define BOXBOX_EVENTPRIORITYQUEUE_HPP

#include <cstddef>
#include <cstdint>
#include <vector>
#include "boxbox/RaceEvent.hpp"

namespace boxbox {

class EventPriorityQueue {
public:
    EventPriorityQueue() = default;

    void push(RaceEvent event);
    RaceEvent pop();
    const RaceEvent& peek() const;

    bool empty() const noexcept { return heap_.empty(); }
    std::size_t size() const noexcept { return heap_.size(); }
    void clear() noexcept { heap_.clear(); }

    std::vector<RaceEvent> processUntil(double t);

private:
    void siftUp(std::size_t index) noexcept;
    void siftDown(std::size_t index) noexcept;

    std::vector<RaceEvent> heap_;
    std::uint64_t nextSequenceId_ = 0;
};

}  // namespace boxbox

#endif  // BOXBOX_EVENTPRIORITYQUEUE_HPP