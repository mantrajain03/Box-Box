// Box Box: checks for the JSON writer and the frontend export bundle.
//
// The bundle is parsed back by a small JSON reader written here, so the tests
// check structure and numbers rather than substrings. Same harness style as the
// other test files; nothing here is random or clock-dependent (NFR-5).

#include "ExportTests.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "ExportCommand.hpp"
#include "TrackOutline.hpp"
#include "boxbox/Common.hpp"
#include "boxbox/JsonWriter.hpp"
#include "boxbox/PitStrategy.hpp"
#include "boxbox/StrategyAnalytics.hpp"
#include "boxbox/TireCompound.hpp"
#include "boxbox/Track.hpp"

namespace {

using boxbox::JsonWriter;
using boxbox::PitStrategy;
using boxbox::TireCompound;
using boxbox::Track;

// ---------------------------------------------------------------------------
// Harness
// ---------------------------------------------------------------------------

ExportTestTally g_tally;
std::string g_section;

void beginSection(const std::string& name) {
    g_section = name;
    std::cout << "\n== " << name << '\n';
}

void reportCheck(bool ok, const char* expression, const char* file, int line,
                 const std::string& detail) {
    ++g_tally.checks;
    if (ok) {
        return;
    }
    ++g_tally.failures;
    std::cout << "   FAIL  " << file << ':' << line << "  in " << g_section << '\n';
    std::cout << "         " << expression << '\n';
    if (!detail.empty()) {
        std::cout << "         " << detail << '\n';
    }
}

#define XCHECK(expr) reportCheck((expr), #expr, __FILE__, __LINE__, std::string())
#define XCHECK_MSG(expr, msg) reportCheck((expr), #expr, __FILE__, __LINE__, (msg))
#define XCHECK_NEAR(actual, expected, tolerance)                                        \
    reportCheck(std::fabs((actual) - (expected)) <= (tolerance), #actual " ~= " #expected, \
                __FILE__, __LINE__,                                                     \
                "actual " + std::to_string(actual) + "  expected " + std::to_string(expected))

#define XCHECK_THROWS(statement, ExType)                                                \
    do {                                                                                \
        bool threw = false;                                                             \
        try {                                                                           \
            statement;                                                                  \
        } catch (const ExType&) {                                                       \
            threw = true;                                                               \
        } catch (...) {                                                                 \
        }                                                                               \
        reportCheck(threw, "throws " #ExType ": " #statement, __FILE__, __LINE__,       \
                    std::string());                                                     \
    } while (false)

// ---------------------------------------------------------------------------
// A small strict JSON reader (RFC 8259 grammar, no extensions)
// ---------------------------------------------------------------------------

struct Json {
    enum class Kind { Null, Bool, Number, String, Array, Object };

    Kind kind = Kind::Null;
    bool boolean = false;
    double number = 0.0;
    std::string text;
    std::vector<Json> items;          // array elements, or object member values
    std::vector<std::string> names;   // object member names, parallel to `items`

    const Json* find(const std::string& name) const {
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (names[i] == name) {
                return &items[i];
            }
        }
        return nullptr;
    }

    const Json& at(const std::string& name) const {
        const Json* member = find(name);
        if (member == nullptr) {
            throw std::out_of_range("no member \"" + name + "\"");
        }
        return *member;
    }

    const Json& at(std::size_t index) const { return items.at(index); }
    std::size_t size() const { return items.size(); }
};

class JsonReader {
public:
    explicit JsonReader(const std::string& source) : s_(source) {}

    /// True and `out` filled when the whole input is exactly one valid value.
    bool parse(Json& out) {
        pos_ = 0;
        if (!value(out, 0)) {
            return false;
        }
        skipSpace();
        return pos_ == s_.size();
    }

private:
    static constexpr int kMaxDepth = 64;

    void skipSpace() {
        while (pos_ < s_.size() &&
               (s_[pos_] == ' ' || s_[pos_] == '\n' || s_[pos_] == '\r' || s_[pos_] == '\t')) {
            ++pos_;
        }
    }

    bool literal(const char* word) {
        std::size_t i = 0;
        while (word[i] != '\0') {
            if (pos_ + i >= s_.size() || s_[pos_ + i] != word[i]) {
                return false;
            }
            ++i;
        }
        pos_ += i;
        return true;
    }

    bool stringValue(std::string& out) {
        if (pos_ >= s_.size() || s_[pos_] != '"') {
            return false;
        }
        ++pos_;
        out.clear();
        while (pos_ < s_.size()) {
            const char ch = s_[pos_++];
            if (ch == '"') {
                return true;
            }
            if (static_cast<unsigned char>(ch) < 0x20U) {
                return false;  // raw control characters are illegal in a JSON string
            }
            if (ch != '\\') {
                out.push_back(ch);
                continue;
            }
            if (pos_ >= s_.size()) {
                return false;
            }
            const char esc = s_[pos_++];
            switch (esc) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'u': {
                    if (pos_ + 4U > s_.size()) {
                        return false;
                    }
                    unsigned code = 0;
                    for (int k = 0; k < 4; ++k) {
                        const char hex = s_[pos_++];
                        code <<= 4U;
                        if (hex >= '0' && hex <= '9') code |= static_cast<unsigned>(hex - '0');
                        else if (hex >= 'a' && hex <= 'f') code |= static_cast<unsigned>(hex - 'a' + 10);
                        else if (hex >= 'A' && hex <= 'F') code |= static_cast<unsigned>(hex - 'A' + 10);
                        else return false;
                    }
                    out.push_back(code < 0x80U ? static_cast<char>(code) : '?');
                    break;
                }
                default: return false;
            }
        }
        return false;  // unterminated
    }

    bool numberValue(double& out) {
        const std::size_t start = pos_;
        if (pos_ < s_.size() && s_[pos_] == '-') ++pos_;
        if (pos_ >= s_.size()) return false;
        if (s_[pos_] == '0') {
            ++pos_;
        } else if (s_[pos_] >= '1' && s_[pos_] <= '9') {
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        } else {
            return false;
        }
        if (pos_ < s_.size() && s_[pos_] == '.') {
            ++pos_;
            const std::size_t digits = pos_;
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
            if (pos_ == digits) return false;
        }
        if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) ++pos_;
            const std::size_t digits = pos_;
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
            if (pos_ == digits) return false;
        }
        out = std::strtod(s_.substr(start, pos_ - start).c_str(), nullptr);
        return true;
    }

    bool value(Json& out, int depth) {
        if (depth > kMaxDepth) {
            return false;
        }
        skipSpace();
        if (pos_ >= s_.size()) {
            return false;
        }
        const char ch = s_[pos_];

        if (ch == '{') {
            ++pos_;
            out.kind = Json::Kind::Object;
            skipSpace();
            if (pos_ < s_.size() && s_[pos_] == '}') {
                ++pos_;
                return true;
            }
            for (;;) {
                skipSpace();
                std::string name;
                if (!stringValue(name)) return false;
                skipSpace();
                if (pos_ >= s_.size() || s_[pos_] != ':') return false;
                ++pos_;
                Json member;
                if (!value(member, depth + 1)) return false;
                out.names.push_back(name);
                out.items.push_back(member);
                skipSpace();
                if (pos_ >= s_.size()) return false;
                if (s_[pos_] == ',') { ++pos_; continue; }
                if (s_[pos_] == '}') { ++pos_; return true; }
                return false;
            }
        }
        if (ch == '[') {
            ++pos_;
            out.kind = Json::Kind::Array;
            skipSpace();
            if (pos_ < s_.size() && s_[pos_] == ']') {
                ++pos_;
                return true;
            }
            for (;;) {
                Json element;
                if (!value(element, depth + 1)) return false;
                out.items.push_back(element);
                skipSpace();
                if (pos_ >= s_.size()) return false;
                if (s_[pos_] == ',') { ++pos_; continue; }
                if (s_[pos_] == ']') { ++pos_; return true; }
                return false;
            }
        }
        if (ch == '"') {
            out.kind = Json::Kind::String;
            return stringValue(out.text);
        }
        if (ch == 't') { out.kind = Json::Kind::Bool; out.boolean = true; return literal("true"); }
        if (ch == 'f') { out.kind = Json::Kind::Bool; out.boolean = false; return literal("false"); }
        if (ch == 'n') { out.kind = Json::Kind::Null; return literal("null"); }
        out.kind = Json::Kind::Number;
        return numberValue(out.number);
    }

    const std::string& s_;
    std::size_t pos_ = 0;
};

bool parseJson(const std::string& text, Json& out) {
    JsonReader reader(text);
    return reader.parse(out);
}

// ---------------------------------------------------------------------------
// Builders
// ---------------------------------------------------------------------------

Track makeTrack(int laps, double pitLoss, const std::vector<double>& sectors) {
    Track track("Test Circuit", laps, pitLoss);
    for (std::size_t i = 0; i < sectors.size(); ++i) {
        track.addSector("S" + std::to_string(i + 1U), sectors[i]);
    }
    return track;
}

