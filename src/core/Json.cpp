#include "core/Json.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace monosect::core {
namespace {

class Parser {
public:
    Parser(std::string_view text, bool singleLine) : text_(text), singleLine_(singleLine) {}

    Result<JsonValue> parseDocument() {
        skipWhitespace();
        auto value = parseValue(0);
        if (!value.has_value()) {
            return value;
        }
        skipWhitespace();
        if (position_ != text_.size() && !singleLine_) {
            return fail("Unexpected characters after the JSON document");
        }
        return value;
    }

    std::size_t position() const noexcept { return position_; }

private:
    static constexpr int kMaxDepth = 64;

    Error fail(std::string message) const {
        // Report line and column: ffprobe output has a single line, job files do not.
        std::size_t line = 1;
        std::size_t column = 1;
        for (std::size_t i = 0; i < std::min(position_, text_.size()); ++i) {
            if (text_[i] == '\n') {
                ++line;
                column = 1;
            } else {
                ++column;
            }
        }
        std::string snippet;
        const std::size_t snippetStart = position_ > 24 ? position_ - 24 : 0;
        const std::size_t snippetEnd = std::min(text_.size(), position_ + 24);
        snippet = std::string(text_.substr(snippetStart, snippetEnd - snippetStart));
        std::string detail = "line " + std::to_string(line) + ", column " + std::to_string(column);
        if (!snippet.empty()) {
            detail += ", near: " + snippet;
        }
        return Error{ErrorCode::ProbeFailed, std::move(message), std::move(detail),
                     "This is a bug in MonoSect or an unexpected tool version."};
    }

    void skipWhitespace() {
        while (position_ < text_.size()) {
            const char c = text_[position_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++position_;
            } else {
                break;
            }
        }
    }

