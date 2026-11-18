#include "json.hpp"

#include <charconv>
#include <cmath>
#include <cstring>
#include <unordered_set>

namespace opensweep::json {

namespace {

const std::string kEmptyString;
const Array kEmptyArray;
const Object kEmptyObject;

// 2^63 as a double; doubles in [-2^63, 2^63) convert to int64 without overflow.
constexpr double kTwo63 = 9223372036854775808.0;

bool double_to_exact_int(double d, std::int64_t& out) noexcept {
    if (!std::isfinite(d)) return false;
    if (d < -kTwo63 || d >= kTwo63) return false;
    if (std::trunc(d) != d) return false;
    out = static_cast<std::int64_t>(d);
    return true;
}

// Decodes one UTF-8 sequence starting at s[i]. Returns its length (1..4) or 0 if invalid.
std::size_t utf8_seq_len(std::string_view s, std::size_t i) noexcept {
    const auto b0 = static_cast<unsigned char>(s[i]);
    if (b0 < 0x80) return 1;
    std::size_t len;
    std::uint32_t cp;
    if (b0 >= 0xC2 && b0 <= 0xDF) {
        len = 2;
        cp = b0 & 0x1Fu;
    } else if (b0 >= 0xE0 && b0 <= 0xEF) {
        len = 3;
        cp = b0 & 0x0Fu;
    } else if (b0 >= 0xF0 && b0 <= 0xF4) {
        len = 4;
        cp = b0 & 0x07u;
    } else {
        return 0;  // continuation byte, overlong 2-byte lead (C0/C1) or > F4
    }
    if (i + len > s.size()) return 0;
    for (std::size_t k = 1; k < len; ++k) {
        const auto b = static_cast<unsigned char>(s[i + k]);
        if ((b & 0xC0u) != 0x80u) return 0;
        cp = (cp << 6) | (b & 0x3Fu);
    }
    if (len == 3 && cp < 0x800) return 0;                    // overlong
    if (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) return 0;  // overlong / out of range
    if (cp >= 0xD800 && cp <= 0xDFFF) return 0;              // UTF-16 surrogates
    return len;
}

void append_utf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

int hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

// ---------------------------------------------------------------------------
// Value

Value::Value(double d) noexcept : type_(Type::Number), num_(d) {
    std::int64_t i = 0;
    if (double_to_exact_int(d, i)) {
        int_ = i;
        is_int_ = true;
    }
}

const std::string& Value::as_string() const noexcept { return is_string() ? str_ : kEmptyString; }
const Array& Value::as_array() const noexcept { return is_array() ? arr_ : kEmptyArray; }
const Object& Value::as_object() const noexcept { return is_object() ? obj_ : kEmptyObject; }

Array& Value::as_array_mut() {
    if (!is_array()) *this = Value(Array{});
    return arr_;
}

Object& Value::as_object_mut() {
    if (!is_object()) *this = Value(Object{});
    return obj_;
}

const Value* Value::find(std::string_view key) const noexcept {
    if (!is_object()) return nullptr;
    for (const auto& kv : obj_) {
        if (kv.first == key) return &kv.second;
    }
    return nullptr;
}

Value& Value::set(std::string_view key, Value v) {
    auto& o = as_object_mut();
    for (auto& kv : o) {
        if (kv.first == key) {
            kv.second = std::move(v);
            return *this;
        }
    }
    o.emplace_back(std::string(key), std::move(v));
    return *this;
}

Value& Value::push_back(Value v) {
    as_array_mut().push_back(std::move(v));
    return *this;
}

std::size_t Value::size() const noexcept {
    if (is_array()) return arr_.size();
    if (is_object()) return obj_.size();
    return 0;
}

bool operator==(const Value& a, const Value& b) noexcept {
    if (a.type_ != b.type_) return false;
    switch (a.type_) {
        case Type::Null: return true;
        case Type::Bool: return a.bool_ == b.bool_;
        case Type::Number:
            if (a.is_int_ && b.is_int_) return a.int_ == b.int_;
            return a.num_ == b.num_;
        case Type::String: return a.str_ == b.str_;
        case Type::Array: return a.arr_ == b.arr_;
        case Type::Object: return a.obj_ == b.obj_;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Parser

class Parser {
public:
    Parser(std::string_view text, const ParseOptions& opts) : s_(text), opts_(opts) {}

    ParseResult run() {
        ParseResult r;
        if (s_.size() > opts_.max_size) {
            r.error = "input exceeds size limit";
            return r;
        }
        if (!is_valid_utf8(s_)) {
            r.error = "input is not valid UTF-8";
            return r;
        }
        Value v;
        skip_ws();
        if (!parse_value(v, 0)) {
            r.error = err_;
            r.offset = pos_;
            return r;
        }
        skip_ws();
        if (pos_ != s_.size()) {
            r.error = "trailing characters after JSON value";
            r.offset = pos_;
            return r;
        }
        r.value = std::move(v);
        return r;
    }

private:
    bool fail(const char* msg) {
        err_ = msg;
        return false;
    }

    bool at_end() const noexcept { return pos_ >= s_.size(); }
    char peek() const noexcept { return at_end() ? '\0' : s_[pos_]; }

    void skip_ws() noexcept {
        while (!at_end()) {
            const char c = s_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                ++pos_;
            else
                break;
        }
    }

    bool consume_literal(std::string_view lit) {
        if (s_.substr(pos_, lit.size()) != lit) return fail("invalid literal");
        pos_ += lit.size();
        return true;
    }

    bool parse_value(Value& out, std::size_t depth) {
        if (at_end()) return fail("unexpected end of input");
        switch (peek()) {
            case '{': return parse_object(out, depth + 1);
            case '[': return parse_array(out, depth + 1);
            case '"': {
                std::string str;
                if (!parse_string(str)) return false;
                out = Value(std::move(str));
                return true;
            }
            case 't':
                if (!consume_literal("true")) return false;
                out = Value(true);
                return true;
            case 'f':
                if (!consume_literal("false")) return false;
                out = Value(false);
                return true;
            case 'n':
                if (!consume_literal("null")) return false;
                out = Value(nullptr);
                return true;
            default:
                if (peek() == '-' || (peek() >= '0' && peek() <= '9')) return parse_number(out);
                return fail("unexpected character");
        }
    }

    bool parse_object(Value& out, std::size_t depth) {
        if (depth > opts_.max_depth) return fail("nesting too deep");
        ++pos_;  // '{'
        Object obj;
        std::unordered_set<std::string> seen;
        skip_ws();
        if (peek() == '}') {
            ++pos_;
            out = Value(std::move(obj));
            return true;
        }
        for (;;) {
            skip_ws();
            if (peek() != '"') return fail("expected string key");
            std::string key;
            if (!parse_string(key)) return false;
            if (!seen.insert(key).second) return fail("duplicate object key");
            skip_ws();
            if (peek() != ':') return fail("expected ':'");
            ++pos_;
            skip_ws();
            Value v;
            if (!parse_value(v, depth)) return false;
            obj.emplace_back(std::move(key), std::move(v));
            skip_ws();
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            if (peek() == '}') {
                ++pos_;
                out = Value(std::move(obj));
                return true;
            }
            return fail("expected ',' or '}'");
        }
    }

    bool parse_array(Value& out, std::size_t depth) {
        if (depth > opts_.max_depth) return fail("nesting too deep");
        ++pos_;  // '['
        Array arr;
        skip_ws();
        if (peek() == ']') {
            ++pos_;
            out = Value(std::move(arr));
            return true;
        }
        for (;;) {
            skip_ws();
            Value v;
            if (!parse_value(v, depth)) return false;
            arr.push_back(std::move(v));
            skip_ws();
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            if (peek() == ']') {
                ++pos_;
                out = Value(std::move(arr));
                return true;
            }
            return fail("expected ',' or ']'");
        }
    }

    bool parse_hex4(std::uint32_t& out) {
        if (pos_ + 4 > s_.size()) return fail("truncated \\u escape");
        std::uint32_t v = 0;
        for (int k = 0; k < 4; ++k) {
            const int h = hex_value(s_[pos_ + static_cast<std::size_t>(k)]);
            if (h < 0) return fail("invalid \\u escape");
            v = (v << 4) | static_cast<std::uint32_t>(h);
        }
        pos_ += 4;
        out = v;
        return true;
    }

    bool parse_string(std::string& out) {
        ++pos_;  // opening quote
        for (;;) {
            if (at_end()) return fail("unterminated string");
            const auto c = static_cast<unsigned char>(s_[pos_]);
            if (c == '"') {
                ++pos_;
                return true;
            }
            if (c < 0x20) return fail("control character in string");
            if (c != '\\') {
                // Input was validated as UTF-8 up front; copy the run of plain bytes.
                const std::size_t start = pos_;
                while (pos_ < s_.size()) {
                    const auto d = static_cast<unsigned char>(s_[pos_]);
                    if (d == '"' || d == '\\' || d < 0x20) break;
                    ++pos_;
                }
                out.append(s_.data() + start, pos_ - start);
                continue;
            }
            ++pos_;  // backslash
            if (at_end()) return fail("unterminated escape");
            const char e = s_[pos_++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    std::uint32_t cp = 0;
                    if (!parse_hex4(cp)) return false;
                    if (cp >= 0xDC00 && cp <= 0xDFFF) return fail("lone low surrogate");
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (pos_ + 2 > s_.size() || s_[pos_] != '\\' || s_[pos_ + 1] != 'u')
                            return fail("lone high surrogate");
                        pos_ += 2;
                        std::uint32_t lo = 0;
                        if (!parse_hex4(lo)) return false;
                        if (lo < 0xDC00 || lo > 0xDFFF) return fail("invalid surrogate pair");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    append_utf8(out, cp);
                    break;
                }
                default: return fail("invalid escape");
            }
        }
    }

    bool parse_number(Value& out) {
        const std::size_t start = pos_;
        bool integral = true;
        if (peek() == '-') ++pos_;
        if (at_end()) return fail("invalid number");
        if (peek() == '0') {
            ++pos_;
            if (!at_end() && peek() >= '0' && peek() <= '9') return fail("leading zero in number");
        } else if (peek() >= '1' && peek() <= '9') {
            while (!at_end() && peek() >= '0' && peek() <= '9') ++pos_;
        } else {
            return fail("invalid number");
        }
        if (peek() == '.') {
            integral = false;
            ++pos_;
            if (at_end() || peek() < '0' || peek() > '9') return fail("invalid fraction");
            while (!at_end() && peek() >= '0' && peek() <= '9') ++pos_;
        }
        if (peek() == 'e' || peek() == 'E') {
            integral = false;
            ++pos_;
            if (peek() == '+' || peek() == '-') ++pos_;
            if (at_end() || peek() < '0' || peek() > '9') return fail("invalid exponent");
            while (!at_end() && peek() >= '0' && peek() <= '9') ++pos_;
        }
        const char* first = s_.data() + start;
        const char* last = s_.data() + pos_;

        Value v;
        v.type_ = Type::Number;
        if (integral) {
            std::int64_t i = 0;
            const auto r = std::from_chars(first, last, i);
            if (r.ec == std::errc() && r.ptr == last) {
                v.set_int(i);
                out = std::move(v);
                return true;
            }
            // Out of int64 range: fall through to double.
        }
        double d = 0.0;
        const auto r = std::from_chars(first, last, d);
        if (r.ptr != last || (r.ec != std::errc() && r.ec != std::errc::result_out_of_range))
            return fail("invalid number");
        if (r.ec == std::errc::result_out_of_range) {
            // Overflow to infinity is not representable in JSON output; reject.
            // Underflow (tiny magnitudes) is harmless: treat as zero.
            // Heuristic: a negative exponent, or no exponent with a zero integer part,
            // means the magnitude is below the smallest double.
            bool has_exp = false;
            bool tiny = false;
            for (const char* p = first; p != last; ++p) {
                if (*p == 'e' || *p == 'E') {
                    has_exp = true;
                    tiny = (p + 1 != last && p[1] == '-');
                    break;
                }
            }
            if (!has_exp) {
                const char* p = first;
                if (*p == '-') ++p;
                tiny = (*p == '0');
            }
            if (!tiny) return fail("number out of range");
            d = 0.0;
        }
        if (!std::isfinite(d)) return fail("number out of range");
        v.num_ = d;
        std::int64_t i = 0;
        if (double_to_exact_int(d, i)) {
            v.int_ = i;
            v.is_int_ = true;
        }
        out = std::move(v);
        return true;
    }

    std::string_view s_;
    const ParseOptions& opts_;
    std::size_t pos_ = 0;
    const char* err_ = "parse error";
};

ParseResult parse(std::string_view text, const ParseOptions& opts) { return Parser(text, opts).run(); }

// ---------------------------------------------------------------------------
// Serializer

bool is_valid_utf8(std::string_view s) noexcept {
    std::size_t i = 0;
    while (i < s.size()) {
        const auto b = static_cast<unsigned char>(s[i]);
        if (b < 0x80) {
            ++i;
            continue;
        }
        const std::size_t n = utf8_seq_len(s, i);
        if (n == 0) return false;
        i += n;
    }
    return true;
}

namespace {

void serialize_string(std::string_view s, std::string& out) {
    static constexpr char kHex[] = "0123456789abcdef";
    out.push_back('"');
    std::size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c >= 0x80) {
            const std::size_t n = utf8_seq_len(s, i);
            if (n == 0) {
                out.append("\xEF\xBF\xBD");  // U+FFFD replacement character
                ++i;
            } else {
                out.append(s.data() + i, n);
                i += n;
            }
            continue;
        }
        switch (c) {
            case '"': out.append("\\\""); break;
            case '\\': out.append("\\\\"); break;
            case '\b': out.append("\\b"); break;
            case '\f': out.append("\\f"); break;
            case '\n': out.append("\\n"); break;
            case '\r': out.append("\\r"); break;
            case '\t': out.append("\\t"); break;
            default:
                if (c < 0x20 || c == 0x7F) {
                    out.append("\\u00");
                    out.push_back(kHex[c >> 4]);
                    out.push_back(kHex[c & 0xF]);
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
        ++i;
    }
    out.push_back('"');
}

void serialize_number(const Value& v, std::string& out) {
    char buf[64];
    if (auto i = v.as_int()) {
        const auto r = std::to_chars(buf, buf + sizeof(buf), *i);
        out.append(buf, r.ptr);
        return;
    }
    const double d = v.as_double();
    if (!std::isfinite(d)) {
        out.append("null");
        return;
    }
    const auto r = std::to_chars(buf, buf + sizeof(buf), d);  // shortest round-trip form
    out.append(buf, r.ptr);
}

}  // namespace

void serialize_to(const Value& v, std::string& out) {
    switch (v.type()) {
        case Type::Null: out.append("null"); return;
        case Type::Bool: out.append(v.as_bool() ? "true" : "false"); return;
        case Type::Number: serialize_number(v, out); return;
        case Type::String: serialize_string(v.as_string(), out); return;
        case Type::Array: {
            out.push_back('[');
            bool first = true;
            for (const auto& e : v.as_array()) {
                if (!first) out.push_back(',');
                first = false;
                serialize_to(e, out);
            }
            out.push_back(']');
            return;
        }
        case Type::Object: {
            out.push_back('{');
            bool first = true;
            for (const auto& kv : v.as_object()) {
                if (!first) out.push_back(',');
                first = false;
                serialize_string(kv.first, out);
                out.push_back(':');
                serialize_to(kv.second, out);
            }
            out.push_back('}');
            return;
        }
    }
}

std::string serialize(const Value& v) {
    std::string out;
    serialize_to(v, out);
    return out;
}

}  // namespace opensweep::json
