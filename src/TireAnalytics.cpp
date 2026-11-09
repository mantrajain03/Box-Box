#include "boxbox/TireAnalytics.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <utility>

#include "boxbox/Common.hpp"
#include "boxbox/TireCompound.hpp"
#include "boxbox/Track.hpp"

namespace boxbox {

namespace {

void requireFinite(double value, const char* what) {
    if (!std::isfinite(value)) {
        throw std::invalid_argument(std::string(what) + " must be a finite number");
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// DegradationProfile
// ---------------------------------------------------------------------------

// O(M). One shared path: a one-compound PitStrategy, then fromStrategy. Its
// prefix table holds a single row, so building it is O(M) too.
DegradationProfile DegradationProfile::fromCompound(const Track& track,
                                                    const TireCompound& compound) {
    return fromStrategy(PitStrategy(track, {compound}), 0);
}

// O(M). One lapDelta call and one O(1) stintCost lookup per age.
DegradationProfile DegradationProfile::fromStrategy(const PitStrategy& strategy,
                                                    std::size_t compoundIndex) {
    if (compoundIndex >= strategy.compounds().size()) {
        throw std::out_of_range("DegradationProfile: compound index " +
                                std::to_string(compoundIndex) + " is not in the compound set");
    }
    const TireCompound& compound = strategy.compounds()[compoundIndex];
    const Track& track = strategy.track();
    const int maxStint = compound.maxStintLaps();  // >= 1, enforced by TireCompound

    DegradationProfile p;
    p.compound_ = compound.name();
    p.cliffRate_ = compound.hasCliff() ? compound.cliffDegradationPerLap() : 0.0;
    if (compound.hasCliff()) {
        p.cliffLap_ = compound.cliffLap();
        // The first penalised age is cliffLap + 1 (PRD 3.2: age > cliffLap).
        // A zero rate penalises nothing, so it never "activates".
        if (p.cliffRate_ > 0.0 && compound.cliffLap() + 1 < maxStint) {
            p.cliffActivationAge_ = compound.cliffLap() + 1;
        }
    }

    const auto m = static_cast<std::size_t>(maxStint);
    p.lapTime_.resize(m);
    p.stintCost_.assign(m + 1U, 0.0);
    for (int age = 0; age < maxStint; ++age) {
        // The canonical lap: the same sum RaceSimulator::lapTimeFor uses.
        const double lap = track.baseLapTime() + compound.lapDelta(age);
        requireFinite(lap, "DegradationProfile: lap time");
        p.lapTime_[static_cast<std::size_t>(age)] = lap;

        // Stint cost straight from the optimizer's prefix table.
        p.stintCost_[static_cast<std::size_t>(age) + 1U] =
            strategy.stintCost(compoundIndex, age + 1);
    }

    p.deriveFromLapsAndCosts();
    return p;
}

// O(M). PitStrategy only tables compounds, so for recorded data the stint
// costs are a running sum of the given laps.
DegradationProfile DegradationProfile::fromLapTimes(std::string label,
                                                    const std::vector<double>& lapTimes) {
    for (const double lap : lapTimes) {
        requireFinite(lap, "DegradationProfile: lap time");
    }

    DegradationProfile p;
    p.compound_ = std::move(label);
    p.lapTime_ = lapTimes;
    p.stintCost_.assign(lapTimes.size() + 1U, 0.0);
    for (std::size_t a = 0; a < lapTimes.size(); ++a) {
        p.stintCost_[a + 1U] = p.stintCost_[a] + lapTimes[a];
    }

    p.deriveFromLapsAndCosts();
    return p;
}

// O(M).
void DegradationProfile::deriveFromLapsAndCosts() {
    const std::size_t m = lapTime_.size();

    incremental_.assign(m, 0.0);
    vsFresh_.assign(m, 0.0);
    nonDecreasing_ = true;
    for (std::size_t a = 0; a < m; ++a) {
        vsFresh_[a] = lapTime_[a] - lapTime_[0];
        if (a > 0U) {
            incremental_[a] = lapTime_[a] - lapTime_[a - 1U];
            if (lapTime_[a] < lapTime_[a - 1U]) {
                nonDecreasing_ = false;
            }
        }
    }

    // See the header: when laps never fall the true mean never falls, so a
    // running maximum only strips rounding dips of a few 1e-14 s.
    average_.assign(m + 1U, 0.0);
    for (std::size_t k = 1; k <= m; ++k) {
        const double raw = stintCost_[k] / static_cast<double>(k);
        average_[k] = (nonDecreasing_ && k > 1U && raw < average_[k - 1U]) ? average_[k - 1U] : raw;
    }
}

// O(C*M).
std::vector<DegradationProfile> buildProfiles(const PitStrategy& strategy) {
    std::vector<DegradationProfile> out;
    out.reserve(strategy.compounds().size());
    for (std::size_t c = 0; c < strategy.compounds().size(); ++c) {
        out.push_back(DegradationProfile::fromStrategy(strategy, c));
    }
    return out;
}

// ---------------------------------------------------------------------------
// TireRangeAnalyzer
// ---------------------------------------------------------------------------

TireRangeAnalyzer::TireRangeAnalyzer(DegradationProfile profile) : profile_(std::move(profile)) {
    freshLap_ = profile_.lapTime().empty() ? 0.0 : profile_.lapTime().front();
    reset();
}

// O(M).
void TireRangeAnalyzer::reset() {
    lapTree_.build(profile_.lapTime());
}

double TireRangeAnalyzer::lapTimeSum(std::size_t ageBegin, std::size_t ageEnd) const {
    return lapTree_.rangeSum(ageBegin, ageEnd);
}

double TireRangeAnalyzer::averageLapTime(std::size_t ageBegin, std::size_t ageEnd) const {
    const double total = lapTree_.rangeSum(ageBegin, ageEnd);  // validates the bounds
    if (ageBegin == ageEnd) {
        throw std::invalid_argument("TireRangeAnalyzer: average over an empty age range");
    }
    return total / static_cast<double>(ageEnd - ageBegin);
}

double TireRangeAnalyzer::minLapTime(std::size_t ageBegin, std::size_t ageEnd) const {
    return lapTree_.rangeMin(ageBegin, ageEnd);
}

std::size_t TireRangeAnalyzer::fastestAge(std::size_t ageBegin, std::size_t ageEnd) const {
    return lapTree_.rangeMinIndex(ageBegin, ageEnd);
}

// O(log M): one range sum, then subtract the fresh lap once per age.
double TireRangeAnalyzer::degradationSum(std::size_t ageBegin, std::size_t ageEnd) const {
    const double total = lapTree_.rangeSum(ageBegin, ageEnd);
    return total - static_cast<double>(ageEnd - ageBegin) * freshLap_;
}

double TireRangeAnalyzer::lapTimeAt(std::size_t age) const {
    return lapTree_.valueAt(age);
}

// O(log M). SegmentTree::update validates both arguments before writing.
void TireRangeAnalyzer::overrideLapTime(std::size_t age, double newLapTime) {
    lapTree_.update(age, newLapTime);
}

// ---------------------------------------------------------------------------
// Threshold queries
// ---------------------------------------------------------------------------

ThresholdResult firstAgeDegradationExceeds(const DegradationProfile& profile,
                                           double limitSeconds) {
    requireFinite(limitSeconds, "firstAgeDegradationExceeds: limit");
    const std::vector<double>& d = profile.degradationVsFresh();
    const auto exceeds = [&](std::size_t age) { return definitelyLess(limitSeconds, d[age]); };

    ThresholdResult r;
    if (profile.lapTimeNonDecreasing()) {
        // false ... false true ... true: find the first true (lower bound).
        r.usedBinarySearch = true;
        std::size_t lo = 0;
        std::size_t hi = d.size();
        while (lo < hi) {
            const std::size_t mid = lo + (hi - lo) / 2U;
            if (exceeds(mid)) {
                hi = mid;
            } else {
                lo = mid + 1U;
            }
        }
        if (lo < d.size()) {
            r.value = static_cast<int>(lo);
        }
        return r;
    }
    for (std::size_t age = 0; age < d.size(); ++age) {
        if (exceeds(age)) {
            r.value = static_cast<int>(age);
            break;
        }
    }
    return r;
}

ThresholdResult longestStintUnderAverage(const DegradationProfile& profile,
                                         double maxAverageLapTime) {
    requireFinite(maxAverageLapTime, "longestStintUnderAverage: threshold");
    const std::vector<double>& avg = profile.averageLapTime();
    const auto m = static_cast<std::size_t>(profile.maxStint());
    // Fits unless the average is above the threshold by more than the tolerance.
    const auto fits = [&](std::size_t k) { return !definitelyLess(maxAverageLapTime, avg[k]); };

    ThresholdResult r;
    if (profile.lapTimeNonDecreasing()) {
        // true ... true false ... false over k = 1 .. m: find the last true.
        r.usedBinarySearch = true;
        std::size_t lo = 1;
        std::size_t hi = m + 1U;
        while (lo < hi) {
            const std::size_t mid = lo + (hi - lo) / 2U;
            if (fits(mid)) {
                lo = mid + 1U;
            } else {
                hi = mid;
            }
        }
        if (lo > 1U) {
            r.value = static_cast<int>(lo - 1U);
        }
        return r;
    }
    for (std::size_t k = m; k >= 1U; --k) {
        if (fits(k)) {
            r.value = static_cast<int>(k);
            break;
        }
    }
    return r;
}

std::vector<int> stintLengthsWithin(const DegradationProfile& profile, double maxAverageLapTime,
                                    double maxDegradationSeconds) {
    requireFinite(maxAverageLapTime, "stintLengthsWithin: average threshold");
    requireFinite(maxDegradationSeconds, "stintLengthsWithin: degradation threshold");

    std::vector<int> out;
    const auto m = static_cast<std::size_t>(profile.maxStint());
    for (std::size_t k = 1; k <= m; ++k) {
        const bool averageOk = !definitelyLess(maxAverageLapTime, profile.averageLapTime()[k]);
        const bool wearOk =
            !definitelyLess(maxDegradationSeconds, profile.degradationVsFresh()[k - 1U]);
        if (averageOk && wearOk) {
            out.push_back(static_cast<int>(k));
        }
    }
    return out;
}

// O(min(Ma, Mb)): one pass over the ages legal on both compounds. Ties keep
// the last leader, so they can delay a switch but never cause one.
CompoundComparison compareCompounds(const DegradationProfile& a, const DegradationProfile& b) {
    enum class Leader { None, B, A };  // B: A is slower; A: A is faster

    CompoundComparison result;
    const std::size_t limit = std::min(a.lapTime().size(), b.lapTime().size());
    result.agesCompared = static_cast<int>(limit);

    Leader last = Leader::None;  // leader at the last non-tied age
    for (std::size_t age = 0; age < limit; ++age) {
        const double lapA = a.lapTime()[age];
        const double lapB = b.lapTime()[age];
        Leader now = Leader::None;
        if (definitelyLess(lapB, lapA)) {
            now = Leader::B;
            if (!result.firstSlowerAge) {
                result.firstSlowerAge = static_cast<int>(age);
            }
        } else if (definitelyLess(lapA, lapB)) {
            now = Leader::A;
        }
        if (now == Leader::None) {
            continue;  // a tie: neither a switch nor a new leader
        }
        if (last != Leader::None && now != last) {
            result.switchAges.push_back(static_cast<int>(age));
        }
        last = now;
    }
    return result;
}

}  // namespace boxbox
