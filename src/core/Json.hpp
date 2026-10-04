// MonoSect — a small, strict JSON reader/writer.
//
// Why not a library: MonoSect ships as a single Windows executable with no
// runtime dependencies, and the only JSON it ever touches is (a) the output of
// ffprobe, (b) its own job database.  A ~700 line parser that is fully auditable
// and has deterministic error messages is a better fit than a dependency - and
// this parser is used by the unit tests that check the ffprobe field handling.
//
// Notable behaviours that the rest of the code relies on:
//  * Numbers that ffprobe prints as strings ("N/A", "12345") are handled by the
//    numeric getters, which return nullopt instead of guessing.
//  * Objects keep their original key order so that a round trip through the job
//    database produces the same file.
#pragma once

#include "core/Result.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace monosect::core {

class JsonValue {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    JsonValue() = default;
    static JsonValue makeNull() { return JsonValue{}; }
    static JsonValue makeBool(bool value);
    static JsonValue makeInt(std::int64_t value);
    static JsonValue makeDouble(double value);
    static JsonValue makeString(std::string value);
    static JsonValue makeArray();
    static JsonValue makeObject();

    [[nodiscard]] Type type() const noexcept { return type_; }
    [[nodiscard]] bool isNull() const noexcept { return type_ == Type::Null; }
    [[nodiscard]] bool isBool() const noexcept { return type_ == Type::Bool; }
    [[nodiscard]] bool isNumber() const noexcept { return type_ == Type::Number; }
    [[nodiscard]] bool isString() const noexcept { return type_ == Type::String; }
    [[nodiscard]] bool isArray() const noexcept { return type_ == Type::Array; }
    [[nodiscard]] bool isObject() const noexcept { return type_ == Type::Object; }

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] const std::vector<JsonValue>& items() const noexcept { return array_; }
    [[nodiscard]] const std::vector<std::pair<std::string, JsonValue>>& members() const noexcept {
        return object_;
    }

    /// Object member, or nullptr when this is not an object / the key is missing.
    [[nodiscard]] const JsonValue* find(std::string_view key) const noexcept;
    /// Array element, or nullptr when out of range.
    [[nodiscard]] const JsonValue* at(std::size_t index) const noexcept;

    [[nodiscard]] std::optional<std::string> asString() const;
    [[nodiscard]] std::optional<bool> asBool() const;
    [[nodiscard]] std::optional<std::int64_t> asInt64() const;
    [[nodiscard]] std::optional<double> asDouble() const;

    /// Convenience: string member, or the fallback when missing or not a string.
    [[nodiscard]] std::string stringOr(std::string_view key, std::string fallback = {}) const;

    void push(JsonValue value);
    void set(std::string key, JsonValue value);

    /// Serialises to compact JSON.  `indent` > 0 pretty-prints.
    [[nodiscard]] std::string dump(int indent = 0) const;

private:
    Type type_ = Type::Null;
    bool bool_ = false;
    bool numberIsInteger_ = true;
    std::int64_t integer_ = 0;
    double double_ = 0.0;
    std::string string_;
    std::vector<JsonValue> array_;
    std::vector<std::pair<std::string, JsonValue>> object_;
};

/// Parses one complete JSON document.  Trailing whitespace is allowed, anything
/// else after the document is an error.
[[nodiscard]] Result<JsonValue> parseJson(std::string_view text);

/// Reads newline-delimited JSON objects (the job database format).  Blank lines
/// are skipped; a malformed line is reported with its line number.
[[nodiscard]] Result<std::vector<JsonValue>> parseJsonLines(std::string_view text);

/// Escapes a string for inclusion in JSON output.
[[nodiscard]] std::string jsonEscape(std::string_view text);

} // namespace monosect::core
