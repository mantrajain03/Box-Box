#include "RaceLapReport.hpp"

#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace boxbox {

namespace {

std::string seconds(double value) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(3) << value;
    return out.str();
}

}  // namespace

void printRaceLapReport(std::ostream& out, const RaceSimulator& simulator,
                        const RaceLapRecorder& recorder, int firstLap, int lastLap) {
    if (firstLap < 1 || firstLap > lastLap || lastLap > recorder.lapsObserved()) {
        throw std::out_of_range("--race-laps: laps " + std::to_string(firstLap) + " to " +
                                std::to_string(lastLap) + " are not inside the " +
                                std::to_string(recorder.lapsObserved()) + " laps run");
    }

    out << "Race laps " << firstLap << " - " << lastLap
        << "   lap times include traffic, not pit loss\n";
    out << "  " << std::right << std::setw(4) << "#" << "  " << std::left << std::setw(16)
        << "driver" << std::right << std::setw(14) << "fastest lap" << std::setw(9) << "on lap"
        << std::setw(16) << "window total" << std::setw(14) << "traffic lost" << '\n';

    const std::vector<Driver>& drivers = simulator.drivers();
    for (std::size_t i = 0; i < drivers.size(); ++i) {
        const LapTimeIndex& laps = recorder.carLaps(i);
        const LapTimeIndex::Lap best = laps.fastestLap(firstLap, lastLap);
        out << "  " << std::right << std::setw(4) << drivers[i].number() << "  " << std::left
            << std::setw(16) << drivers[i].name() << std::right << std::setw(14)
            << seconds(best.seconds) << std::setw(9) << best.lap << std::setw(16)
            << seconds(laps.totalTime(firstLap, lastLap)) << std::setw(14)
            << seconds(recorder.trafficLoss(i, firstLap, lastLap)) << '\n';
    }

    const std::int64_t swaps = recorder.swapsBetween(firstLap - 1, lastLap);
    out << "\n  Position swaps during laps " << firstLap << " - " << lastLap << ": " << swaps
        << (swaps == 1 ? " pair" : " pairs") << " of cars changed places\n\n";
}

}  // namespace boxbox