    bool consume(char expected) {
        if (position_ < text_.size() && text_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    Result<JsonValue> parseValue(int depth) {
        if (depth > kMaxDepth) {
            return fail("The JSON document is nested too deeply");
        }
        skipWhitespace();
        if (position_ >= text_.size()) {
            return fail("Unexpected end of JSON input");
        }
        const char c = text_[position_];
        switch (c) {
            case '{': return parseObject(depth);
            case '[': return parseArray(depth);
            case '"': {
                auto text = parseString();
                if (!text.has_value()) {
                    return failure<JsonValue>(text.error());
                }
                return JsonValue::makeString(std::move(text.value()));
            }
            case 't':
                if (text_.substr(position_, 4) == "true") {
                    position_ += 4;
                    return JsonValue::makeBool(true);
                }
                return fail("Expected 'true'");
            case 'f':
                if (text_.substr(position_, 5) == "false") {
                    position_ += 5;
                    return JsonValue::makeBool(false);
                }
                return fail("Expected 'false'");
            case 'n':
                if (text_.substr(position_, 4) == "null") {
                    position_ += 4;
                    return JsonValue::makeNull();
                }
                return fail("Expected 'null'");
            default:
                return parseNumber();
        }
    }

    Result<JsonValue> parseObject(int depth) {
        ++position_;  // '{'
        JsonValue object = JsonValue::makeObject();
        skipWhitespace();
        if (consume('}')) {
            return object;
        }
        while (true) {
            skipWhitespace();
            if (position_ >= text_.size() || text_[position_] != '"') {
                return fail("Expected a member name inside an object");
            }
            auto key = parseString();
            if (!key.has_value()) {
                return failure<JsonValue>(key.error());
            }
            skipWhitespace();
            if (!consume(':')) {
                return fail("Expected ':' after a member name");
            }
            auto value = parseValue(depth + 1);
            if (!value.has_value()) {
                return value;
            }
            object.set(std::move(key.value()), std::move(value.value()));
            skipWhitespace();
            if (consume(',')) {
                continue;
            }
            if (consume('}')) {
                return object;
            }
            return fail("Expected ',' or '}' inside an object");
        }
    }

    Result<JsonValue> parseArray(int depth) {
        ++position_;  // '['
        JsonValue array = JsonValue::makeArray();
        skipWhitespace();
        if (consume(']')) {
            return array;
        }
        while (true) {
            auto value = parseValue(depth + 1);
            if (!value.has_value()) {
                return value;
            }
            array.push(std::move(value.value()));
            skipWhitespace();
            if (consume(',')) {
                continue;
            }
            if (consume(']')) {
                return array;
            }
            return fail("Expected ',' or ']' inside an array");
        }
    }

    Result<std::string> parseString() {
        ++position_;  // opening quote
        std::string out;
        while (position_ < text_.size()) {
            const char c = text_[position_++];
            if (c == '"') {
                return out;
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (position_ >= text_.size()) {
                return fail("A string ends in the middle of an escape sequence");
            }
            const char escape = text_[position_++];
            switch (escape) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    if (position_ + 4 > text_.size()) {
                        return fail("A \\u escape is truncated");
                    }
                    unsigned code = 0;
                    for (int i = 0; i < 4; ++i) {
                        const char hex = text_[position_ + static_cast<std::size_t>(i)];
                        code <<= 4;
                        if (hex >= '0' && hex <= '9') {
                            code |= static_cast<unsigned>(hex - '0');
                        } else if (hex >= 'a' && hex <= 'f') {
                            code |= static_cast<unsigned>(hex - 'a' + 10);
                        } else if (hex >= 'A' && hex <= 'F') {
                            code |= static_cast<unsigned>(hex - 'A' + 10);
                        } else {
                            return fail("A \\u escape contains a non-hexadecimal character");
                        }
                    }
                    position_ += 4;
                    out += utf8FromCodePoint(code);
                    break;
                }
                default:
                    return fail(std::string("Unknown escape sequence \\") + escape);
            }
        }
        return fail("A string is not terminated");
    }

    static std::string utf8FromCodePoint(unsigned code) {
        std::string out;
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
        return out;
    }

    Result<JsonValue> parseNumber() {
        const std::size_t start = position_;
        if (position_ < text_.size() && (text_[position_] == '-' || text_[position_] == '+')) {
            ++position_;
        }
        bool sawDigit = false;
        bool sawDot = false;
        bool sawExponent = false;
        while (position_ < text_.size()) {
            const char c = text_[position_];
            if (c >= '0' && c <= '9') {
                sawDigit = true;
                ++position_;
            } else if (c == '.' && !sawDot && !sawExponent) {
                sawDot = true;
                ++position_;
            } else if ((c == 'e' || c == 'E') && !sawExponent && sawDigit) {
                sawExponent = true;
                ++position_;
                if (position_ < text_.size() && (text_[position_] == '+' || text_[position_] == '-')) {
                    ++position_;
                }
            } else {
                break;
            }
        }
        if (!sawDigit) {
            return fail(std::string("Unexpected character '") + text_[start] + "'");
        }
        const std::string text(text_.substr(start, position_ - start));
        if (!sawDot && !sawExponent) {
            errno = 0;
            char* end = nullptr;
            const long long value = std::strtoll(text.c_str(), &end, 10);
            if (errno == 0 && end != nullptr && *end == '\0') {
                return JsonValue::makeInt(static_cast<std::int64_t>(value));
            }
        }
        return JsonValue::makeDouble(std::strtod(text.c_str(), nullptr));
    }

    std::string_view text_;
    std::size_t position_ = 0;
    bool singleLine_ = false;
};

std::string numberToString(double value) {
    char buffer[40];
    if (value == std::floor(value) && std::fabs(value) < 9.0e15) {
        std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(value));
        return buffer;
    }
    std::snprintf(buffer, sizeof(buffer), "%.10g", value);
    return buffer;
}

void dumpInto(const JsonValue& value, std::string& out, int indent, int depth) {
    const std::string pad(static_cast<std::size_t>(indent) * static_cast<std::size_t>(depth), ' ');
    const std::string padInner(static_cast<std::size_t>(indent) * static_cast<std::size_t>(depth + 1), ' ');
    const char* newline = indent > 0 ? "\n" : "";
    switch (value.type()) {
        case JsonValue::Type::Null:
            out += "null";
            break;
        case JsonValue::Type::Bool: {
            const auto flag = value.asBool();
            out += (flag.has_value() && *flag) ? "true" : "false";
            break;
        }
        case JsonValue::Type::Number: {
            const auto integer = value.asInt64();
            const auto floating = value.asDouble();
            if (integer.has_value()) {
                out += std::to_string(*integer);
            } else if (floating.has_value()) {
                out += numberToString(*floating);
            } else {
                out += "0";
            }
            break;
        }
        case JsonValue::Type::String: {
            const auto text = value.asString();
            out += '"';
            out += jsonEscape(text.has_value() ? *text : std::string());
            out += '"';
            break;
        }
        case JsonValue::Type::Array: {
            if (value.items().empty()) {
                out += "[]";
                break;
            }
            out += '[';
            out += newline;
            bool first = true;
            for (const auto& item : value.items()) {
                if (!first) {
                    out += ',';
                    out += newline;
                }
                first = false;
                out += padInner;
                dumpInto(item, out, indent, depth + 1);
            }
            out += newline;
            out += pad;
            out += ']';
            break;
        }
        case JsonValue::Type::Object: {
            if (value.members().empty()) {
                out += "{}";
                break;
            }
            out += '{';
            out += newline;
            bool first = true;
            for (const auto& member : value.members()) {
                if (!first) {
                    out += ',';
                    out += newline;
                }
                first = false;
                out += padInner;
                out += '"';
                out += jsonEscape(member.first);
                out += "\":";
                if (indent > 0) {
                    out += ' ';
                }
                dumpInto(member.second, out, indent, depth + 1);
            }
            out += newline;
            out += pad;
            out += '}';
            break;
        }
    }
}

} // namespace

