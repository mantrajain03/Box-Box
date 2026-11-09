#ifndef BOXBOX_TIRE_ANALYTICS_HPP
#define BOXBOX_TIRE_ANALYTICS_HPP

/// Tire intelligence (plan sec. 4.2): degradation profiles, range analytics
/// and threshold queries, built on the canonical lap-time model.
///
/// Sources of truth, never duplicated here:
///   * lap time at a tire age   Track::baseLapTime() + TireCompound::lapDelta(age),
///                              the same sum RaceSimulator::lapTimeFor uses
///   * stint cost               PitStrategy::stintCost, i.e. its prefix table
///   * tolerances               kTimeEpsilon and definitelyLess from Common.hpp
///
/// Conventions:
///   * tire age is 0-based; a k-lap stint runs ages 0 .. k-1 (PRD 3.1)
///   * library ranges are HALF-OPEN, [begin, end), as in SegmentTree and
///     FenwickTree. The CLI turns the user's inclusive "--from A --to B" into
///     [A, B + 1) before calling in here.
///
/// HONEST COST NOTE (NFR-2). Building a profile is O(M), M = maxStintLaps,
/// and every caller today (the `tires` CLI) builds a fresh profile for each
/// command. End to end, each command therefore costs O(M), whatever the
/// per-query figures below say. The O(log M) queries only pay off when one
/// profile or analyzer serves many queries. M is small in practice (45 for
/// the default HARD compound), so at that size a plain scan is about as fast;
/// the structures are here for correctness-checked range queries and point
/// overrides, not for speed on 45 values.

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "boxbox/PitStrategy.hpp"
#include "boxbox/SegmentTree.hpp"

namespace boxbox {

/// Everything about one compound's pace by tire age, read-only once built.
///
/// AGES AND STINT LENGTHS. maxStintLaps is INCLUSIVE: it is the longest legal
/// stint, so stint lengths 1 .. maxStintLaps are all legal. Tire age is
/// 0-based, so a k-lap stint runs ages 0 .. k-1, and the legal ages are
/// 0 .. maxStintLaps - 1. Hence:
///   * lapTime(), incrementalDegradation(), degradationVsFresh() have
///     maxStint() entries, indexed by age 0 .. maxStint() - 1
///   * stintCost() and averageLapTime() have maxStint() + 1 entries, indexed
///     by stint length k = 0 .. maxStint(); entry 0 is the empty stint
///   * the last lap of a k-lap stint is lapTime()[k - 1]
/// Example: maxStintLaps 22 allows stints of 1 to 22 laps, run at ages 0 to 21.
///
/// Built only by the factories below, and every derived field, including
/// lapTimeNonDecreasing(), is computed from the stored numbers inside them.
/// A caller cannot hand-set the flag that gates the binary searches.
class DegradationProfile {
public:
    /// One compound on one track, for sensitivity sweeps: vary a TireCompound
    /// and call this, with no PitStrategy to manage. O(M).
    ///
    /// Internally it builds a one-compound PitStrategy and delegates to
    /// fromStrategy, so the two paths share one code path and give
    /// bit-identical profiles. That costs O(M) for the one-row prefix table,
    /// the same order as building the profile itself.
    static DegradationProfile fromCompound(const Track& track, const TireCompound& compound);

    /// One compound of an existing optimizer: an adapter for callers that
    /// already hold a PitStrategy. Lap times come from the canonical model
    /// (Track::baseLapTime() + TireCompound::lapDelta(age)); stint costs are
    /// read from PitStrategy::stintCost, so the optimizer's prefix table stays
    /// the single source of stint costs. O(M).
    /// \throws std::out_of_range if compoundIndex is not a compound of
    ///         `strategy`.
    static DegradationProfile fromStrategy(const PitStrategy& strategy,
                                           std::size_t compoundIndex);

    /// Recorded lap times by tire age, with no compound model behind them,
    /// for example a stint taken from simulated race data. PitStrategy has no
    /// table for arbitrary data, so stint costs are summed here. No cliff is
    /// reported. O(M).
    /// \throws std::invalid_argument if any lap time is not finite.
    static DegradationProfile fromLapTimes(std::string label,
                                           const std::vector<double>& lapTimes);

    const std::string& compound() const noexcept { return compound_; }

    /// The compound's maxStintLaps: the longest legal stint, inclusive, and so
    /// also the number of legal tire ages (0 .. maxStint() - 1).
    int maxStint() const noexcept { return static_cast<int>(lapTime_.size()); }

    /// Configured cliff lap, or empty when the compound has none.
    std::optional<int> cliffLap() const noexcept { return cliffLap_; }

