#include "DriverReport.hpp"

#include <cstddef>
#include <iomanip>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "boxbox/Common.hpp"

namespace boxbox {

namespace {

std::string seconds(double value) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(3) << value;
    return out.str();
}

std::string lapSpan(int firstLap, int lastLap) {
    return std::to_string(firstLap) + "-" + std::to_string(lastLap);
}

std::string compoundName(const RaceSimulator& simulator, std::size_t index) {
    if (index < simulator.compounds().size()) {
        return simulator.compounds()[index].name();
    }
    return "compound " + std::to_string(index);
}

/// The car-number and driver columns every table starts with.
void carColumns(std::ostream& out, const std::string& number, const std::string& name) {
    out << "  " << std::right << std::setw(4) << number << "  " << std::left << std::setw(16)
        << name << std::right;
}

void carColumns(std::ostream& out, const DriverSummary& s) {
    carColumns(out, std::to_string(s.driverNumber), s.name);
}

void printRanking(std::ostream& out, const DriverAnalytics& analytics, RankMetric metric,
                  const std::string& title, const std::string& column) {
    out << "  " << title << '\n';
    out << "  " << std::right << std::setw(4) << "rank" << std::setw(5) << "#" << "  " << std::left
        << std::setw(16) << "driver" << std::right << std::setw(10) << column << '\n';

    const std::vector<int> order = rankDrivers(analytics.summaries(), metric);
    for (std::size_t i = 0; i < order.size(); ++i) {
        const DriverSummary& s = analytics.summary(order[i]);
        out << "  " << std::right << std::setw(4) << (i + 1U) << std::setw(5) << s.driverNumber
            << "  " << std::left << std::setw(16) << s.name << std::right << std::setw(10)
            << (s.laps > 0 ? seconds(rankValue(s, metric)) : std::string("-")) << '\n';
    }
    out << '\n';
}

void printLapTable(std::ostream& out, const RaceSimulator& simulator,
                   const DriverAnalytics& analytics, const std::vector<LapRecord>& laps) {
    out << "  " << std::right << std::setw(4) << "rank" << std::setw(5) << "lap" << std::setw(5)
        << "#" << "  " << std::left << std::setw(16) << "driver" << std::setw(10) << "compound"
        << std::right << std::setw(5) << "age" << std::setw(11) << "lap time" << '\n';
    for (std::size_t i = 0; i < laps.size(); ++i) {
        const LapRecord& r = laps[i];
        out << "  " << std::right << std::setw(4) << (i + 1U) << std::setw(5) << r.lap
            << std::setw(5) << r.driverNumber << "  " << std::left << std::setw(16)
            << analytics.summary(r.driverNumber).name << std::setw(10)
            << compoundName(simulator, r.compoundIndex) << std::right << std::setw(5) << r.tireAge
            << std::setw(11) << seconds(r.lapTime) << '\n';
    }
}

}  // namespace

