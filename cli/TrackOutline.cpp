#include "TrackOutline.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace boxbox {

namespace {

constexpr std::size_t kDensePoints = 1440;   ///< curve samples before resampling by arc length
constexpr double kTwoPi = 6.283185307179586;
constexpr double kDrawWidth = 1000.0;
constexpr double kDrawHeight = 600.0;
constexpr double kMargin = 70.0;
constexpr double kAspect = 1.45;             ///< stretches the curve into a landscape shape
constexpr std::size_t kPitHalfSpan = 8;      ///< pit lane covers this many points either side of the line
constexpr double kPitOffset = 28.0;          ///< how far inside the track the pit lane runs
constexpr double kLineHalfLength = 17.0;     ///< half the length of the start/finish line

/// FNV-1a over the name's bytes. std::hash is implementation-defined, and the
/// outline must be the same on every platform.
std::uint64_t hashName(const std::string& name) {
    std::uint64_t h = 1469598103934665603ULL;
    for (const char ch : name) {
        h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(ch));
        h *= 1099511628211ULL;
    }
    return h;
}

/// SplitMix64, mapped to [0, 1).
double nextUnit(std::uint64_t& state) {
    state += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = state;
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    z ^= z >> 31U;
    return static_cast<double>(z >> 11U) / 9007199254740992.0;
}

/// The same loop with its points spaced equally along its length.
std::vector<OutlinePoint> resampleByArcLength(const std::vector<OutlinePoint>& dense,
                                              std::size_t count) {
    const std::size_t m = dense.size();
    std::vector<double> arc(m + 1U, 0.0);
    for (std::size_t i = 0; i < m; ++i) {
        const OutlinePoint& a = dense[i];
        const OutlinePoint& b = dense[(i + 1U) % m];
        arc[i + 1U] = arc[i] + std::hypot(b.x - a.x, b.y - a.y);
    }
    const double total = arc[m];

    std::vector<OutlinePoint> out;
    out.reserve(count);
    std::size_t seg = 0;
    for (std::size_t k = 0; k < count; ++k) {
        const double target = total * static_cast<double>(k) / static_cast<double>(count);
        while (seg + 1U < m && arc[seg + 1U] < target) {
            ++seg;
        }
        const double span = arc[seg + 1U] - arc[seg];
        const double f = (span > 0.0) ? (target - arc[seg]) / span : 0.0;
        const OutlinePoint& a = dense[seg];
        const OutlinePoint& b = dense[(seg + 1U) % m];
        out.push_back(OutlinePoint{a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f});
    }
    return out;
}

double roundTenth(double v) { return std::round(v * 10.0) / 10.0; }

}  // namespace