    /// Configured extra seconds per lap of age past the cliff. 0.0 when there
    /// is no cliff. Read from TireCompound, not reconstructed from lap times.
    double cliffDegradationPerLap() const noexcept { return cliffRate_; }

    /// First tire age that actually pays the cliff penalty: cliffLap + 1,
    /// present only when a cliff is configured, its rate is above zero, and
    /// that age is inside the legal stint.
    std::optional<int> cliffActivationAge() const noexcept { return cliffActivationAge_; }

    /// lapTime()[a]: seconds for a lap at tire age a. maxStint() entries.
    const std::vector<double>& lapTime() const noexcept { return lapTime_; }

    /// lapTime[a] - lapTime[a-1]; 0.0 at age 0. maxStint() entries.
    const std::vector<double>& incrementalDegradation() const noexcept { return incremental_; }

    /// lapTime[a] - lapTime[0]. maxStint() entries.
    const std::vector<double>& degradationVsFresh() const noexcept { return vsFresh_; }

    /// stintCost()[k]: green-flag seconds of a k-lap stint, excluding pit
    /// loss. maxStint() + 1 entries; [0] is 0.0.
    const std::vector<double>& stintCost() const noexcept { return stintCost_; }

    /// averageLapTime()[k]: mean lap of a k-lap stint, stintCost[k] / k.
    /// maxStint() + 1 entries; [0] is 0.0 and means nothing.
    ///
    /// When lapTimeNonDecreasing() holds, this is stored as a running maximum
    /// of stintCost[k] / k. In exact arithmetic that changes nothing, because
    /// a mean of non-decreasing laps never falls. In floating point the raw
    /// quotient can dip by a few 1e-14 s (seen with zero wear), which would
    /// make the binary search below disagree with a scan. The running maximum
    /// removes that dip and differs from the raw quotient by no more than it.
    const std::vector<double>& averageLapTime() const noexcept { return average_; }

    /// True when the STORED lap times never fall from one age to the next.
    /// Checked on the actual numbers while building, so it also covers
    /// rounding. It gates every binary search in this module.
    bool lapTimeNonDecreasing() const noexcept { return nonDecreasing_; }

private:
    DegradationProfile() = default;

    /// Derive incremental, vsFresh, average and the flag from lapTime_ and
    /// stintCost_. O(M).
    void deriveFromLapsAndCosts();

    std::string compound_;
    std::optional<int> cliffLap_;
    double cliffRate_ = 0.0;
    std::optional<int> cliffActivationAge_;
    std::vector<double> lapTime_;
    std::vector<double> incremental_;
    std::vector<double> vsFresh_;
    std::vector<double> stintCost_;
    std::vector<double> average_;
    bool nonDecreasing_ = false;
};

/// One profile per compound of `strategy`, in compound order. O(C*M).
std::vector<DegradationProfile> buildProfiles(const PitStrategy& strategy);

/// Range queries by tire age over one profile, with what-if point overrides
/// ("age 12 was 0.8 s slower after a lock-up"), backed by one SegmentTree
/// over the lap times.
///
/// Degradation sums need no second tree: every degradation value is that
/// lap's time minus the ORIGINAL fresh lap, so a range's total is
/// lapTimeSum(b, e) - (e - b) * freshLap. Overriding age 0 does not move
/// that reference.
///
/// Range minimum matters after overrides. On an unmodified profile whose lap
/// times never fall, the fastest age in [b, e) is always b; the tree still
/// answers correctly, it just has nothing to discover.
///
/// COSTS: construct O(M), reset O(M), every query O(log M), override O(log M).
class TireRangeAnalyzer {
public:
    explicit TireRangeAnalyzer(DegradationProfile profile);

    const DegradationProfile& profile() const noexcept { return profile_; }
    std::size_t ageCount() const noexcept { return lapTree_.size(); }

    /// Sum of lap times over ages [b, e). O(log M).
    /// \throws std::out_of_range on bad bounds.
    double lapTimeSum(std::size_t ageBegin, std::size_t ageEnd) const;

    /// Mean lap over ages [b, e). O(log M).
    /// \throws std::out_of_range on bad bounds, std::invalid_argument if empty.
    double averageLapTime(std::size_t ageBegin, std::size_t ageEnd) const;

    /// Fastest lap over ages [b, e). O(log M). Throws as averageLapTime does.
    double minLapTime(std::size_t ageBegin, std::size_t ageEnd) const;