/// The bundle for `strategy`, as text. `failure` is empty on success.
std::string bundleFor(const PitStrategy& strategy, std::string& failure,
                      const boxbox::ExportSettings& settings = boxbox::ExportSettings()) {
    std::ostringstream out;
    failure.clear();
    const bool ok = boxbox::writeBundleJson(out, strategy, settings, failure);
    if (!ok && failure.empty()) {
        failure = "writeBundleJson returned false with no reason";
    }
    return out.str();
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

void testJsonWriter() {
    beginSection("JsonWriter");

    {
        std::ostringstream out;
        JsonWriter json(out);
        json.beginObject()
            .key("a").integer(1)
            .key("b").beginArray().number(1.5).boolean(true).null().string("x").endArray()
            .key("c").beginObject().endObject()
            .endObject();
        XCHECK(json.complete());
        XCHECK(out.str() == "{\"a\":1,\"b\":[1.5,true,null,\"x\"],\"c\":{}}");
    }

    // Escaping, including a control character, and a round trip through the reader.
    {
        std::ostringstream out;
        JsonWriter json(out);
        const std::string nasty = std::string("q\"b\\n\n\t") + '\x01' + "end";
        json.beginArray().string(nasty).endArray();
        Json parsed;
        XCHECK(parseJson(out.str(), parsed));
        XCHECK(parsed.kind == Json::Kind::Array && parsed.size() == 1U);
        if (parsed.size() == 1U) {
            XCHECK_MSG(parsed.at(0).text == nasty, "an escaped string must read back unchanged");
        }
    }

    // Numbers: no NaN or infinity in JSON, and no negative zero.
    {
        std::ostringstream out;
        JsonWriter json(out);
        json.beginArray()
            .number(std::nan(""))
            .number(boxbox::kUnreachable)
            .number(-0.0)
            .number(4549.28)
            .number(1.0e-7)
            .endArray();
        XCHECK(out.str() == "[null,null,0,4549.28,1e-07]");
        Json parsed;
        XCHECK(parseJson(out.str(), parsed));
    }

    // Misuse is an error, never silently broken JSON.
    {
        std::ostringstream out;
        JsonWriter json(out);
        json.beginObject();
        XCHECK_THROWS(json.integer(1), std::logic_error);      // value with no key
        XCHECK_THROWS(json.endArray(), std::logic_error);      // wrong closer
        json.key("k");
        XCHECK_THROWS(json.key("again"), std::logic_error);    // two keys in a row
        XCHECK_THROWS(json.endObject(), std::logic_error);     // dangling key
        json.integer(2).endObject();
        XCHECK(json.complete());
        XCHECK_THROWS(json.integer(3), std::logic_error);      // second root value
    }
    {
        std::ostringstream out;
        JsonWriter json(out);
        json.beginArray();
        XCHECK_THROWS(json.key("k"), std::logic_error);        // key inside an array
        XCHECK(!json.complete());
    }
}

void testBundleStructure() {
    beginSection("Export bundle: valid JSON and the expected sections");

    const PitStrategy strategy(makeTrack(50, 22.0, {28.4, 31.2, 30.4}), boxbox::defaultCompounds());
    std::string failure;
    const std::string text = bundleFor(strategy, failure);
    XCHECK_MSG(failure.empty(), failure);

    Json root;
    const bool parsed = parseJson(text, root);
    XCHECK_MSG(parsed, "the bundle must be one valid JSON document");
    if (!parsed) {
        return;
    }

    try {
        XCHECK(root.kind == Json::Kind::Object);
        const Json& meta = root.at("meta");
        XCHECK(meta.at("laps").number > 49.5 && meta.at("laps").number < 50.5);
        XCHECK_NEAR(meta.at("pitLoss").number, 22.0, boxbox::kTimeEpsilon);
        XCHECK_NEAR(meta.at("closureTolerance").number, boxbox::kClosureTolerance, 1.0e-15);

        const Json& track = root.at("track");
        XCHECK(track.at("sectors").size() == 3U);
        XCHECK_NEAR(track.at("baseLapTime").number, 90.0, boxbox::kTimeEpsilon);

        XCHECK(root.at("compounds").size() == strategy.compounds().size());
        const Json& table = root.at("strategy").at("stintCostTable");
        XCHECK(table.size() == strategy.compounds().size());
        for (std::size_t c = 0; c < table.size(); ++c) {
            XCHECK(static_cast<int>(table.at(c).size()) == strategy.compounds()[c].maxStintLaps());
            // The exported table is the optimizer's own prefix-sum lookup.
            for (std::size_t l = 0; l < table.at(c).size(); ++l) {
                XCHECK_NEAR(table.at(c).at(l).number,
                            strategy.stintCost(c, static_cast<int>(l) + 1),
                            1.0e-8);
            }
        }
    } catch (const std::exception& error) {
        XCHECK_MSG(false, std::string("missing section: ") + error.what());
    }
}

void testBundleMatchesOptimizer() {
    beginSection("Export bundle: numbers are the optimizer's, FR-8 closes");

    const PitStrategy strategy(makeTrack(50, 22.0, {28.4, 31.2, 30.4}), boxbox::defaultCompounds());
    const boxbox::StrategyResult optimal = strategy.optimize();
    std::string failure;
    const std::string text = bundleFor(strategy, failure);
    Json root;
    if (!parseJson(text, root)) {
        XCHECK_MSG(false, "bundle is not valid JSON");
        return;
    }

    try {
        const Json& strat = root.at("strategy");
        const Json& exported = strat.at("optimal");

        // The exported optimum is PitStrategy::optimize(), to the writer's 12 digits.
        XCHECK_NEAR(exported.at("totalTime").number, optimal.totalTime, 1.0e-7);
        XCHECK_NEAR(exported.at("dpValue").number, optimal.dpValue, 1.0e-7);
        XCHECK(exported.at("stints").size() == optimal.stints.size());

        // Stint lengths sum to exactly totalLaps.
        int planned = 0;
        for (std::size_t i = 0; i < exported.at("stints").size(); ++i) {
            planned += static_cast<int>(exported.at("stints").at(i).at("laps").number + 0.5);
        }
        XCHECK(planned == 50);

        // The DP trace has one cell per lap plus lap 0 and ends at dp[N].
        const Json& dp = strat.at("dp");
        XCHECK(dp.size() == 51U);
        XCHECK_NEAR(dp.at(50).at("cost").number, optimal.dpValue, 1.0e-7);
        XCHECK_NEAR(dp.at(0).at("cost").number, 0.0, boxbox::kTimeEpsilon);
        for (std::size_t lap = 1; lap < dp.size(); ++lap) {
            XCHECK(dp.at(lap).at("byCompound").size() == strategy.compounds().size());
        }

        // The backtrack is the optimizer's plan.
        const Json& back = strat.at("backtrack");
        XCHECK(back.size() == optimal.stints.size());
        for (std::size_t i = 0; i < back.size() && i < optimal.stints.size(); ++i) {
            XCHECK(static_cast<std::size_t>(back.at(i).at("compound").number + 0.5) ==
                   optimal.stints[i].compoundIndex);
            XCHECK(static_cast<int>(back.at(i).at("laps").number + 0.5) == optimal.stints[i].laps);
        }

        // Brute force agrees with the DP on the short verification race.
        const Json& verification = strat.at("verification");
        XCHECK(verification.at("laps").number < 8.5);
        XCHECK_NEAR(verification.at("gap").number, 0.0, boxbox::kClosureTolerance);

        // Stop counts 0..3; the best of them cannot beat the optimum.
        const Json& stops = strat.at("stopCounts");
        XCHECK(stops.size() == 4U);
        for (std::size_t i = 0; i < stops.size(); ++i) {
            if (stops.at(i).at("feasible").boolean) {
                XCHECK(stops.at(i).at("report").at("totalTime").number >=
                       optimal.totalTime - boxbox::kClosureTolerance);
            }
        }

        // Alternatives: cheapest first, entry 0 is the optimum.
        const Json& alternatives = strat.at("alternatives");
        XCHECK(alternatives.size() == 5U);
        XCHECK_NEAR(alternatives.at(0).at("totalTime").number, optimal.totalTime, 1.0e-7);
        for (std::size_t i = 1; i < alternatives.size(); ++i) {
            XCHECK(alternatives.at(i).at("totalTime").number >=
                   alternatives.at(i - 1U).at("totalTime").number - boxbox::kClosureTolerance);
        }

        // Default sweep 0:40:2 gives 21 rows; a free stop beats an expensive one.
        const Json& rows = strat.at("sweep").at("rows");
        XCHECK(rows.size() == 21U);
        XCHECK(rows.at(0).at("totalTime").number < rows.at(20).at("totalTime").number);

        // FR-8, through the bundle.
        const Json& closure = root.at("race").at("closure");
        XCHECK(closure.at("passed").boolean);
        XCHECK_NEAR(closure.at("simulatedTotal").number, optimal.totalTime,
                    boxbox::kClosureTolerance);
    } catch (const std::exception& error) {
        XCHECK_MSG(false, std::string("bundle is missing a field: ") + error.what());
    }
}

void testBundleScenarios() {
    beginSection("Export bundle: one full scenario per pit loss");

    const Track base = makeTrack(50, 22.0, {28.4, 31.2, 30.4});
    const PitStrategy strategy(base, boxbox::defaultCompounds());
    std::string failure;
    const std::string text = bundleFor(strategy, failure);
    Json root;
    if (!parseJson(text, root)) {
        XCHECK_MSG(false, "bundle is not valid JSON");
        return;
    }

    try {
        const Json& scenarios = root.at("strategy").at("scenarios");
        XCHECK(scenarios.size() == 21U);  // 0..40 step 2 already contains the race's own 22 s

        double previousLoss = -1.0;
        double previousStops = 1.0e9;
        bool hasBase = false;
        for (std::size_t i = 0; i < scenarios.size(); ++i) {
            const Json& s = scenarios.at(i);
            const double pitLoss = s.at("pitLoss").number;
            XCHECK_MSG(pitLoss > previousLoss, "scenarios must be sorted by pit loss");
            previousLoss = pitLoss;

            // The scenario IS the optimizer's answer at that pit loss.
            Track track = base;
            track.setPitLoss(pitLoss);
            const PitStrategy independent(track, boxbox::defaultCompounds());
            const boxbox::StrategyResult expected = independent.optimize();
            const Json& optimal = s.at("optimal");
            XCHECK_NEAR(optimal.at("totalTime").number, expected.totalTime, 1.0e-7);
            XCHECK(static_cast<int>(optimal.at("stops").number + 0.5) == expected.stopCount());

            // The DP trace ends at dp[N] and has one cell per lap plus lap 0.
            const Json& dp = s.at("dp");
            XCHECK(dp.size() == 51U);
            XCHECK_NEAR(dp.at(50).at("cost").number, expected.dpValue, 1.0e-7);

            // The backtrack tiles the race.
            int planned = 0;
            for (std::size_t b = 0; b < s.at("backtrack").size(); ++b) {
                planned += static_cast<int>(s.at("backtrack").at(b).at("laps").number + 0.5);
            }
            XCHECK(planned == 50);

            XCHECK(s.at("stopCounts").size() == 4U);
            XCHECK_NEAR(s.at("verification").at("gap").number, 0.0, boxbox::kClosureTolerance);

            // A dearer stop never means more stops.
            const double stops = optimal.at("stops").number;
            XCHECK_MSG(stops <= previousStops, "optimal stop count must not rise with pit loss");
            previousStops = stops;

            if (std::fabs(pitLoss - 22.0) < 1.0e-9) {
                hasBase = true;
                XCHECK_NEAR(optimal.at("totalTime").number,
                            root.at("strategy").at("optimal").at("totalTime").number, 1.0e-7);
            }
        }
        XCHECK(hasBase);
    } catch (const std::exception& error) {
        XCHECK_MSG(false, std::string("bundle is missing a field: ") + error.what());
    }

    // A pit loss that is not on the sweep grid is added, and the list stays sorted.
    {
        const PitStrategy offGrid(makeTrack(20, 21.3, {90.0}), boxbox::defaultCompounds());
        std::string offFailure;
        const std::string offText = bundleFor(offGrid, offFailure);
        Json offRoot;
        XCHECK_MSG(offFailure.empty(), offFailure);
        XCHECK(parseJson(offText, offRoot));
        if (offRoot.kind == Json::Kind::Object) {
            const Json& list = offRoot.at("strategy").at("scenarios");
            XCHECK(list.size() == 22U);
            bool found = false;
            for (std::size_t i = 0; i < list.size(); ++i) {
                found = found || std::fabs(list.at(i).at("pitLoss").number - 21.3) < 1.0e-9;
                if (i > 0U) {
                    XCHECK(list.at(i).at("pitLoss").number > list.at(i - 1U).at("pitLoss").number);
                }
            }
            XCHECK_MSG(found, "the race's own pit loss must be one of the scenarios");
        }
    }
}

void testBundleTires() {
    beginSection("Export bundle: tire profiles, range queries, thresholds, windows");

    const Track track = makeTrack(50, 22.0, {28.4, 31.2, 30.4});
    const std::vector<TireCompound> compounds = boxbox::defaultCompounds();
    const PitStrategy strategy(track, compounds);
    std::string failure;
    const std::string text = bundleFor(strategy, failure);
    Json root;
    if (!parseJson(text, root)) {
        XCHECK_MSG(false, "bundle is not valid JSON");
        return;
    }

    try {
        const Json& tires = root.at("tires");
        const Json& profiles = tires.at("profiles");
        XCHECK(profiles.size() == compounds.size());
        XCHECK_NEAR(tires.at("baseLapTime").number, track.baseLapTime(), boxbox::kTimeEpsilon);

        const Json& limits = tires.at("limits");
        XCHECK(limits.size() > 10U);
        XCHECK_NEAR(limits.at(0).number, 0.0, boxbox::kTimeEpsilon);
        for (std::size_t i = 1; i < limits.size(); ++i) {
            XCHECK(limits.at(i).number > limits.at(i - 1U).number);
        }

        for (std::size_t c = 0; c < profiles.size(); ++c) {
            const Json& p = profiles.at(c);
            const TireCompound& compound = compounds[c];
            const std::size_t m = static_cast<std::size_t>(compound.maxStintLaps());
            XCHECK(p.at("name").text == compound.name());
            XCHECK(p.at("lapTime").size() == m);
            XCHECK(p.at("stintCost").size() == m + 1U);
            XCHECK(p.at("nonDecreasing").boolean);

            // Lap time is the track plus the compound's own model; stint cost is the optimizer's table.
            std::vector<double> lap;
            for (std::size_t a = 0; a < m; ++a) {
                lap.push_back(p.at("lapTime").at(a).number);
                XCHECK_NEAR(lap.back(), track.baseLapTime() + compound.lapDelta(static_cast<int>(a)), 1.0e-8);
                XCHECK_NEAR(p.at("degradationVsFresh").at(a).number, lap.back() - lap.front(), 1.0e-8);
            }
            for (std::size_t k = 1; k <= m; ++k) {
                XCHECK_NEAR(p.at("stintCost").at(k).number, strategy.stintCost(c, static_cast<int>(k)), 1.0e-7);
            }
            XCHECK_NEAR(p.at("stintCost").at(0).number, 0.0, boxbox::kTimeEpsilon);

            // The cliff.
            if (compound.hasCliff()) {
                XCHECK(static_cast<int>(p.at("cliffLap").number + 0.5) == compound.cliffLap());
                XCHECK(p.at("cliffActivationAge").kind == Json::Kind::Number);
                XCHECK(p.at("cliffStepSeconds").kind == Json::Kind::Number);
            }

            // Every range query against a plain scan.
            const Json& ranges = p.at("ranges");
            XCHECK(ranges.kind == Json::Kind::Object);
            for (std::size_t b = 0; b < m; ++b) {
                double sum = 0.0, lowest = boxbox::kUnreachable, loss = 0.0;
                for (std::size_t e = b + 1U; e <= m; ++e) {
                    sum += lap[e - 1U];
                    lowest = std::min(lowest, lap[e - 1U]);
                    loss += lap[e - 1U] - lap[0];
                    const std::size_t i = e - b - 1U;
                    XCHECK_NEAR(ranges.at("sum").at(b).at(i).number, sum, 1.0e-7);
                    XCHECK_NEAR(ranges.at("average").at(b).at(i).number, sum / static_cast<double>(e - b), 1.0e-7);
                    XCHECK_NEAR(ranges.at("min").at(b).at(i).number, lowest, 1.0e-8);
                    XCHECK_NEAR(ranges.at("degradation").at(b).at(i).number, loss, 1.0e-7);
                }
            }

            // Every threshold answer against a plain scan, and every bisection step is legal.
            const Json& queries = p.at("thresholds");
            XCHECK(queries.size() == limits.size());
            for (std::size_t q = 0; q < queries.size(); ++q) {
                const double limit = limits.at(q).number;
                int expected = -1;
                for (std::size_t a = 0; a < m && expected < 0; ++a) {
                    if (p.at("degradationVsFresh").at(a).number > limit + boxbox::kTimeEpsilon) {
                        expected = static_cast<int>(a);
                    }
                }
                const Json& answer = queries.at(q).at("age");
                if (expected < 0) {
                    XCHECK_MSG(answer.kind == Json::Kind::Null, "no age should exceed this limit");
                } else {
                    XCHECK(answer.kind == Json::Kind::Number && static_cast<int>(answer.number + 0.5) == expected);
                }

                const Json& steps = queries.at(q).at("steps");
                XCHECK_MSG(steps.size() <= 8U, "bisection over 45 ages takes at most 6-7 steps");
                if (steps.size() > 0U) {
                    XCHECK(static_cast<std::size_t>(steps.at(0).at(0).number + 0.5) == 0U);
                    XCHECK(static_cast<std::size_t>(steps.at(0).at(1).number + 0.5) == m);
                }
                for (std::size_t s = 0; s < steps.size(); ++s) {
                    const double lo = steps.at(s).at(0).number, hi = steps.at(s).at(1).number, mid = steps.at(s).at(2).number;
                    XCHECK(lo < hi && lo <= mid && mid < hi);
                }
            }
            // The last limit is past every compound's wear, so nothing exceeds it.
            XCHECK(queries.at(queries.size() - 1U).at("age").kind == Json::Kind::Null);
        }

        // Pairwise comparison against a plain scan.
        const Json& comparisons = tires.at("comparisons");
        XCHECK(comparisons.size() == 3U);
        for (std::size_t i = 0; i < comparisons.size(); ++i) {
            const Json& cmp = comparisons.at(i);
            const std::size_t a = static_cast<std::size_t>(cmp.at("a").number + 0.5);
            const std::size_t b = static_cast<std::size_t>(cmp.at("b").number + 0.5);
            const std::size_t both = static_cast<std::size_t>(std::min(compounds[a].maxStintLaps(), compounds[b].maxStintLaps()));
            XCHECK(static_cast<std::size_t>(cmp.at("agesCompared").number + 0.5) == both);
            int firstSlower = -1;
            for (std::size_t age = 0; age < both && firstSlower < 0; ++age) {
                if (compounds[a].lapDelta(static_cast<int>(age)) > compounds[b].lapDelta(static_cast<int>(age)) + boxbox::kTimeEpsilon) {
                    firstSlower = static_cast<int>(age);
                }
            }
            if (firstSlower < 0) {
                XCHECK(cmp.at("firstSlowerAge").kind == Json::Kind::Null);
            } else {
                XCHECK(static_cast<int>(cmp.at("firstSlowerAge").number + 0.5) == firstSlower);
            }
        }

        // Sliding windows against a plain scan, in both runs.
        for (const char* name : {"overtakeOff", "overtakeOn"}) {
            const Json& windows = root.at("race").at("runs").at(name).at("overview").at("windows");
            XCHECK(windows.at("sizes").size() == 3U);
            const Json& cars = windows.at("cars");
            XCHECK(cars.size() == 1U + compounds.size());
            for (std::size_t ci = 0; ci < cars.size(); ++ci) {
                const Json& car = cars.at(ci);
                const Json& times = car.at("lapTimes");
                XCHECK(times.size() == 50U);
                for (const std::size_t w : {3U, 5U, 10U}) {
                    const Json& series = car.at("w" + std::to_string(w));
                    for (std::size_t k = 0; k < times.size(); ++k) {
                        const std::size_t first = (k + 1U > w) ? k + 1U - w : 0U;
                        double lowest = boxbox::kUnreachable, highest = -boxbox::kUnreachable, sum = 0.0;
                        std::size_t lowestAt = first, highestAt = first;
                        for (std::size_t j = first; j <= k; ++j) {
                            const double t = times.at(j).number;
                            sum += t;
                            if (t < lowest) { lowest = t; lowestAt = j; }
                            if (t > highest) { highest = t; highestAt = j; }
                        }
                        XCHECK_NEAR(series.at("fastest").at(k).number, lowest, 1.0e-8);
                        XCHECK_NEAR(series.at("slowest").at(k).number, highest, 1.0e-8);
                        XCHECK_NEAR(series.at("average").at(k).number, sum / static_cast<double>(k + 1U - first), 1.0e-7);
                        XCHECK(static_cast<std::size_t>(series.at("fastestLap").at(k).number + 0.5) == lowestAt + 1U);
                        XCHECK(static_cast<std::size_t>(series.at("slowestLap").at(k).number + 0.5) == highestAt + 1U);
                    }
                }
            }
        }
    } catch (const std::exception& error) {
        XCHECK_MSG(false, std::string("bundle is missing a field: ") + error.what());
    }

    // A compound with no cliff, one stint too long for the range matrix, and a single compound.
    {
        std::vector<TireCompound> custom;
        custom.emplace_back("LONG", 0.0, 0.01, 120);
        const PitStrategy longStrategy(makeTrack(10, 20.0, {90.0}), custom);
        std::string customFailure;
        const std::string customText = bundleFor(longStrategy, customFailure);
        Json customRoot;
        XCHECK_MSG(customFailure.empty(), customFailure);
        XCHECK(parseJson(customText, customRoot));
        if (customRoot.kind == Json::Kind::Object) {
            const Json& p = customRoot.at("tires").at("profiles").at(0);
            XCHECK(p.at("ranges").kind == Json::Kind::Null);        // 120 ages is over the cap
            XCHECK(p.at("cliffActivationAge").kind == Json::Kind::Null);
            XCHECK(static_cast<int>(p.at("cliffLap").number - 0.5) < 0);
            XCHECK(customRoot.at("tires").at("comparisons").size() == 0U);
        }
    }
}

void testBundleDriverAnalytics() {
    beginSection("Export bundle: rankings, top laps, sectors, range queries, tree nodes");

    const PitStrategy strategy(makeTrack(50, 22.0, {28.4, 31.2, 30.4}), boxbox::defaultCompounds());
    std::string failure;
    const std::string text = bundleFor(strategy, failure);
    XCHECK_MSG(failure.empty(), failure);
    Json root;
    if (!parseJson(text, root)) {
        XCHECK_MSG(false, "bundle is not valid JSON");
        return;
    }

    try {
        const std::size_t N = 50;

        // --- structures: the nodes a query touches ---------------------------------
        const Json& structures = root.at("structures");
        XCHECK(structures.at("segmentNodes").size() == N);
        XCHECK(structures.at("fenwickNodes").size() == N + 1U);
        for (std::size_t b = 0; b < N; ++b) {
            const Json& row = structures.at("segmentNodes").at(b);
            XCHECK(row.size() == N - b);
            for (std::size_t i = 0; i < row.size(); ++i) {
                // The nodes lie inside [b + 1, b + i + 1] and together cover it exactly.
                std::size_t covered = 0;
                for (std::size_t k = 0; k < row.at(i).size(); ++k) {
                    const double first = row.at(i).at(k).at(1).number, last = row.at(i).at(k).at(2).number;
                    XCHECK(first >= static_cast<double>(b + 1U) && last <= static_cast<double>(b + i + 1U) && first <= last);
                    covered += static_cast<std::size_t>(last - first + 1.5);
                }
                XCHECK(covered == i + 1U);
                XCHECK_MSG(row.at(i).size() <= 12U, "a range needs at most 2 nodes per level");
            }
        }
        for (std::size_t k = 0; k <= N; ++k) {
            // A Fenwick prefix of k laps is covered exactly by disjoint nodes ending at k, k - lowbit(k), ...
            const Json& nodes = structures.at("fenwickNodes").at(k);
            std::size_t covered = 0, expectedEnd = k;
            for (std::size_t i = 0; i < nodes.size(); ++i) {
                const std::size_t id = static_cast<std::size_t>(nodes.at(i).at(0).number + 0.5);
                const std::size_t last = static_cast<std::size_t>(nodes.at(i).at(2).number + 0.5);
                const std::size_t first = static_cast<std::size_t>(nodes.at(i).at(1).number + 0.5);
                XCHECK(id == last && last == expectedEnd);
                covered += last - first + 1U;
                expectedEnd = first - 1U;
            }
            XCHECK(covered == k);
        }

        for (const char* name : {"overtakeOff", "overtakeOn"}) {
            const Json& run = root.at("race").at("runs").at(name);
            const Json& a = run.at("analytics");
            const Json& cars = run.at("overview").at("windows").at("cars");
            const std::size_t carCount = cars.size();
            const bool trafficOn = run.at("overtake").at("enabled").boolean;

            // Lap times by car, from the windows block (the same LapRecords).
            std::vector<std::vector<double>> times(carCount + 1U);
            std::vector<double> everyLap;
            for (std::size_t c = 0; c < carCount; ++c) {
                const std::size_t car = static_cast<std::size_t>(cars.at(c).at("car").number + 0.5);
                for (std::size_t l = 0; l < cars.at(c).at("lapTimes").size(); ++l) {
                    times[car].push_back(cars.at(c).at("lapTimes").at(l).number);
                    everyLap.push_back(times[car].back());
                }
            }
            const Json& overviewDrivers = run.at("overview").at("drivers");
            const auto statOf = [&](std::size_t car, const std::string& metric) {
                for (std::size_t i = 0; i < overviewDrivers.size(); ++i) {
                    if (static_cast<std::size_t>(overviewDrivers.at(i).at("car").number + 0.5) != car) { continue; }
                    if (metric == "bestLap") { return overviewDrivers.at(i).at("bestLap").at("time").number; }
                    return overviewDrivers.at(i).at(metric).number;
                }
                return -1.0;
            };

            // --- rankings: a permutation of the cars, in non-decreasing order of the metric ---
            for (const char* metric : {"averageLap", "bestLap", "lapRange", "meanAbsDeviation"}) {
                const Json& order = a.at("rankings").at(metric);
                XCHECK(order.size() == carCount);
                std::vector<bool> seen(carCount + 1U, false);
                for (std::size_t i = 0; i < order.size(); ++i) {
                    const std::size_t car = static_cast<std::size_t>(order.at(i).number + 0.5);
                    XCHECK(car >= 1U && car <= carCount && !seen[car]);
                    seen[car] = true;
                    if (i > 0U) {
                        const std::size_t previous = static_cast<std::size_t>(order.at(i - 1U).number + 0.5);
                        XCHECK_MSG(statOf(car, metric) >= statOf(previous, metric), std::string("ranking by ") + metric);
                    }
                }
            }

            // --- top laps: the K smallest and K largest lap times in the field ---
            std::vector<double> sorted = everyLap;
            std::sort(sorted.begin(), sorted.end());
            for (const std::size_t k : {3U, 5U, 10U}) {
                const Json& set = a.at("topLaps").at("field").at("k" + std::to_string(k));
                XCHECK(set.at("fastest").size() == k && set.at("slowest").size() == k);
                for (std::size_t i = 0; i < k; ++i) {
                    XCHECK_NEAR(set.at("fastest").at(i).at("time").number, sorted[i], 1.0e-9);
                    XCHECK_NEAR(set.at("slowest").at(i).at("time").number, sorted[sorted.size() - 1U - i], 1.0e-9);
                    // A listed lap really is that car's lap.
                    const std::size_t car = static_cast<std::size_t>(set.at("fastest").at(i).at("car").number + 0.5);
                    const std::size_t lap = static_cast<std::size_t>(set.at("fastest").at(i).at("lap").number + 0.5);
                    XCHECK_NEAR(times[car][lap - 1U], set.at("fastest").at(i).at("time").number, 1.0e-9);
                }
            }
            for (std::size_t c = 0; c < a.at("topLaps").at("cars").size(); ++c) {
                const Json& entry = a.at("topLaps").at("cars").at(c);
                const std::size_t car = static_cast<std::size_t>(entry.at("car").number + 0.5);
                std::vector<double> own = times[car];
                std::sort(own.begin(), own.end());
                for (std::size_t i = 0; i < 5U; ++i) {
                    XCHECK_NEAR(entry.at("k5").at("fastest").at(i).at("time").number, own[i], 1.0e-9);
                    XCHECK(static_cast<std::size_t>(entry.at("k5").at("fastest").at(i).at("car").number + 0.5) == car);
                }
            }

            // --- sectors: modelled; the sectors add back to the lap ---
            const Json& sectors = a.at("sectors");
            XCHECK(sectors.at("modelled").boolean);
            XCHECK(sectors.at("names").size() == 3U);
            XCHECK(sectors.at("pairs").size() == carCount * (carCount - 1U));
            for (std::size_t c = 0; c < sectors.at("cars").size(); ++c) {
                const Json& s = sectors.at("cars").at(c);
                double total = 0.0;
                for (std::size_t k = 0; k < s.at("average").size(); ++k) { total += s.at("average").at(k).number; }
                XCHECK_NEAR(total, statOf(static_cast<std::size_t>(s.at("car").number + 0.5), "averageLap"), 1.0e-6);
            }
            for (std::size_t p = 0; p < sectors.at("pairs").size(); ++p) {
                const Json& rows = sectors.at("pairs").at(p).at("rows");
                XCHECK(rows.size() == 3U);
                for (std::size_t r = 0; r < rows.size(); ++r) {
                    XCHECK_NEAR(rows.at(r).at("averageGap").number,
                                rows.at(r).at("averageA").number - rows.at(r).at("averageB").number, 1.0e-9);
                }
            }

            // --- ranges: every (first lap, last lap) against a plain scan ---
            const Json& ranges = a.at("ranges").at("cars");
            XCHECK(ranges.size() == carCount);
            for (std::size_t c = 0; c < ranges.size(); ++c) {
                const std::size_t car = static_cast<std::size_t>(ranges.at(c).at("car").number + 0.5);
                // Per-lap traffic from the cumulative figure on each lap row.
                std::vector<double> cumulative(1U, 0.0);
                for (std::size_t l = 0; l < run.at("laps").size(); ++l) {
                    const Json& order = run.at("laps").at(l).at("order");
                    for (std::size_t p = 0; p < order.size(); ++p) {
                        if (static_cast<std::size_t>(order.at(p).at("car").number + 0.5) == car) {
                            cumulative.push_back(order.at(p).at("traffic").number);
                        }
                    }
                }
                XCHECK(trafficOn == (ranges.at(c).at("traffic").kind == Json::Kind::Array));
                for (std::size_t b = 0; b < N; ++b) {
                    double sum = 0.0, lowest = boxbox::kUnreachable;
                    std::size_t lowestLap = b + 1U;
                    for (std::size_t e = b + 1U; e <= N; ++e) {
                        sum += times[car][e - 1U];
                        if (times[car][e - 1U] < lowest) { lowest = times[car][e - 1U]; lowestLap = e; }
                        const std::size_t i = e - b - 1U;
                        XCHECK_NEAR(ranges.at(c).at("sum").at(b).at(i).number, sum, 1.0e-7);
                        XCHECK(static_cast<std::size_t>(ranges.at(c).at("fastestLap").at(b).at(i).number + 0.5) == lowestLap);
                        if (trafficOn) {
                            XCHECK_NEAR(ranges.at(c).at("traffic").at(b).at(i).number, cumulative[e] - cumulative[b], 1.0e-7);
                        }
                    }
                }
            }
        }
    } catch (const std::exception& error) {
        XCHECK_MSG(false, std::string("bundle is missing a field: ") + error.what());
    }

    // A race past the cap exports no range matrices and no node lists, and still validates.
    {
        const PitStrategy longRace(makeTrack(101, 22.0, {28.4, 31.2, 30.4}), boxbox::defaultCompounds());
        std::string longFailure;
        const std::string longText = bundleFor(longRace, longFailure);
        Json longRoot;
        XCHECK_MSG(longFailure.empty(), longFailure);
        XCHECK(parseJson(longText, longRoot));
        if (longRoot.kind == Json::Kind::Object) {
            XCHECK(longRoot.at("structures").kind == Json::Kind::Null);
            XCHECK(longRoot.at("race").at("runs").at("overtakeOff").at("analytics").at("ranges").kind == Json::Kind::Null);
            XCHECK(longRoot.at("race").at("runs").at("overtakeOff").at("analytics").at("rankings").kind == Json::Kind::Object);
        }
    }
}

void testBundlePit() {
    beginSection("Export bundle: pit analytics, greedy pit calls, queue disorder, undercut grid");

    const Track track = makeTrack(50, 22.0, {28.4, 31.2, 30.4});
    const PitStrategy strategy(track, boxbox::defaultCompounds());
    std::string failure;
    const std::string text = bundleFor(strategy, failure);
    XCHECK_MSG(failure.empty(), failure);
    Json root;
    if (!parseJson(text, root)) {
        XCHECK_MSG(false, "bundle is not valid JSON");
        return;
    }

    try {
        const int N = 50;

        for (const char* name : {"overtakeOff", "overtakeOn"}) {
            const Json& run = root.at("race").at("runs").at(name);
            const Json& pit = run.at("pit");

            // Every stop in the lap rows, found by a plain scan.
            std::vector<std::pair<int, int>> stops;   // (lap, car)
            for (std::size_t l = 0; l < run.at("laps").size(); ++l) {
                const Json& order = run.at("laps").at(l).at("order");
                for (std::size_t p = 0; p < order.size(); ++p) {
                    if (order.at(p).at("pitted").boolean) {
                        stops.push_back({static_cast<int>(run.at("laps").at(l).at("lap").number + 0.5),
                                         static_cast<int>(order.at(p).at("car").number + 0.5)});
                    }
                }
            }

            // Per car: the stop laps match, the stints tile the race, pit time is stops x pit loss.
            int total = 0;
            for (std::size_t d = 0; d < pit.at("drivers").size(); ++d) {
                const Json& car = pit.at("drivers").at(d);
                const int number = static_cast<int>(car.at("car").number + 0.5);
                std::vector<int> expected;
                for (const auto& s : stops) { if (s.second == number) { expected.push_back(s.first); } }
                std::sort(expected.begin(), expected.end());
                XCHECK(car.at("stopLaps").size() == expected.size());
                for (std::size_t i = 0; i < expected.size() && i < car.at("stopLaps").size(); ++i) {
                    XCHECK(static_cast<int>(car.at("stopLaps").at(i).number + 0.5) == expected[i]);
                }
                int covered = 0;
                for (std::size_t i = 0; i < car.at("stintLengths").size(); ++i) {
                    covered += static_cast<int>(car.at("stintLengths").at(i).number + 0.5);
                }
                XCHECK(covered == N);
                XCHECK(car.at("stintLengths").size() == expected.size() + 1U);
                XCHECK_NEAR(car.at("pitTime").number, 22.0 * static_cast<double>(expected.size()), boxbox::kTimeEpsilon);
                XCHECK(car.at("trafficAfter").size() == expected.size());
                total += static_cast<int>(expected.size());
            }
            XCHECK(total == static_cast<int>(stops.size()));
            XCHECK_NEAR(pit.at("field").at("pitTime").number, 22.0 * static_cast<double>(total), boxbox::kTimeEpsilon);
            XCHECK(static_cast<int>(pit.at("field").at("stints").at("count").number + 0.5) == total + static_cast<int>(pit.at("drivers").size()));

            // Stops per lap bucket against a plain count.
            for (const std::size_t size : {1U, 3U, 5U}) {
                const Json& buckets = pit.at("stopBuckets").at("b" + std::to_string(size));
                int counted = 0;
                for (std::size_t b = 0; b < buckets.size(); ++b) {
                    const int from = static_cast<int>(buckets.at(b).at("from").number + 0.5);
                    const int to = static_cast<int>(buckets.at(b).at("to").number + 0.5);
                    int expected = 0;
                    for (const auto& s : stops) { if (s.first >= from && s.first <= to) { ++expected; } }
                    XCHECK(static_cast<int>(buckets.at(b).at("stops").number + 0.5) == expected);
                    counted += expected;
                }
                XCHECK(counted == total);
                XCHECK(static_cast<int>(buckets.at(buckets.size() - 1U).at("to").number + 0.5) == N);
            }

            // Chronological stops, and the pit window.
            const Json& chrono = pit.at("stops");
            XCHECK(chrono.size() == stops.size());
            for (std::size_t i = 1; i < chrono.size(); ++i) {
                XCHECK(chrono.at(i).at("time").number >= chrono.at(i - 1U).at("time").number);
            }
            int earliest = N, latest = 0;
            for (const auto& s : stops) { earliest = std::min(earliest, s.first); latest = std::max(latest, s.first); }
            XCHECK(static_cast<int>(pit.at("window").at("earliest").number + 0.5) == earliest);
            XCHECK(static_cast<int>(pit.at("window").at("latest").number + 0.5) == latest);

            // Greedy pit calls: every window holds a call, and no smaller set of laps would do.
            const Json& greedy = pit.at("greedy");
            XCHECK(greedy.size() == 11U);
            for (std::size_t g = 0; g < greedy.size(); ++g) {
                const Json& entry = greedy.at(g);
                std::vector<std::pair<int, int>> windows;
                for (std::size_t w = 0; w < entry.at("windows").size(); ++w) {
                    windows.push_back({static_cast<int>(entry.at("windows").at(w).at("earliest").number + 0.5),
                                       static_cast<int>(entry.at("windows").at(w).at("latest").number + 0.5)});
                }
                XCHECK(windows.size() == stops.size());
                std::vector<int> calls;
                for (std::size_t c = 0; c < entry.at("calls").size(); ++c) { calls.push_back(static_cast<int>(entry.at("calls").at(c).number + 0.5)); }

                const auto coversAll = [&](const std::vector<int>& chosen) {
                    for (const auto& w : windows) {
                        bool hit = false;
                        for (const int lap : chosen) { hit = hit || (lap >= w.first && lap <= w.second); }
                        if (!hit) { return false; }
                    }
                    return true;
                };
                XCHECK_MSG(coversAll(calls), "every stop window must contain a pit call");

                // Minimality, by trying every smaller set (only when it is cheap to).
                if (calls.size() >= 1U && calls.size() - 1U <= 2U) {
                    bool smaller = false;
                    if (calls.size() == 1U) { smaller = windows.empty(); }
                    else if (calls.size() == 2U) { for (int a = 1; a < N && !smaller; ++a) { smaller = coversAll({a}); } }
                    else { for (int a = 1; a < N && !smaller; ++a) { for (int b = a; b < N && !smaller; ++b) { smaller = coversAll({a, b}); } } }
                    XCHECK_MSG(!smaller, "the greedy plan must use the fewest possible calls");
                }
                if (g > 0U) {
                    XCHECK_MSG(calls.size() <= static_cast<std::size_t>(greedy.at(g - 1U).at("calls").size()),
                               "wider windows never need more calls");
                }
            }

            // The queue: events counted, and the inversions against a plain pair count.
            const Json& events = run.at("events");
            XCHECK(static_cast<std::size_t>(pit.at("queue").at("events").number + 0.5) == events.size());
            long long inversions = 0;
            for (std::size_t i = 0; i < events.size(); ++i) {
                for (std::size_t j = i + 1U; j < events.size(); ++j) {
                    if (events.at(i).at("seq").number > events.at(j).at("seq").number) { ++inversions; }
                }
            }
            XCHECK(static_cast<long long>(pit.at("queue").at("inversions").number + 0.5) == inversions);
        }

        // The undercut grid against the core, pair by pair.
        const Json& u = root.at("strategy").at("undercut");
        XCHECK(u.at("delta").size() == static_cast<std::size_t>(N - 2));
        const double optimum = strategy.optimize().totalTime;
        for (int earlier = 1; earlier <= N - 2; ++earlier) {
            const Json& row = u.at("earlierTotal").at(static_cast<std::size_t>(earlier - 1));
            XCHECK(row.size() == static_cast<std::size_t>(N - 1 - earlier));
            for (int later = earlier + 1; later <= N - 1; ++later) {
                const std::size_t r = static_cast<std::size_t>(earlier - 1), c = static_cast<std::size_t>(later - earlier - 1);
                const boxbox::FirstStopComparison expected = boxbox::compareFirstStopLaps(strategy, earlier, later);
                const Json& a = u.at("earlierTotal").at(r).at(c);
                const Json& b = u.at("laterTotal").at(r).at(c);
                const Json& d = u.at("delta").at(r).at(c);
                const double ea = expected.result.earlierPlanTotalTime, eb = expected.result.laterPlanTotalTime;
                XCHECK(std::isfinite(ea) == (a.kind == Json::Kind::Number));
                XCHECK(std::isfinite(eb) == (b.kind == Json::Kind::Number));
                if (a.kind == Json::Kind::Number) {
                    XCHECK_NEAR(a.number, ea, 1.0e-7);
                    XCHECK_MSG(a.number >= optimum - boxbox::kClosureTolerance, "no plan beats the optimum");
                }
                if (b.kind == Json::Kind::Number) { XCHECK_NEAR(b.number, eb, 1.0e-7); }
                if (a.kind == Json::Kind::Number && b.kind == Json::Kind::Number) {
                    XCHECK(d.kind == Json::Kind::Number);
                    XCHECK_NEAR(d.number, b.number - a.number, 1.0e-7);   // positive: stopping earlier is faster
                } else {
                    XCHECK(d.kind == Json::Kind::Null);
                }
            }
        }
    } catch (const std::exception& error) {
        XCHECK_MSG(false, std::string("bundle is missing a field: ") + error.what());
    }

    // Too short or too long for the grid: null, and the rest of the bundle is unaffected.
    for (const int laps : {2, 101}) {
        const PitStrategy other(makeTrack(laps, 22.0, {28.4, 31.2, 30.4}), boxbox::defaultCompounds());
        std::string otherFailure;
        const std::string otherText = bundleFor(other, otherFailure);
        Json otherRoot;
        XCHECK_MSG(otherFailure.empty(), otherFailure);
        XCHECK(parseJson(otherText, otherRoot));
        if (otherRoot.kind == Json::Kind::Object) {
            XCHECK(otherRoot.at("strategy").at("undercut").kind == Json::Kind::Null);
            XCHECK(otherRoot.at("race").at("runs").at("overtakeOff").at("pit").at("greedy").size() == 11U);
        }
    }
}

void testBundleRaceRuns() {
    beginSection("Export bundle: race runs and events");

    const PitStrategy strategy(makeTrack(50, 22.0, {28.4, 31.2, 30.4}), boxbox::defaultCompounds());
    const boxbox::StrategyResult optimal = strategy.optimize();
    std::string failure;
    const std::string text = bundleFor(strategy, failure);
    Json root;
    if (!parseJson(text, root)) {
        XCHECK_MSG(false, "bundle is not valid JSON");
        return;
    }

    try {
        const Json& runs = root.at("race").at("runs");
        for (const char* name : {"overtakeOff", "overtakeOn"}) {
            const Json& run = runs.at(name);
            const std::size_t cars = run.at("drivers").size();
            XCHECK(cars == 1U + strategy.compounds().size());  // OPTIMAL + one reference per compound

            const Json& laps = run.at("laps");
            XCHECK(laps.size() == 50U);
            for (std::size_t i = 0; i < laps.size(); ++i) {
                XCHECK(laps.at(i).at("order").size() == cars);
                XCHECK(static_cast<std::size_t>(laps.at(i).at("lap").number + 0.5) == i + 1U);
                const Json& order = laps.at(i).at("order");
                for (std::size_t p = 0; p < order.size(); ++p) {
                    XCHECK(static_cast<std::size_t>(order.at(p).at("position").number + 0.5) == p + 1U);
                }
            }
            XCHECK(run.at("eventLogValid").boolean);
            XCHECK(run.at("events").size() > 0U);
            XCHECK(run.at("final").size() == cars);
        }

        // The Command Center overview: numbers the core's analytics produced.
        for (const char* name : {"overtakeOff", "overtakeOn"}) {
            const Json& run = runs.at(name);
            const Json& overview = run.at("overview");
            const std::size_t cars = run.at("drivers").size();

            // Fastest lap is the minimum over every recorded lap time.
            double minimum = boxbox::kUnreachable;
            for (std::size_t i = 0; i < run.at("laps").size(); ++i) {
                const Json& order = run.at("laps").at(i).at("order");
                for (std::size_t p = 0; p < order.size(); ++p) {
                    minimum = std::min(minimum, order.at(p).at("lapTime").number);
                }
            }
            XCHECK_NEAR(overview.at("fastestLap").at("time").number, minimum, 1.0e-8);

            // Compound laps cover every car-lap; stops agree with the pitted flags.
            double compoundTotal = 0.0;
            for (std::size_t c = 0; c < overview.at("compoundLaps").size(); ++c) {
                compoundTotal += overview.at("compoundLaps").at(c).number;
            }
            XCHECK_NEAR(compoundTotal, 50.0 * static_cast<double>(cars), 1.0e-9);

            int pitted = 0;
            for (std::size_t i = 0; i < run.at("laps").size(); ++i) {
                const Json& order = run.at("laps").at(i).at("order");
                for (std::size_t p = 0; p < order.size(); ++p) {
                    pitted += order.at(p).at("pitted").boolean ? 1 : 0;
                }
            }
            XCHECK(static_cast<int>(overview.at("totalStops").number + 0.5) == pitted);

            int bucketed = 0;
            const Json& buckets = overview.at("pitBuckets");
            for (std::size_t b = 0; b < buckets.size(); ++b) {
                bucketed += static_cast<int>(buckets.at(b).at("stops").number + 0.5);
            }
            XCHECK_MSG(bucketed == pitted, "pit buckets must account for every stop");
            XCHECK(buckets.size() == 10U);

            XCHECK(overview.at("lapTrend").size() == 50U);
            XCHECK(overview.at("drivers").size() == cars);
        }

        // Tire state after each lap: a car that pitted is on a fresh set (age 0).
        {
            const Json& laps = runs.at("overtakeOff").at("laps");
            for (std::size_t i = 0; i < laps.size(); ++i) {
                const Json& order = laps.at(i).at("order");
                for (std::size_t p = 0; p < order.size(); ++p) {
                    if (order.at(p).at("pitted").boolean) {
                        XCHECK(order.at(p).at("fittedAge").number < 0.5);
                    } else {
                        XCHECK_NEAR(order.at(p).at("fittedAge").number,
                                    order.at(p).at("age").number + 1.0, 1.0e-9);
                    }
                }
            }
        }

        // Overtake off: the optimal car's final time IS the optimizer's total (FR-8).
        const Json& finalOff = runs.at("overtakeOff").at("final");
        double optimalTime = -1.0;
        for (std::size_t i = 0; i < finalOff.size(); ++i) {
            if (finalOff.at(i).at("car").number > 0.5 && finalOff.at(i).at("car").number < 1.5) {
                optimalTime = finalOff.at(i).at("time").number;
            }
        }
        XCHECK_NEAR(optimalTime, optimal.totalTime, boxbox::kClosureTolerance);

        // The rule only ever costs time.
        const Json& finalOn = runs.at("overtakeOn").at("final");
        for (std::size_t i = 0; i < finalOn.size(); ++i) {
            XCHECK(finalOn.at(i).at("traffic").number >= 0.0);
        }
        XCHECK(runs.at("overtakeOn").at("overtake").at("enabled").boolean);
        XCHECK(!runs.at("overtakeOff").at("overtake").at("enabled").boolean);
    } catch (const std::exception& error) {
        XCHECK_MSG(false, std::string("bundle is missing a field: ") + error.what());
    }
}

/// True when segments ab and cd properly cross (touching at an end does not count).
bool segmentsCross(const boxbox::OutlinePoint& a, const boxbox::OutlinePoint& b,
                   const boxbox::OutlinePoint& c, const boxbox::OutlinePoint& d) {
    const auto side = [](const boxbox::OutlinePoint& p, const boxbox::OutlinePoint& q,
                         const boxbox::OutlinePoint& r) {
        return (q.x - p.x) * (r.y - p.y) - (q.y - p.y) * (r.x - p.x);
    };
    const double d1 = side(a, b, c), d2 = side(a, b, d), d3 = side(c, d, a), d4 = side(c, d, b);
    return ((d1 > 0.0 && d2 < 0.0) || (d1 < 0.0 && d2 > 0.0)) &&
           ((d3 > 0.0 && d4 < 0.0) || (d3 < 0.0 && d4 > 0.0));
}

void testTrackOutline() {
    beginSection("Track outline: closed, even, simple, deterministic");

    const Track track = makeTrack(50, 22.0, {28.4, 31.2, 30.4});
    const boxbox::TrackOutline outline = boxbox::makeTrackOutline(track);
    const std::vector<boxbox::OutlinePoint>& pts = outline.points;
    const std::size_t n = pts.size();

    XCHECK(n == boxbox::kOutlinePointCount);

    // Inside the drawing area, with the margin the page relies on.
    for (const boxbox::OutlinePoint& p : pts) {
        XCHECK(p.x >= 0.0 && p.x <= outline.width && p.y >= 0.0 && p.y <= outline.height);
    }

    // Equally spaced by arc length: a car moving uniformly in progress moves uniformly on screen.
    double total = 0.0;
    std::vector<double> gaps;
    for (std::size_t i = 0; i < n; ++i) {
        const boxbox::OutlinePoint& a = pts[i];
        const boxbox::OutlinePoint& b = pts[(i + 1U) % n];
        gaps.push_back(std::hypot(b.x - a.x, b.y - a.y));
        total += gaps.back();
    }
    const double mean = total / static_cast<double>(n);
    for (const double gap : gaps) {
        XCHECK_MSG(std::fabs(gap - mean) <= 0.15 * mean, "points must be evenly spaced");
    }

    // A closed loop that never crosses itself.
    bool simple = true;
    for (std::size_t i = 0; i < n && simple; ++i) {
        for (std::size_t j = i + 2U; j < n && simple; ++j) {
            if (i == 0U && j == n - 1U) { continue; }  // adjacent across the seam
            simple = !segmentsCross(pts[i], pts[(i + 1U) % n], pts[j], pts[(j + 1U) % n]);
        }
    }
    XCHECK_MSG(simple, "the outline must not cross itself");

    // Sector boundaries at each sector's share of the base lap time.
    XCHECK(outline.sectorStart.size() == 3U);
    if (outline.sectorStart.size() == 3U) {
        XCHECK(outline.sectorStart[0] == 0U);
        XCHECK(outline.sectorStart[1] == static_cast<std::size_t>(std::llround(240.0 * 28.4 / 90.0)));
        XCHECK(outline.sectorStart[2] == static_cast<std::size_t>(std::llround(240.0 * 59.6 / 90.0)));
    }

    // Pit lane: beside the line, and not on top of the track.
    XCHECK(outline.pitLane.size() == 17U);
    if (outline.pitLane.size() == 17U) {
        const boxbox::OutlinePoint& mid = outline.pitLane[8];
        const double away = std::hypot(mid.x - pts[0].x, mid.y - pts[0].y);
        XCHECK_MSG(away > 20.0 && away < 36.0, "pit lane should run about 28 units off the line");
    }
    const double lineLength = std::hypot(outline.lineB.x - outline.lineA.x,
                                         outline.lineB.y - outline.lineA.y);
    XCHECK(lineLength > 25.0 && lineLength < 40.0);

    // Same name, same shape; a different name, a different shape. Sector times
    // move the boundaries but not the drawn loop.
    const boxbox::TrackOutline again = boxbox::makeTrackOutline(track);
    bool same = again.points.size() == pts.size();
    for (std::size_t i = 0; same && i < pts.size(); ++i) {
        same = (again.points[i].x == pts[i].x) && (again.points[i].y == pts[i].y);  // exact: same bits
    }
    XCHECK_MSG(same, "an outline must be reproducible");

    Track other("Another Circuit", 50, 22.0);
    other.addSector("S1", 28.4);
    const boxbox::TrackOutline different = boxbox::makeTrackOutline(other);
    bool differs = false;
    for (std::size_t i = 0; i < pts.size() && i < different.points.size(); ++i) {
        differs = differs || std::fabs(different.points[i].x - pts[i].x) > 0.5;
    }
    XCHECK_MSG(differs, "different circuits should look different");

    // Edge cases: no sectors, and a single sector.
    XCHECK(boxbox::makeTrackOutline(makeTrack(5, 1.0, {})).sectorStart.size() == 1U);
    XCHECK(boxbox::makeTrackOutline(makeTrack(5, 1.0, {95.5})).sectorStart.size() == 1U);
}

void testBundleTimeline() {
    beginSection("Export bundle: line crossings and pit windows are consistent");

    const PitStrategy strategy(makeTrack(50, 22.0, {28.4, 31.2, 30.4}), boxbox::defaultCompounds());
    std::string failure;
    const std::string text = bundleFor(strategy, failure);
    Json root;
    if (!parseJson(text, root)) {
        XCHECK_MSG(false, "bundle is not valid JSON");
        return;
    }

    try {
        const Json& outline = root.at("track").at("outline");
        XCHECK(outline.at("points").size() == boxbox::kOutlinePointCount);
        XCHECK(outline.at("sectorStart").size() == 3U);
        XCHECK(outline.at("pitLane").size() == 17U);
        XCHECK(outline.at("startLine").size() == 2U);

        for (const char* name : {"overtakeOff", "overtakeOn"}) {
            const Json& run = root.at("race").at("runs").at(name);
            const std::size_t cars = run.at("drivers").size();
            const Json& laps = run.at("laps");

            // Per car, lap by lap: it starts where the previous lap's clock ended, runs for
            // its lap time to the line, and loses exactly the pit loss if it stopped.
            std::vector<double> previousTime(cars + 1U, 0.0);
            for (std::size_t i = 0; i < laps.size(); ++i) {
                const Json& order = laps.at(i).at("order");
                for (std::size_t p = 0; p < order.size(); ++p) {
                    const Json& e = order.at(p);
                    const std::size_t car = static_cast<std::size_t>(e.at("car").number + 0.5);
                    const double cross = e.at("cross").number;
                    const double time = e.at("time").number;

                    XCHECK_NEAR(previousTime[car] + e.at("lapTime").number, cross,
                                boxbox::kClosureTolerance);
                    if (e.at("pitted").boolean) {
                        XCHECK_NEAR(time - cross, 22.0, boxbox::kClosureTolerance);
                    } else {
                        XCHECK_NEAR(time, cross, boxbox::kClosureTolerance);
                    }
                    previousTime[car] = time;
                }
            }

            // The replay lasts until the last car finishes.
            double latest = 0.0;
            for (std::size_t i = 0; i < run.at("final").size(); ++i) {
                latest = std::max(latest, run.at("final").at(i).at("time").number);
            }
            XCHECK_NEAR(run.at("duration").number, latest, 1.0e-7);
        }
    } catch (const std::exception& error) {
        XCHECK_MSG(false, std::string("bundle is missing a field: ") + error.what());
    }
}

void testBundleDeterminism() {
    beginSection("Export bundle: byte-identical for identical inputs (NFR-5)");

    const PitStrategy strategy(makeTrack(40, 19.5, {29.1, 30.7, 31.4}), boxbox::defaultCompounds());
    std::string failureA;
    std::string failureB;
    const std::string first = bundleFor(strategy, failureA);
    const std::string second = bundleFor(strategy, failureB);

    XCHECK(failureA.empty() && failureB.empty());
    XCHECK(!first.empty());
    XCHECK_MSG(first == second, "two exports of the same race must match byte for byte");

    // A different pit loss must actually change the bundle.
    const PitStrategy other(makeTrack(40, 25.0, {29.1, 30.7, 31.4}), boxbox::defaultCompounds());
    std::string failureC;
    XCHECK(bundleFor(other, failureC) != first);
}

void testBundleEdgeCases() {
    beginSection("Export bundle: edge cases and rejected settings");

    const std::vector<TireCompound> compounds = boxbox::defaultCompounds();
    std::string failure;

    // Zero laps is not a race to export; nothing is written.
    {
        std::ostringstream out;
        const PitStrategy strategy(makeTrack(0, 22.0, {90.0}), compounds);
        XCHECK_THROWS(boxbox::writeBundleJson(out, strategy, boxbox::ExportSettings(), failure),
                      std::invalid_argument);
        XCHECK(out.str().empty());
    }

    // One lap: one stint, no stop, valid JSON.
    {
        const PitStrategy strategy(makeTrack(1, 22.0, {90.0}), compounds);
        const std::string text = bundleFor(strategy, failure);
        Json root;
        XCHECK_MSG(failure.empty(), failure);
        XCHECK(parseJson(text, root));
        if (root.kind == Json::Kind::Object) {
            XCHECK(root.at("strategy").at("optimal").at("stints").size() == 1U);
            XCHECK(root.at("strategy").at("optimal").at("pitLaps").size() == 0U);
            XCHECK(root.at("race").at("closure").at("passed").boolean);
        }
    }

    // Single-sector track, stops forced by a short max stint (the PRD hand case).
    {
        std::vector<TireCompound> hand;
        hand.emplace_back("A", 0.0, 1.0, 3);
        hand.emplace_back("B", 2.0, 0.0, 4);
        const PitStrategy strategy(makeTrack(4, 5.0, {10.0}), hand);
        const std::string text = bundleFor(strategy, failure);
        Json root;
        XCHECK_MSG(failure.empty(), failure);
        XCHECK(parseJson(text, root));
        if (root.kind == Json::Kind::Object) {
            XCHECK_NEAR(root.at("strategy").at("optimal").at("totalTime").number, 47.0,
                        boxbox::kTimeEpsilon);
            XCHECK_NEAR(root.at("strategy").at("dp").at(4).at("cost").number, 47.0,
                        boxbox::kTimeEpsilon);
        }
    }

    // A rejected setting throws before the first byte is written.
    {
        const PitStrategy strategy(makeTrack(10, 22.0, {90.0}), compounds);
        boxbox::ExportSettings badSweep;
        badSweep.sweepStep = 0.0;
        boxbox::ExportSettings badCount;
        badCount.alternatives = 0U;
        boxbox::ExportSettings badStops;
        badStops.maxStops = -1;
        boxbox::ExportSettings badRule;
        badRule.overtake.minGap = -1.0;

        for (const boxbox::ExportSettings* settings : {&badSweep, &badCount, &badStops, &badRule}) {
            std::ostringstream out;
            XCHECK_THROWS(boxbox::writeBundleJson(out, strategy, *settings, failure),
                          std::invalid_argument);
            XCHECK_MSG(out.str().empty(), "a rejected setting must not leave a partial document");
        }
    }

    // More stops requested than laps exist is clamped, not an error.
    {
        const PitStrategy strategy(makeTrack(3, 22.0, {90.0}), compounds);
        boxbox::ExportSettings many;
        many.maxStops = 50;
        const std::string text = bundleFor(strategy, failure, many);
        Json root;
        XCHECK_MSG(failure.empty(), failure);
        XCHECK(parseJson(text, root));
        if (root.kind == Json::Kind::Object) {
            XCHECK(root.at("strategy").at("stopCounts").size() == 4U);  // 0..laps
        }
    }
}

void testExportCommandArguments() {
    beginSection("export command: arguments and exit statuses");

    const PitStrategy strategy(makeTrack(10, 22.0, {90.0}), boxbox::defaultCompounds());

    const auto run = [&](const std::vector<std::string>& args, std::string& out, std::string& err) {
        std::ostringstream o;
        std::ostringstream e;
        const int status = boxbox::runExportCommand(args, strategy, o, e);
        out = o.str();
        err = e.str();
        return status;
    };

    std::string out;
    std::string err;

    XCHECK(run({"help"}, out, err) == boxbox::kExportOk);
    XCHECK(out.find("--out") != std::string::npos);
    XCHECK(err.empty());

    // Every bad argument is a usage error with a message, and writes no file.
    for (const std::vector<std::string>& args : std::vector<std::vector<std::string>>{
             {"--nonsense"},
             {"--out"},
             {"--out", ""},
             {"--max-stops", "-1"},
             {"--max-stops", "x"},
             {"--alternatives", "0"},
             {"--alternatives", "9999"},
             {"--sweep", "1:2"},
             {"--sweep", "0:10:0"},
             {"--sweep", "10:0:1"},
             {"--min-gap", "-3"},
             {"--pass-margin", "abc"}}) {
        const int status = run(args, out, err);
        XCHECK_MSG(status == boxbox::kExportUsageError, "args[0] = " + args.front());
        XCHECK_MSG(!err.empty(), "a usage error must explain itself: " + args.front());
        XCHECK(out.empty());
    }
}

}  // namespace

ExportTestTally runExportTests() {
    g_tally = ExportTestTally();

    testJsonWriter();
    testBundleStructure();
    testBundleMatchesOptimizer();
    testBundleScenarios();
    testBundleTires();
    testBundleDriverAnalytics();
    testBundlePit();
    testBundleRaceRuns();
    testTrackOutline();
    testBundleTimeline();
    testBundleDeterminism();
    testBundleEdgeCases();
    testExportCommandArguments();

    return g_tally;
}
