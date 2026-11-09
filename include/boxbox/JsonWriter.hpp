#ifndef BOXBOX_JSON_WRITER_HPP
#define BOXBOX_JSON_WRITER_HPP

#include <iosfwd>
#include <string>
#include <vector>

/// Minimal streaming JSON writer. C++17 standard library only (NFR-1).
///
/// Writes compact JSON with no whitespace, in exactly the order the calls are
/// made, so identical calls produce byte-identical output (NFR-5). Doubles are
/// written with 12 significant digits under the classic locale; a NaN or an
/// infinity is written as null, because JSON cannot represent either.
///
/// Misuse (a value with no key inside an object, a key inside an array, closing
/// the wrong kind of container, a second root value) throws std::logic_error
/// instead of producing broken JSON.
///
/// COST: O(1) per call apart from string escaping, which is O(length).
namespace boxbox {

class JsonWriter {
public:
    explicit JsonWriter(std::ostream& out);

    JsonWriter& beginObject();
    JsonWriter& endObject();
    JsonWriter& beginArray();
    JsonWriter& endArray();

    /// Object member name. Must be followed by exactly one value or container.
    JsonWriter& key(const std::string& name);

    // Distinct names rather than overloads: value(5) would be ambiguous between
    // integer, floating-point and bool conversions.
    JsonWriter& string(const std::string& text);
    JsonWriter& number(double value);
    JsonWriter& integer(long long value);
    JsonWriter& boolean(bool value);
    JsonWriter& null();

    /// True once one complete root value has been written and every container
    /// closed.
    bool complete() const noexcept { return rootDone_ && stack_.empty(); }

private:
    struct Frame {
        bool isObject = false;
        bool hasItems = false;       ///< something already written in this container
        bool awaitingValue = false;  ///< objects only: a key was written, its value was not
    };

    /// Called before every value or container: writes the separator and checks
    /// that a value is legal here.
    void beforeValue();
    void writeEscaped(const std::string& text);

    std::ostream& out_;
    std::vector<Frame> stack_;
    bool rootDone_ = false;
};

}  // namespace boxbox

#endif  // BOXBOX_JSON_WRITER_HPP