void printDriverStats(std::ostream& out, const RaceSimulator& simulator,
                      const DriverAnalytics& analytics) {
    const std::vector<DriverSummary>& cars = analytics.summaries();

    out << "Driver statistics   from the recorded laps, in seconds\n"
        << "  race time = green-flag time + pit time. traffic is the part of the\n"
        << "  green-flag time lost behind other cars: lap times include it and\n"
        << "  leave pit loss out.\n\n";

    carColumns(out, "#", "driver");
    out << std::setw(6) << "laps" << std::setw(13) << "race time" << std::setw(13) << "green-flag"
        << std::setw(11) << "pit time" << std::setw(10) << "traffic" << std::setw(7) << "stops"
        << '\n';
    for (const DriverSummary& s : cars) {
        carColumns(out, s);
        out << std::setw(6) << s.laps << std::setw(13) << seconds(s.totalTime) << std::setw(13)
            << seconds(s.greenTime) << std::setw(11) << seconds(s.pitTime) << std::setw(10)
            << seconds(s.trafficTime) << std::setw(7) << s.stops << '\n';
    }
    out << '\n';

    carColumns(out, "#", "driver");
    out << std::setw(10) << "avg lap" << std::setw(10) << "best" << std::setw(6) << "lap"
        << std::setw(10) << "worst" << std::setw(6) << "lap" << std::setw(9) << "range"
        << std::setw(10) << "mean dev" << '\n';
    for (const DriverSummary& s : cars) {
        carColumns(out, s);
        if (s.laps == 0) {
            out << std::setw(10) << "-" << '\n';
            continue;
        }
        out << std::setw(10) << seconds(s.averageLap) << std::setw(10) << seconds(s.bestLap.lapTime)
            << std::setw(6) << s.bestLap.lap << std::setw(10) << seconds(s.worstLap.lapTime)
            << std::setw(6) << s.worstLap.lap << std::setw(9) << seconds(s.lapRange)
            << std::setw(10) << seconds(s.meanAbsDeviation) << '\n';
    }
    out << "  avg lap = green-flag time / laps. range = worst - best. mean dev = the\n"
        << "  mean absolute deviation of a car's laps from its own avg lap.\n\n";

    out << "  Stints   compound and laps\n";
    for (const DriverSummary& s : cars) {
        carColumns(out, s);
        if (s.stints.empty()) {
            out << "-";
        }
        for (std::size_t i = 0; i < s.stints.size(); ++i) {
            const StintSummary& stint = s.stints[i];
            out << (i > 0U ? ", " : "") << compoundName(simulator, stint.compoundIndex) << ' '
                << lapSpan(stint.firstLap, stint.lastLap);
        }
        if (!s.compounds.empty()) {
            out << "   (";
            for (std::size_t i = 0; i < s.compounds.size(); ++i) {
                out << (i > 0U ? ", " : "") << compoundName(simulator, s.compounds[i].compoundIndex)
                    << ' ' << s.compounds[i].laps;
            }
            out << " laps)";
        }
        out << '\n';
    }
    out << '\n';

    out << "  Running position after each lap   best / worst / final\n";
    for (const DriverSummary& s : cars) {
        carColumns(out, s);
        if (s.positions.empty()) {
            out << "-\n";
            continue;
        }
        out << 'P' << s.bestPosition << " / P" << s.worstPosition << " / P" << s.positions.back()
            << '\n';
    }
    out << '\n';

    printRanking(out, analytics, RankMetric::AverageLap, "Pace ranking   avg lap, lower first",
                 "avg lap");
    printRanking(out, analytics, RankMetric::MeanAbsDeviation,
                 "Consistency ranking   mean dev, lower first", "mean dev");
    out << "  Equal values rank by car number; a car with no laps ranks last.\n\n";

    out << "  Sector times: not measured. The simulator times whole laps only; --sector-stats\n"
        << "  shows a MODELLED split of each lap, and the sectors under Race setup are track\n"
        << "  configuration, not a measurement of any car.\n\n";
}

