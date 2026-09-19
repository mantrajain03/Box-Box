#ifndef UNDERCUT_COMMON_HPP
#define UNDERCUT_COMMON_HPP

#include <cmath>
#include <limits>

/// Undercut: F1 pit-stop and race strategy scheduler.
///
/// Conventions used throughout the core (see PRD sec. 3.1):
///   * every time is in SECONDS, stored as double;
///   * laps are 1-based in anything user-facing;
///   * tire age is 0-based, so a stint of L laps runs ages 0 .. L-1;
///   * floating-point values are NEVER compared with ==.
namespace undercut {

/// General-purpose comparison tolerance, in seconds.
///
/// Used for DP candidate comparison and for ordinary equality checks. Chosen
/// far below any physically meaningful lap-time difference, but far above the
/// rounding noise of summing a few thousand doubles of magnitude ~1e2.
inline constexpr double kTimeEpsilon = 1e-9;

/// Tolerance for the FR-8 optimizer/simulator loop-closure check, in seconds.
///
/// The optimizer totals a race via prefix sums while the simulator accumulates
/// it lap by lap. The two sum the same values in a different order, so their
/// results differ in the last bits. One microsecond is comfortably above that
/// noise and far below anything that could hide a modelling bug.
///
/// This value must NOT be relaxed to make a failing check pass.
inline constexpr double kClosureTolerance = 1e-6;

/// Sentinel for "no feasible plan reaches this state".
inline constexpr double kUnreachable = std::numeric_limits<double>::infinity();

/// True when a and b are equal to within `tol` seconds.
inline bool nearlyEqual(double a, double b, double tol = kTimeEpsilon) noexcept {
    return std::fabs(a - b) <= tol;
}

/// True when a is smaller than b by a margin that exceeds `tol`.
///
/// Values that merely tie within tolerance return false, which is what gives
/// the optimizer its stable tie-breaking rule (NFR-5): among near-equal
/// candidates the first one enumerated keeps the slot.
inline bool definitelyLess(double a, double b, double tol = kTimeEpsilon) noexcept {
    return a < b - tol;
}

/// True when `t` is an ordinary finite time rather than kUnreachable or NaN.
inline bool isReachable(double t) noexcept {
    return std::isfinite(t);
}

}  // namespace undercut

#endif  // UNDERCUT_COMMON_HPP