JsonValue JsonValue::makeBool(bool value) {
    JsonValue out;
    out.type_ = Type::Bool;
    out.bool_ = value;
    return out;
}

JsonValue JsonValue::makeInt(std::int64_t value) {
    JsonValue out;
    out.type_ = Type::Number;
    out.numberIsInteger_ = true;
    out.integer_ = value;
    out.double_ = static_cast<double>(value);
    return out;
}

JsonValue JsonValue::makeDouble(double value) {
    JsonValue out;
    out.type_ = Type::Number;
    out.numberIsInteger_ = false;
    out.double_ = value;
    out.integer_ = static_cast<std::int64_t>(value);
    return out;
}

JsonValue JsonValue::makeString(std::string value) {
    JsonValue out;
    out.type_ = Type::String;
    out.string_ = std::move(value);
    return out;
}

JsonValue JsonValue::makeArray() {
    JsonValue out;
    out.type_ = Type::Array;
    return out;
}

JsonValue JsonValue::makeObject() {
    JsonValue out;
    out.type_ = Type::Object;
    return out;
}

std::size_t JsonValue::size() const noexcept {
    switch (type_) {
        case Type::Array: return array_.size();
        case Type::Object: return object_.size();
        case Type::String: return string_.size();
        default: return 0;
    }
}

const JsonValue* JsonValue::find(std::string_view key) const noexcept {
    if (type_ != Type::Object) {
        return nullptr;
    }
    for (const auto& member : object_) {
        if (member.first == key) {
            return &member.second;
        }
    }
    return nullptr;
}

const JsonValue* JsonValue::at(std::size_t index) const noexcept {
    if (type_ != Type::Array || index >= array_.size()) {
        return nullptr;
    }
    return &array_[index];
}

std::optional<std::string> JsonValue::asString() const {
    if (type_ == Type::String) {
        return string_;
    }
    return std::nullopt;
}