TrackOutline makeTrackOutline(const Track& track) {
    TrackOutline outline;
    outline.width = kDrawWidth;
    outline.height = kDrawHeight;

    // A closed curve r(theta) = 1 + three harmonics. Their amplitudes sum to
    // well under 1, so r stays positive and the curve is star-shaped about the
    // origin: it can bend and wind, but never cross itself.
    std::uint64_t state = hashName(track.name());
    const double a1 = 0.16 + 0.10 * nextUnit(state);
    const double a2 = 0.08 + 0.06 * nextUnit(state);
    const double a3 = 0.03 + 0.04 * nextUnit(state);
    const double p1 = kTwoPi * nextUnit(state);
    const double p2 = kTwoPi * nextUnit(state);
    const double p3 = kTwoPi * nextUnit(state);

    std::vector<OutlinePoint> dense;
    dense.reserve(kDensePoints);
    for (std::size_t i = 0; i < kDensePoints; ++i) {
        const double theta = kTwoPi * static_cast<double>(i) / static_cast<double>(kDensePoints);
        const double r = 1.0 + a1 * std::cos(2.0 * theta + p1) + a2 * std::cos(3.0 * theta + p2) +
                         a3 * std::cos(5.0 * theta + p3);
        dense.push_back(OutlinePoint{kAspect * r * std::cos(theta), r * std::sin(theta)});
    }

    std::vector<OutlinePoint> pts = resampleByArcLength(dense, kOutlinePointCount);

    // Fit to the drawing area, keeping the proportions.
    double minX = pts[0].x, maxX = pts[0].x, minY = pts[0].y, maxY = pts[0].y;
    for (const OutlinePoint& p : pts) {
        minX = std::min(minX, p.x); maxX = std::max(maxX, p.x);
        minY = std::min(minY, p.y); maxY = std::max(maxY, p.y);
    }
    const double scale = std::min((kDrawWidth - 2.0 * kMargin) / (maxX - minX),
                                  (kDrawHeight - 2.0 * kMargin) / (maxY - minY));
    const double offsetX = (kDrawWidth - (maxX - minX) * scale) / 2.0 - minX * scale;
    const double offsetY = (kDrawHeight - (maxY - minY) * scale) / 2.0 - minY * scale;
    for (OutlinePoint& p : pts) {
        p.x = p.x * scale + offsetX;
        p.y = p.y * scale + offsetY;
    }

    double centreX = 0.0, centreY = 0.0;
    for (const OutlinePoint& p : pts) {
        centreX += p.x;
        centreY += p.y;
    }
    centreX /= static_cast<double>(pts.size());
    centreY /= static_cast<double>(pts.size());

    // Pit lane: the points around the line, pushed toward the inside of the loop.
    const std::size_t n = pts.size();
    for (std::size_t k = 0; k <= 2U * kPitHalfSpan; ++k) {
        const std::size_t i = (n - kPitHalfSpan + k) % n;
        const OutlinePoint& before = pts[(i + n - 1U) % n];
        const OutlinePoint& after = pts[(i + 1U) % n];
        double nx = -(after.y - before.y);
        double ny = after.x - before.x;
        const double length = std::hypot(nx, ny);
        if (length > 0.0) { nx /= length; ny /= length; }
        if (nx * (centreX - pts[i].x) + ny * (centreY - pts[i].y) < 0.0) { nx = -nx; ny = -ny; }
        outline.pitLane.push_back(OutlinePoint{pts[i].x + nx * kPitOffset, pts[i].y + ny * kPitOffset});
    }

    // Start/finish line: across the track at point 0.
    {
        const OutlinePoint& before = pts[n - 1U];
        const OutlinePoint& after = pts[1];
        double nx = -(after.y - before.y);
        double ny = after.x - before.x;
        const double length = std::hypot(nx, ny);
        if (length > 0.0) { nx /= length; ny /= length; }
        outline.lineA = OutlinePoint{pts[0].x - nx * kLineHalfLength, pts[0].y - ny * kLineHalfLength};
        outline.lineB = OutlinePoint{pts[0].x + nx * kLineHalfLength, pts[0].y + ny * kLineHalfLength};
    }

    // Sector boundaries at each sector's cumulative share of the base lap time.
    const double lap = track.baseLapTime();
    double cumulative = 0.0;
    for (const Sector& sector : track.sectors()) {
        const double share = (lap > 0.0) ? cumulative / lap : 0.0;
        outline.sectorStart.push_back(
            std::min(static_cast<std::size_t>(std::llround(share * static_cast<double>(n))), n - 1U));
        cumulative += sector.baseTime;
    }
    if (outline.sectorStart.empty()) {
        outline.sectorStart.push_back(0);
    }

    // Round last, so the points the page draws are the points that were measured.
    for (OutlinePoint& p : pts) { p.x = roundTenth(p.x); p.y = roundTenth(p.y); }
    for (OutlinePoint& p : outline.pitLane) { p.x = roundTenth(p.x); p.y = roundTenth(p.y); }
    outline.lineA = OutlinePoint{roundTenth(outline.lineA.x), roundTenth(outline.lineA.y)};
    outline.lineB = OutlinePoint{roundTenth(outline.lineB.x), roundTenth(outline.lineB.y)};
    outline.points = pts;
    return outline;
}

}  // namespace boxbox