void printTopLaps(std::ostream& out, const RaceSimulator& simulator,
                  const DriverAnalytics& analytics, std::size_t k, const LapFilter& filter) {
    if (filter.driverNumber && !analytics.contains(*filter.driverNumber)) {
        throw std::invalid_argument("--top-laps-car: no car numbered " +
                                    std::to_string(*filter.driverNumber) + " in this race");
    }

    const std::vector<LapRecord>& all = analytics.records();
    if (all.empty()) {
        out << "Fastest and slowest laps   no laps were run\n\n";
        return;
    }

    const std::vector<LapRecord> fastest = selectLaps(all, k, LapOrder::Fastest, filter);
    const std::vector<LapRecord> slowest = selectLaps(all, k, LapOrder::Slowest, filter);
    const std::size_t matching = selectLaps(all, all.size(), LapOrder::Fastest, filter).size();

    std::string scope = "of ";
    scope += std::to_string(matching) + " laps";
    if (filter.driverNumber) {
        scope += " of car " + std::to_string(*filter.driverNumber);
    }
    if (filter.firstLap > 1 || filter.lastLap != LapFilter{}.lastLap) {
        scope += " in laps " + std::to_string(filter.firstLap) + "-" +
                 (filter.lastLap == LapFilter{}.lastLap ? std::string("end")
                                                         : std::to_string(filter.lastLap));
    }
    if (matching == 0U) {
        out << "Fastest and slowest laps   no laps match the filter\n\n";
        return;
    }

    out << "Fastest " << fastest.size() << ' ' << scope
        << "   lap times include traffic, not pit loss\n";
    printLapTable(out, simulator, analytics, fastest);
    out << "\nSlowest " << slowest.size() << ' ' << scope << '\n';
    printLapTable(out, simulator, analytics, slowest);
    out << "  Equal lap times are listed by lap, then car number.\n\n";

    out << "Fastest lap on each compound\n";
    for (std::size_t c = 0; c < simulator.compounds().size(); ++c) {
        LapFilter onCompound = filter;  // keep any car / lap-range narrowing
        onCompound.compoundIndex = c;
        const std::vector<LapRecord> best = selectLaps(all, 1U, LapOrder::Fastest, onCompound);
        out << "  " << std::left << std::setw(12) << simulator.compounds()[c].name() << std::right;
        if (best.empty()) {
            out << std::setw(9) << "not run" << '\n';
            continue;
        }
        const LapRecord& r = best.front();
        out << std::setw(9) << seconds(r.lapTime) << "   lap " << r.lap << "   #" << r.driverNumber
            << ' ' << analytics.summary(r.driverNumber).name << "   tire age " << r.tireAge << '\n';
    }
    out << '\n';
}

void printLapWindows(std::ostream& out, const DriverAnalytics& analytics, std::size_t window) {
    const std::string w = std::to_string(window);
    out << "Rolling " << w << "-lap windows   lap times include traffic, not pit loss\n"
        << "  quickest / slowest = the " << w << " laps in a row with the lowest / highest\n"
        << "  average lap. drop-off = slowest - quickest. spread = slowest lap minus\n"
        << "  fastest lap within one window, shown for the window where it is widest.\n\n";

    carColumns(out, "#", "driver");
    out << std::setw(10) << "quickest" << std::setw(9) << "laps" << std::setw(10) << "slowest"
        << std::setw(9) << "laps" << std::setw(10) << "drop-off" << std::setw(9) << "spread"
        << std::setw(9) << "laps" << '\n';

    bool anyShort = false;
    for (const DriverSummary& s : analytics.summaries()) {
        carColumns(out, s);
        if (s.laps == 0) {
            out << std::setw(10) << "-" << '\n';
            continue;
        }
        const WindowSummary ws = summarizeWindows(analytics.lapTimes(s.driverNumber), window);
        out << std::setw(10) << seconds(ws.quickest.average) << std::setw(9)
            << lapSpan(ws.quickest.firstLap, ws.quickest.lastLap) << std::setw(10)
            << seconds(ws.slowest.average) << std::setw(9)
            << lapSpan(ws.slowest.firstLap, ws.slowest.lastLap) << std::setw(10)
            << seconds(ws.dropOff) << std::setw(9) << seconds(ws.widest.spread()) << std::setw(9)
            << lapSpan(ws.widest.firstLap, ws.widest.lastLap);
        if (!ws.complete) {
            out << "  *";
            anyShort = true;
        }
        out << '\n';
    }
    if (anyShort) {
        out << "  * fewer than " << w << " laps: the whole race is the one window.\n";
    }
    std::ostringstream tolerance;  // read from the core, never restated here
    tolerance << std::scientific << std::setprecision(0) << kTimeEpsilon;
    out << "  Near-equal stretches (within " << tolerance.str() << " s) go to the earliest.\n\n";
}

namespace {

constexpr const char* kModelledNote =
    "Sector times are a MODELLED split of each lap in proportion to the track's configured\n"
    "sector durations. The simulator times whole laps only, so nothing here is a measured\n"
    "sector performance and it adds no information beyond the lap time.\n\n";

std::string sectorName(const RaceSimulator& simulator, std::size_t index) {
    const std::vector<Sector>& sectors = simulator.track().sectors();
    return index < sectors.size() && !sectors[index].name.empty() ? sectors[index].name
                                                                   : "S" + std::to_string(index + 1U);
}

}  // namespace

