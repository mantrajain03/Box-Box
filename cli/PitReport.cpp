#include "PitReport.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <ios>
#include <iomanip>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>

#include "boxbox/PitAnalytics.hpp"

namespace boxbox {

namespace {

std::string compoundName(const std::vector<TireCompound>& compounds, int index) {
    if (index < 0) {
        return "unknown compound";   // the log did not say which compound
    }
    if (static_cast<std::size_t>(index) < compounds.size()) {
        return compounds[static_cast<std::size_t>(index)].name();
    }
    return "compound index " + std::to_string(index);
}

std::string clock(double seconds) {
    const long long ms = std::llround(seconds * 1000.0);
    std::ostringstream text;
    text << ms / 60000 << ':' << std::setfill('0') << std::setw(2) << (ms / 1000) % 60 << '.'
         << std::setw(3) << ms % 1000;
    return text.str();
}

}  // namespace

void printPitReport(std::ostream& out, const PitReportInput& in) {
    if (in.log == nullptr || in.compounds == nullptr) {
        throw std::invalid_argument("printPitReport: a log and a compound set are required");
    }
    if (in.windowFrom != 0 && (in.windowFrom < 1 || in.windowFrom > in.windowTo)) {
        throw std::invalid_argument("printPitReport: the pit window needs 1 <= FROM <= TO");
    }
    if (in.callWindow < 0 || in.trafficLapsAfter < 1 || in.histogramBinLaps < 1) {
        throw std::invalid_argument(
            "printPitReport: callWindow must not be negative; trafficLapsAfter and histogramBinLaps "
            "must be at least 1");
    }

    const PitAnalytics analytics(*in.log);
    const std::vector<TireCompound>& compounds = *in.compounds;
    const std::vector<DriverPitSummary> summaries = analytics.allSummaries();

    // Leave the caller's stream as it was found.
    const std::ios_base::fmtflags savedFlags = out.flags();
    const std::streamsize savedPrecision = out.precision();
    struct Restore {
        std::ostream& stream;
        std::ios_base::fmtflags flags;
        std::streamsize precision;
        ~Restore() {
            stream.flags(flags);
            stream.precision(precision);
        }
    } restore{out, savedFlags, savedPrecision};

    out << std::fixed << std::setprecision(3);
    out << "\n=== Pit Report ===\n";
    if (summaries.empty()) {
        out << "  No driver pit data recorded.\n\n";
        return;
    }

    bool allTile = true;
    std::string firstProblem;
    for (const DriverPitSummary& summary : summaries) {
        out << "  Driver " << summary.driverNumber << "  stops: " << summary.stopCount
            << "  pit loss: " << analytics.totalPitLoss(summary.driverNumber, in.pitLoss) << " s\n";

        const std::vector<StintInterval> intervals = PitAnalytics::stintIntervals(summary);
        for (std::size_t i = 0; i < summary.stintLengthsLaps.size(); ++i) {
            out << "    Stint " << (i + 1U) << ": ";
            if (i < summary.compoundIndicesUsed.size()) {
                out << compoundName(compounds, summary.compoundIndicesUsed[i]);
            } else {
                out << "unknown compound";
            }
            out << ", " << summary.stintLengthsLaps[i] << " laps (laps " << intervals[i].firstLap
                << "-" << intervals[i].lastLap << ")\n";
        }
        if (!summary.stopLaps.empty()) {
            out << "    Stop at end of lap(s): ";
            for (std::size_t i = 0; i < summary.stopLaps.size(); ++i) {
                if (i > 0U) out << ", ";
                out << summary.stopLaps[i];
            }
            out << '\n';
        }
        const StintStats stats = PitAnalytics::stintStats(summary);
        out << "    Stints: " << stats.count << ", shortest " << stats.shortestLaps
            << ", longest " << stats.longestLaps << ", mean " << std::setprecision(1)
            << stats.meanLaps << std::setprecision(3) << " laps\n";

        const CoverageReport coverage = PitAnalytics::checkStintCoverage(intervals, in.raceLaps);
        if (!coverage.tiles) {
            allTile = false;
            if (firstProblem.empty()) {
                firstProblem = "driver " + std::to_string(summary.driverNumber) + ": " + coverage.message;
            }
        }
    }

    out << "  Stint coverage: ";
    if (allTile) {
        out << "every car's stints tile laps 1-" << in.raceLaps << " with no gap or overlap\n";
    } else {
        out << "PROBLEM, " << firstProblem << '\n';
    }

    const PitAnalytics::PitWindow window = analytics.pitWindow();
    if (window.hasStops) {
        out << "  Overall pit window: laps " << window.earliestStopLap << " - " << window.latestStopLap
            << '\n';
    } else {
        out << "  Overall pit window: no pit stops recorded\n";
    }

    const StintStats field = analytics.fieldStintStats();
    out << "  Field stints: " << field.count << ", shortest " << field.shortestLaps << ", longest "
        << field.longestLaps << ", mean " << std::setprecision(1) << field.meanLaps
        << std::setprecision(3) << " laps\n";
    out << "  Stint length distribution (" << in.histogramBinLaps << "-lap bins):\n";
    for (const StintHistogramBin& bin : analytics.stintHistogram(in.histogramBinLaps)) {
        out << "    " << std::setw(3) << bin.firstLaps << "-" << std::left << std::setw(3)
            << bin.lastLaps << std::right << " laps  " << std::string(static_cast<std::size_t>(bin.count), '#')
            << " " << bin.count << '\n';
    }

    out << "  Pit-lane time lost: " << analytics.fieldPitLoss(in.pitLoss) << " s ("
        << "stops x " << in.pitLoss << " s; the simulator charges a stop as one lump, so there is\n"
        << "    no separate entry, service or exit duration)\n";

    out << "  Stops in order:\n";
    const std::vector<PitStopRecord> stops = analytics.chronologicalPitStops();
    if (stops.empty()) {
        out << "    none\n";
    }
    for (const PitStopRecord& stop : stops) {
        out << "    " << clock(stop.time) << "  lap " << std::setw(3) << stop.lap << "  car "
            << std::setw(3) << stop.driverNumber << "  " << compoundName(compounds, stop.fromCompound)
            << " -> " << compoundName(compounds, stop.toCompound) << '\n';
    }

    if (in.windowFrom != 0) {
        const PitWindowSummary w = analytics.windowSummary(in.windowFrom, in.windowTo, in.pitLoss);
        out << "  Pit window laps " << w.fromLap << "-" << w.toLap << ": " << w.stops << " stop(s) by "
            << w.drivers << " car(s)";
        if (w.stops > 0) {
            out << ", between laps " << w.earliestLap << " and " << w.latestLap;
        }
        out << ", pit loss " << w.pitLossTotal << " s";
        if (in.raceLaps >= 1 && in.windowFrom >= 1 && in.windowTo <= in.raceLaps) {
            const StopLapIndex index = analytics.buildStopIndex(in.raceLaps);
            out << " (prefix-sum count " << index.stopsInLaps(in.windowFrom, in.windowTo) << ")";
        }
        out << '\n';
    }

    // The greedy problem, stated: every stop has an allowed window of +/- callWindow laps around
    // the lap it was made on; choose the fewest laps so that each stop's window holds one.
    std::vector<StopWindow> windows;
    for (const PitStopRecord& stop : stops) {
        StopWindow w;
        w.driverNumber = stop.driverNumber;
        w.latestLap = in.raceLaps >= 1 ? std::min(in.raceLaps, stop.lap + in.callWindow)
                                       : stop.lap + in.callWindow;
        // A stop logged past the race distance (a damaged log) must not make the window empty.
        w.earliestLap = std::max(1, std::min(stop.lap - in.callWindow, w.latestLap));
        w.latestLap = std::max(w.latestLap, w.earliestLap);
        windows.push_back(w);
    }
    const std::vector<int> calls = PitAnalytics::minimumStopLaps(windows);
    out << "  Fewest pit calls so that every stop falls within " << in.callWindow
        << " lap(s) of a called lap: " << calls.size();
    if (!calls.empty()) {
        out << " (laps ";
        for (std::size_t i = 0; i < calls.size(); ++i) {
            if (i > 0U) out << ", ";
            out << calls[i];
        }
        out << ")";
    }
    out << '\n';

    if (in.records != nullptr) {
        out << "  Traffic lost in the " << in.trafficLapsAfter << " lap(s) after each stop:\n";
        const std::vector<TrafficAfterStop> after =
            PitAnalytics::trafficAfterStops(*in.records, in.trafficLapsAfter);
        if (after.empty()) {
            out << "    none\n";
        }
        for (const TrafficAfterStop& t : after) {
            out << "    car " << std::setw(3) << t.driverNumber << "  stop at lap " << std::setw(3)
                << t.stopLap << "  " << t.trafficSeconds << " s over " << t.lapsConsidered
                << " lap(s)\n";
        }
    }
    out << '\n';
}

}  // namespace boxbox