    /// Age of the fastest lap over [b, e), smallest age on ties. O(log M).
    std::size_t fastestAge(std::size_t ageBegin, std::size_t ageEnd) const;

    /// Sum over [b, e) of each lap's loss against the original fresh lap.
    /// O(log M). \throws std::out_of_range on bad bounds.
    double degradationSum(std::size_t ageBegin, std::size_t ageEnd) const;

    /// Current lap time at an age, overrides included. O(1).
    /// \throws std::out_of_range if age >= ageCount().
    double lapTimeAt(std::size_t age) const;

    /// Replace one age's lap time. O(log M). The profile is not modified.
    /// \throws std::out_of_range on a bad age, std::invalid_argument if the
    ///         time is not finite. A rejected override changes nothing.
    void overrideLapTime(std::size_t age, double newLapTime);

    /// Drop every override. O(M).
    void reset();

private:
    DegradationProfile profile_;
    double freshLap_ = 0.0;
    SegmentTree lapTree_;
};

/// Result of a threshold query (firstAgeDegradationExceeds,
/// longestStintUnderAverage).
///
/// Each query states its algorithm. Binary search is used only when
/// profile.lapTimeNonDecreasing() holds, which makes the searched property
/// monotonic in the stored numbers; otherwise the query scans. The answer is
/// the same either way. All comparisons use definitelyLess with kTimeEpsilon.
struct ThresholdResult {
    std::optional<int> value;       ///< empty when nothing satisfies the query
    bool usedBinarySearch = false;  ///< true when the binary-search path ran
};

/// First tire age whose degradationVsFresh is more than `limitSeconds`.
/// Monotonic because lapTime[a] - lapTime[0] never falls when lapTime never
/// falls (subtracting the same number keeps the order, rounding included).
/// O(log M) by binary search when that holds, else an O(M) scan.
/// \throws std::invalid_argument if limitSeconds is not finite.
ThresholdResult firstAgeDegradationExceeds(const DegradationProfile& profile,
                                           double limitSeconds);

/// Longest stint k in 1 .. maxStint whose averageLapTime()[k] is at most
/// `maxAverageLapTime`. The stored averages never fall when lap times never
/// fall, so the stints that fit form a prefix 1 .. K and binary search finds
/// K in O(log M). Otherwise every k is checked, O(M).
/// \throws std::invalid_argument if the threshold is not finite.
ThresholdResult longestStintUnderAverage(const DegradationProfile& profile,
                                         double maxAverageLapTime);

/// Every stint length whose average is at most `maxAverageLapTime` AND whose
/// last lap has lost at most `maxDegradationSeconds` against a fresh tire.
/// Always a scan, O(M): the answer itself can be M long.
/// \throws std::invalid_argument if either threshold is not finite.
std::vector<int> stintLengthsWithin(const DegradationProfile& profile,
                                    double maxAverageLapTime,
                                    double maxDegradationSeconds);

/// How two compounds compare age by age (agreed with Sarthak, 2026-11-06).
///
/// Only ages legal on BOTH compounds are compared: 0 .. agesCompared - 1.
///
/// At each age, A is SLOWER when its lap is more than kTimeEpsilon slower
/// than B's, FASTER when B's lap is more than kTimeEpsilon slower than A's,
/// and otherwise TIED.
///
/// firstSlowerAge: the first age at which A is slower, whatever happens after.
/// Empty when A is never slower inside the compared ages. 0 means A is slower
/// on a fresh set.
///
/// switchAges: every age at which the faster compound changes, in increasing
/// order. Ties do not count on their own:
///   * the first age that is not a tie only sets who leads; it is not a switch
///   * a tied age keeps the previous leader; it never starts a switch
///   * a switch is recorded at the first non-tied age whose leader differs
///     from the last non-tied age before it, so slower, tie, tie, faster is
///     one switch, at the "faster" age
///   * slower, tie, slower has no switch at all
/// An empty list means one compound leads at every non-tied age (or every
/// age ties): the gap never changes sign.
///
/// The gap A - B changes slope at each compound's cliff, so it can fall and
/// then rise again. This is therefore always a full scan, never a binary
/// search. O(min(Ma, Mb)) time; O(S) space for S switches. Deterministic.
struct CompoundComparison {
    int agesCompared = 0;               ///< min of the two maxStint() values
    std::optional<int> firstSlowerAge;  ///< first age A is slower than B
    std::vector<int> switchAges;        ///< every age the faster compound changes
};

CompoundComparison compareCompounds(const DegradationProfile& a, const DegradationProfile& b);

}  // namespace boxbox

#endif  // BOXBOX_TIRE_ANALYTICS_HPP