void printSectorStats(std::ostream& out, const RaceSimulator& simulator,
                      const DriverAnalytics& analytics) {
    out << "Sector statistics   seconds\n" << kModelledNote;

    const std::vector<DriverSectorSummary> sums = sectorSummaries(analytics);
    const std::vector<FieldSectorBest> best = fieldBestSectors(analytics);
    if (best.empty()) {
        out << "  No sector data: this track has no sectors, or no lap was run.\n\n";
        return;
    }

    out << "  Fastest sector of each car (lap in brackets), average, and best combined lap\n";
    out << "  " << std::right << std::setw(4) << "#" << "  " << std::left << std::setw(16) << "driver"
        << std::right;
    for (std::size_t k = 0; k < best.size(); ++k) {
        out << std::setw(14) << ("fastest " + sectorName(simulator, k)) << std::setw(10)
            << ("avg " + sectorName(simulator, k));
    }
    out << std::setw(12) << "combined" << '\n';
    for (const DriverSectorSummary& s : sums) {
        carColumns(out, std::to_string(s.driverNumber), analytics.summary(s.driverNumber).name);
        if (!s.available) {
            out << "  -\n";
            continue;
        }
        for (std::size_t k = 0; k < s.fastest.size(); ++k) {
            out << std::setw(9) << seconds(s.fastest[k].time) << " (" << std::setw(2) << s.fastest[k].lap
                << ")" << std::setw(10) << seconds(s.average[k]);
        }
        out << std::setw(12) << seconds(s.bestCombinedLap) << '\n';
    }

    out << "\n  Best sector in the field\n";
    for (std::size_t k = 0; k < best.size(); ++k) {
        out << "    " << std::left << std::setw(6) << sectorName(simulator, k) << std::right
            << std::setw(10) << seconds(best[k].time) << "   car " << best[k].driverNumber << ", lap "
            << best[k].lap << '\n';
    }
    out << "    combined " << seconds(fieldBestCombinedLap(analytics)) << " s: the lap the field "
        << "could post if one car took the best of every sector\n";

    out << "\n  Distribution over every lap of every car (6 bins from fastest to slowest)\n";
    for (std::size_t k = 0; k < best.size(); ++k) {
        const SectorDistribution d = sectorDistribution(analytics, k, 6);
        out << "    " << std::left << std::setw(6) << sectorName(simulator, k) << std::right << "min "
            << seconds(d.minimum) << "  mean " << seconds(d.mean) << "  max " << seconds(d.maximum)
            << "  sd " << seconds(d.standardDeviation) << "  bins";
        for (const int count : d.histogram) {
            out << ' ' << count;
        }
        out << '\n';
    }
    out << '\n';
}

void printSectorComparison(std::ostream& out, const RaceSimulator& simulator,
                           const DriverAnalytics& analytics, int carA, int carB) {
    if (!analytics.contains(carA) || !analytics.contains(carB)) {
        throw std::invalid_argument("--sector-compare: both cars must be in the race");
    }
    const std::vector<SectorComparisonRow> rows = compareSectors(analytics, carA, carB);
    out << "Sector comparison   car " << carA << " against car " << carB << "   seconds\n"
        << kModelledNote;
    out << "  " << std::left << std::setw(8) << "sector" << std::right << std::setw(12) << "fastest A"
        << std::setw(12) << "fastest B" << std::setw(12) << "average A" << std::setw(12)
        << "average B" << std::setw(12) << "A - B" << '\n';
    for (const SectorComparisonRow& r : rows) {
        out << "  " << std::left << std::setw(8) << sectorName(simulator, r.sector) << std::right
            << std::setw(12) << seconds(r.fastestA) << std::setw(12) << seconds(r.fastestB)
            << std::setw(12) << seconds(r.averageA) << std::setw(12) << seconds(r.averageB)
            << std::setw(12) << seconds(r.averageGap) << '\n';
    }
    out << "  A negative A - B means car " << carA << " was quicker on average in that sector.\n\n";
}

}  // namespace boxbox
