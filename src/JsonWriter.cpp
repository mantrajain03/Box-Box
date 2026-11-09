#include "boxbox/JsonWriter.hpp"

#include <cmath>
#include <iomanip>
#include <locale>
#include <ostream>
#include <sstream>
#include <stdexcept>

namespace boxbox {

namespace {

constexpr int kSignificantDigits = 12;

}  // namespace

JsonWriter::JsonWriter(std::ostream& out) : out_(out) {}

void JsonWriter::beforeValue() {
    if (stack_.empty()) {
        if (rootDone_) {
            throw std::logic_error("JsonWriter: a second root value");
        }
        rootDone_ = true;
        return;
    }

    Frame& top = stack_.back();
    if (top.isObject) {
        if (!top.awaitingValue) {
            throw std::logic_error("JsonWriter: a value inside an object needs a key first");
        }
        top.awaitingValue = false;
    } else {
        if (top.hasItems) {
            out_ << ',';
        }
        top.hasItems = true;
    }
}

JsonWriter& JsonWriter::beginObject() {
    beforeValue();
    out_ << '{';
    Frame frame;
    frame.isObject = true;
    stack_.push_back(frame);
    return *this;
}

JsonWriter& JsonWriter::endObject() {
    if (stack_.empty() || !stack_.back().isObject || stack_.back().awaitingValue) {
        throw std::logic_error("JsonWriter: endObject with no open object or a dangling key");
    }
    stack_.pop_back();
    out_ << '}';
    return *this;
}

JsonWriter& JsonWriter::beginArray() {
    beforeValue();
    out_ << '[';
    stack_.push_back(Frame{});
    return *this;
}

JsonWriter& JsonWriter::endArray() {
    if (stack_.empty() || stack_.back().isObject) {
        throw std::logic_error("JsonWriter: endArray with no open array");
    }
    stack_.pop_back();
    out_ << ']';
    return *this;
}

JsonWriter& JsonWriter::key(const std::string& name) {
    if (stack_.empty() || !stack_.back().isObject || stack_.back().awaitingValue) {
        throw std::logic_error("JsonWriter: a key is only legal directly inside an object");
    }
    Frame& top = stack_.back();
    if (top.hasItems) {
        out_ << ',';
    }
    top.hasItems = true;
    top.awaitingValue = true;
    writeEscaped(name);
    out_ << ':';
    return *this;
}

JsonWriter& JsonWriter::string(const std::string& text) {
    beforeValue();
    writeEscaped(text);
    return *this;
}

JsonWriter& JsonWriter::number(double value) {
    beforeValue();
    if (!std::isfinite(value)) {
        out_ << "null";
        return *this;
    }

    std::ostringstream text;
    text.imbue(std::locale::classic());
    text << std::setprecision(kSignificantDigits) << value;

    // A tiny negative rounds to "-0", which is legal JSON but pointless; and
    // string equality is not a floating-point comparison.
    std::string rendered = text.str();
    if (rendered == "-0") {
        rendered = "0";
    }
    out_ << rendered;
    return *this;
}

JsonWriter& JsonWriter::integer(long long value) {
    beforeValue();
    out_ << value;
    return *this;
}

JsonWriter& JsonWriter::boolean(bool value) {
    beforeValue();
    out_ << (value ? "true" : "false");
    return *this;
}

JsonWriter& JsonWriter::null() {
    beforeValue();
    out_ << "null";
    return *this;
}

void JsonWriter::writeEscaped(const std::string& text) {
    static const char* const kHex = "0123456789abcdef";

    out_ << '"';
    for (const char raw : text) {
        const unsigned char ch = static_cast<unsigned char>(raw);
        switch (ch) {
            case '"': out_ << "\\\""; break;
            case '\\': out_ << "\\\\"; break;
            case '\n': out_ << "\\n"; break;
            case '\r': out_ << "\\r"; break;
            case '\t': out_ << "\\t"; break;
            default:
                if (ch < 0x20U) {
                    out_ << "\\u00" << kHex[ch >> 4U] << kHex[ch & 0x0FU];
                } else {
                    out_ << raw;  // UTF-8 bytes pass through unchanged
                }
        }
    }
    out_ << '"';
}

}  // namespace boxbox
