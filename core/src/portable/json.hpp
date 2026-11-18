// Minimal, dependency-free JSON value, parser and serializer for OpenSweep Core.
//
// Design goals (security first, the parser faces untrusted pipe input):
//   * hard limits on input size and nesting depth;
//   * strict RFC 8259 grammar (no comments, no trailing commas, no NaN/Infinity,
//     no leading zeros, no BOM, no trailing garbage);
//   * strict UTF-8 validation of the whole input (overlong forms, surrogates and
//     code points above U+10FFFF are rejected); string contents are passed through
//     unchanged, \u escapes are decoded to UTF-8 (surrogate pairs combined, lone
//     surrogates rejected);
//   * duplicate object keys are rejected (avoids "which one wins" ambiguity);
//   * numbers are kept as double, plus an exact int64 when the value is an
//     integer that is exactly representable.
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace opensweep::json {

class Value;
using Array = std::vector<Value>;
using Object = std::vector<std::pair<std::string, Value>>;  // insertion order preserved

enum class Type { Null, Bool, Number, String, Array, Object };

class Value {
public:
    Value() noexcept = default;
    Value(std::nullptr_t) noexcept {}
    Value(bool b) noexcept : type_(Type::Bool), bool_(b) {}
    Value(double d) noexcept;
    Value(float f) noexcept : Value(static_cast<double>(f)) {}
    template <typename T>
        requires(std::is_integral_v<T> && !std::is_same_v<T, bool> && !std::is_same_v<T, char>)
    Value(T v) noexcept {
        type_ = Type::Number;
        if constexpr (std::is_signed_v<T>) {
            set_int(static_cast<std::int64_t>(v));
        } else {
            if (static_cast<std::uint64_t>(v) <=
                static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                set_int(static_cast<std::int64_t>(v));
            } else {
                num_ = static_cast<double>(v);
                is_int_ = false;
            }
        }
    }
    Value(const char* s) : type_(Type::String), str_(s ? s : "") {}
    Value(std::string s) noexcept : type_(Type::String), str_(std::move(s)) {}
    Value(std::string_view s) : type_(Type::String), str_(s) {}
    Value(Array a) noexcept : type_(Type::Array), arr_(std::move(a)) {}
    Value(Object o) noexcept : type_(Type::Object), obj_(std::move(o)) {}

    static Value array() { return Value(Array{}); }
    static Value object() { return Value(Object{}); }

    Type type() const noexcept { return type_; }
    bool is_null() const noexcept { return type_ == Type::Null; }
    bool is_bool() const noexcept { return type_ == Type::Bool; }
    bool is_number() const noexcept { return type_ == Type::Number; }
    bool is_string() const noexcept { return type_ == Type::String; }
    bool is_array() const noexcept { return type_ == Type::Array; }
    bool is_object() const noexcept { return type_ == Type::Object; }
    // True when the number is an integer exactly representable as int64.
    bool is_int() const noexcept { return type_ == Type::Number && is_int_; }

    // Accessors return a safe default when the type does not match.
    bool as_bool(bool def = false) const noexcept { return is_bool() ? bool_ : def; }
    double as_double(double def = 0.0) const noexcept { return is_number() ? num_ : def; }
    std::optional<std::int64_t> as_int() const noexcept {
        if (is_int()) return int_;
        return std::nullopt;
    }
    const std::string& as_string() const noexcept;
    const Array& as_array() const noexcept;
    const Object& as_object() const noexcept;
    Array& as_array_mut();    // converts to an empty array if not an array
    Object& as_object_mut();  // converts to an empty object if not an object

    // Object helpers. find() returns nullptr when absent or when this is not an object.
    const Value* find(std::string_view key) const noexcept;
    // Sets (or replaces) a key. Converts this value to an object if needed.
    Value& set(std::string_view key, Value v);
    // Appends to an array. Converts this value to an array if needed.
    Value& push_back(Value v);
    std::size_t size() const noexcept;

    friend bool operator==(const Value& a, const Value& b) noexcept;

private:
    void set_int(std::int64_t v) noexcept {
        int_ = v;
        is_int_ = true;
        num_ = static_cast<double>(v);
    }
    friend class Parser;

    Type type_ = Type::Null;
    bool bool_ = false;
    bool is_int_ = false;
    std::int64_t int_ = 0;
    double num_ = 0.0;
    std::string str_;
    Array arr_;
    Object obj_;
};

struct ParseOptions {
    std::size_t max_depth = 32;          // arrays/objects nested deeper than this are rejected
    std::size_t max_size = 1024 * 1024;  // inputs longer than this (bytes) are rejected
};

struct ParseResult {
    std::optional<Value> value;  // engaged on success
    std::string error;           // human readable, never contains input bytes
    std::size_t offset = 0;      // byte offset of the error
    bool ok() const noexcept { return value.has_value(); }
};

ParseResult parse(std::string_view text, const ParseOptions& opts = {});

// Serializes compactly (no whitespace). Non-finite doubles become null. Invalid UTF-8
// inside strings is replaced by U+FFFD so the output is always valid UTF-8 JSON.
std::string serialize(const Value& v);
void serialize_to(const Value& v, std::string& out);

// Returns true if `s` is entirely valid UTF-8 (strict: no overlongs/surrogates).
bool is_valid_utf8(std::string_view s) noexcept;

}  // namespace opensweep::json