std::optional<bool> JsonValue::asBool() const {
    if (type_ == Type::Bool) {
        return bool_;
    }
    if (type_ == Type::Number) {
        return numberIsInteger_ ? integer_ != 0 : double_ != 0.0;
    }
    return std::nullopt;
}

std::optional<std::int64_t> JsonValue::asInt64() const {
    if (type_ == Type::Number) {
        if (numberIsInteger_) {
            return integer_;
        }
        // ffprobe occasionally prints 2.0 for an integer field.
        if (double_ == std::floor(double_) && std::fabs(double_) < 9.0e18) {
            return static_cast<std::int64_t>(double_);
        }
        return std::nullopt;
    }
    if (type_ == Type::String) {
        const std::string& text = string_;
        if (text.empty() || text == "N/A") {
            return std::nullopt;
        }
        errno = 0;
        char* end = nullptr;
        const long long value = std::strtoll(text.c_str(), &end, 10);
        if (errno == 0 && end != nullptr && *end == '\0') {
            return static_cast<std::int64_t>(value);
        }
        // A decimal string such as "29.97" is not an integer, but callers that
        // want it use asDouble().
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<double> JsonValue::asDouble() const {
    if (type_ == Type::Number) {
        return numberIsInteger_ ? static_cast<double>(integer_) : double_;
    }
    if (type_ == Type::String) {
        const std::string& text = string_;
        if (text.empty() || text == "N/A") {
            return std::nullopt;
        }
        errno = 0;
        char* end = nullptr;
        const double value = std::strtod(text.c_str(), &end);
        if (errno == 0 && end != nullptr && *end == '\0') {
            return value;
        }
        return std::nullopt;
    }
    return std::nullopt;
}

std::string JsonValue::stringOr(std::string_view key, std::string fallback) const {
    if (const JsonValue* member = find(key); member != nullptr) {
        if (auto text = member->asString(); text.has_value()) {
            return *text;
        }
    }
    return fallback;
}

void JsonValue::push(JsonValue value) {
    if (type_ != Type::Array) {
        *this = JsonValue::makeArray();
    }
    array_.push_back(std::move(value));
}

void JsonValue::set(std::string key, JsonValue value) {
    if (type_ != Type::Object) {
        *this = JsonValue::makeObject();
    }
    for (auto& member : object_) {
        if (member.first == key) {
            member.second = std::move(value);
            return;
        }
    }
    object_.emplace_back(std::move(key), std::move(value));
}

std::string JsonValue::dump(int indent) const {
    std::string out;
    dumpInto(*this, out, indent, 0);
    return out;
}

Result<JsonValue> parseJson(std::string_view text) {
    Parser parser(text, false);
    return parser.parseDocument();
}

Result<std::vector<JsonValue>> parseJsonLines(std::string_view text) {
    std::vector<JsonValue> lines;
    std::size_t start = 0;
    std::size_t lineNumber = 0;
    while (start <= text.size()) {
        ++lineNumber;
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        std::string_view line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        // Skip blank lines and indentation-only lines.
        const std::size_t firstNonSpace = line.find_first_not_of(" \t");
        if (firstNonSpace != std::string_view::npos) {
            line.remove_prefix(firstNonSpace);
            Parser parser(line, true);
            auto value = parser.parseDocument();
            if (!value.has_value()) {
                return Error{ErrorCode::DatabaseError,
                             "A line of the job database is not valid JSON.",
                             "line " + std::to_string(lineNumber) + ": " + value.error().detail,
                             "MonoSect will rebuild the job list from the parts it can read."};
            }
            if (!value.value().isObject()) {
                return Error{ErrorCode::DatabaseError,
                             "A line of the job database is not a JSON object.",
                             "line " + std::to_string(lineNumber),
                             "MonoSect will rebuild the job list from the parts it can read."};
            }
            lines.push_back(std::move(value.value()));
        }
        if (end == text.size()) {
            break;
        }
        start = end + 1;
    }
    return lines;
}

std::string jsonEscape(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned>(c));
                    out += buffer;
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

} // namespace monosect::core
