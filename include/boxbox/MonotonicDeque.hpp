#ifndef BOXBOX_MONOTONIC_DEQUE_HPP
#define BOXBOX_MONOTONIC_DEQUE_HPP

#include <cstddef>
#include <vector>

namespace boxbox {

/// Hand-written monotonic deque: the minimum, or the maximum, of the last W
/// values of a stream, in O(1) amortised time per value (plan sec. 4.3C;
/// driver analytics, owner Mantra). Used by trailingWindows (DriverAnalytics)
/// for the fastest and slowest lap among a car's last W laps.
///
/// How it works, for a minimum. Values arrive one at a time with push(), and
/// the deque keeps only the values that can still become the answer:
///   * a new value first removes every value at the BACK that is larger than
///     it. Those can never be the minimum again: the new value is smaller and
///     stays in the window longer than they do.
///   * the value that slides out of the window leaves from the FRONT.
/// What remains is non-decreasing from front to back, so the front is the
/// window's minimum. A maximum works the same with the comparison reversed.
///
/// The window is TRAILING. After the k-th push (k = 1, 2, ...) it covers the
/// values at stream positions max(0, k - W) .. k - 1. Until W values have
/// arrived it simply holds all of them, so W = 1 answers with each value
/// itself, and a W at least as long as the stream gives the running minimum.
///
/// Ties: a value only removes values strictly worse than itself, so equal
/// values both stay and the front is the EARLIEST of equal extremes - the one
/// a left-to-right scan with a strict < picks. Comparisons are exact, < only,
/// like SegmentTree and Leaderboard::isAhead: the answer must be the very
/// element a brute-force scan picks, with no tolerance to blur which.
///
/// Storage is a ring buffer of exactly W slots, written by hand rather than
/// borrowed from std::deque, because the structure is the point. The window
/// never holds more than W values, so the ring never has to grow.
///
/// COSTS, W = window (NFR-2):
///   construct              O(W) - allocates the ring
///   push                   O(1) amortised. Each value enters once and leaves
///                          at most once, so n pushes cost O(n) in total; a
///                          single push can still remove up to W - 1 values.
///   extreme, extremeIndex  O(1)
///   clear                  O(1)
///   storage                O(W)
class MonotonicDeque {
public:
    enum class Kind { Minimum, Maximum };

    /// Largest window accepted. The ring is allocated up front at the window's
    /// size, so an absurd window is rejected here instead of failing inside
    /// the allocator. A caller that only needs min(window, stream length)
    /// slots, as trailingWindows does, should cap the window itself.
    static constexpr std::size_t kMaxWindow = std::size_t{1} << 26;

    /// \throws std::invalid_argument if window is 0 or above kMaxWindow.
    MonotonicDeque(std::size_t window, Kind kind);

    /// Append the next value of the stream. Its stream index is the value of
    /// pushed() before the call. O(1) amortised.
    /// \throws std::invalid_argument if value is not finite: a NaN has no
    ///         order. A rejected push changes nothing.
    void push(double value);

    /// The minimum (or maximum) of the current window. O(1).
    /// \throws std::out_of_range if nothing has been pushed.
    double extreme() const;

    /// Stream index of extreme(), the earliest on ties. O(1).
    /// \throws std::out_of_range if nothing has been pushed.
    std::size_t extremeIndex() const;

    std::size_t window() const noexcept { return window_; }
    Kind kind() const noexcept { return kind_; }

    /// Values pushed since construction or the last clear().
    std::size_t pushed() const noexcept { return pushed_; }

    /// Values currently held - the candidates - between 1 and W after a push.
    std::size_t size() const noexcept { return count_; }

    bool empty() const noexcept { return count_ == 0U; }

    /// Forget every value; the window and kind stay. O(1).
    void clear() noexcept;

private:
    struct Entry {
        double value = 0.0;
        std::size_t index = 0;  ///< position in the stream
    };

    /// True when `incoming` is strictly better than `held`, so `held` can go.
    bool beats(double incoming, double held) const noexcept;

    /// Ring slot of the entry `offset` places behind the front.
    std::size_t slot(std::size_t offset) const noexcept { return (head_ + offset) % window_; }

    std::size_t window_;
    Kind kind_;
    std::vector<Entry> ring_;
    std::size_t head_ = 0;    ///< slot of the front entry
    std::size_t count_ = 0;   ///< entries held
    std::size_t pushed_ = 0;  ///< stream length so far
};

}  // namespace boxbox

#endif  // BOXBOX_MONOTONIC_DEQUE_HPP
