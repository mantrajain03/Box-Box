#include "boxbox/MonotonicDeque.hpp"

#include <cmath>
#include <stdexcept>

namespace boxbox {

// O(W): the ring is allocated once, at its final size.
MonotonicDeque::MonotonicDeque(std::size_t window, Kind kind) : window_(window), kind_(kind) {
    if (window_ == 0U) {
        throw std::invalid_argument("MonotonicDeque: the window must hold at least one value");
    }
    if (window_ > kMaxWindow) {
        throw std::invalid_argument("MonotonicDeque: the window is larger than kMaxWindow");
    }
    ring_.resize(window_);
}

bool MonotonicDeque::beats(double incoming, double held) const noexcept {
    return (kind_ == Kind::Minimum) ? incoming < held : held < incoming;
}

// O(1) amortised; see the class comment. The value is checked before
// anything is touched, so a rejected push changes nothing.
void MonotonicDeque::push(double value) {
    if (!std::isfinite(value)) {
        throw std::invalid_argument("MonotonicDeque: values must be finite numbers");
    }
    const std::size_t index = pushed_;

    // 1. The value pushed W places ago leaves the window now. Held indices
    //    rise from front to back, so if it is still held it is the front.
    if (count_ > 0U && ring_[head_].index + window_ <= index) {
        head_ = slot(1U);
        --count_;
    }

    // 2. Drop every value at the back that the new one beats. Equal values
    //    stay, which keeps the earliest of equal extremes in front.
    while (count_ > 0U && beats(value, ring_[slot(count_ - 1U)].value)) {
        --count_;
    }

    // 3. Step 1 left at most W - 1 values - a full ring always holds the value
    //    that has just left the window - so there is always a free slot.
    ring_[slot(count_)] = Entry{value, index};
    ++count_;
    ++pushed_;
}

double MonotonicDeque::extreme() const {
    if (count_ == 0U) {
        throw std::out_of_range("MonotonicDeque: no value has been pushed");
    }
    return ring_[head_].value;
}

std::size_t MonotonicDeque::extremeIndex() const {
    if (count_ == 0U) {
        throw std::out_of_range("MonotonicDeque: no value has been pushed");
    }
    return ring_[head_].index;
}

void MonotonicDeque::clear() noexcept {
    head_ = 0;
    count_ = 0;
    pushed_ = 0;
}

}  // namespace boxbox
