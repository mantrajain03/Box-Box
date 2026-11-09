#include "boxbox/EventPriorityQueue.hpp"

#include <cmath>
#include <stdexcept>
#include <utility>

namespace boxbox {

void EventPriorityQueue::push(RaceEvent event) {
    // Invalid-time policy: reject. A NaN timestamp would break the strict weak
    // order the heap relies on, and a negative one is before the race began.
    if (!std::isfinite(event.timestamp) || event.timestamp < 0.0) {
        throw std::invalid_argument(
            "EventPriorityQueue::push: timestamp must be finite and not negative");
    }
    event.sequenceId = nextSequenceId_++;
    heap_.push_back(std::move(event));
    siftUp(heap_.size() - 1);
}

RaceEvent EventPriorityQueue::pop() {
    if (heap_.empty()) {
        throw std::out_of_range("EventPriorityQueue::pop on empty queue");
    }
    RaceEvent top = heap_.front();
    heap_.front() = std::move(heap_.back());
    heap_.pop_back();
    if (!heap_.empty()) {
        siftDown(0);
    }
    return top;
}

const RaceEvent& EventPriorityQueue::peek() const {
    if (heap_.empty()) {
        throw std::out_of_range("EventPriorityQueue::peek on empty queue");
    }
    return heap_.front();
}

std::vector<RaceEvent> EventPriorityQueue::processUntil(double t) {
    std::vector<RaceEvent> result;
    while (!heap_.empty() && !definitelyLess(t, heap_.front().timestamp)) {
        result.push_back(pop());
    }
    return result;
}

void EventPriorityQueue::siftUp(std::size_t index) noexcept {
    while (index > 0) {
        std::size_t parent = (index - 1) / 2;
        if (!eventComesBefore(heap_[index], heap_[parent])) {
            break;
        }
        std::swap(heap_[index], heap_[parent]);
        index = parent;
    }
}

void EventPriorityQueue::siftDown(std::size_t index) noexcept {
    const std::size_t n = heap_.size();
    while (true) {
        std::size_t left = 2 * index + 1;
        std::size_t right = 2 * index + 2;
        std::size_t smallest = index;

        if (left < n && eventComesBefore(heap_[left], heap_[smallest])) {
            smallest = left;
        }
        if (right < n && eventComesBefore(heap_[right], heap_[smallest])) {
            smallest = right;
        }
        if (smallest == index) {
            break;
        }
        std::swap(heap_[index], heap_[smallest]);
        index = smallest;
    }
}

}  // namespace boxbox