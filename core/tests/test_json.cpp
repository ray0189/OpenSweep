#include <cmath>
#include <string>

#include "json.hpp"
#include "test_framework.hpp"

using namespace opensweep::json;

namespace {
bool parses(std::string_view s) { return parse(s).ok(); }
std::string nested(char open, char close, int depth) {
    return std::string(static_cast<std::size_t>(depth), open) + std::string(static_cast<std::size_t>(depth), close);
}
}  // namespace

TEST(json_valid_documents) {
    auto r = parse(R"({"a":1,"b":[true,false,null],"c":{"d":"x"},"e":-2.5e3,"f":""})");
    CHECK(r.ok());
    const Value& v = *r.value;
    CHECK(v.is_object());
    CHECK_EQ(v.size(), 5u);
    CHECK(v.find("a")->as_int() == 1);
    CHECK(v.find("b")->is_array());
    CHECK_EQ(v.find("b")->as_array().size(), 3u);
    CHECK(v.find("b")->as_array()[0].as_bool() == true);
    CHECK(v.find("b")->as_array()[2].is_null());
    CHECK_EQ(v.find("c")->find("d")->as_string(), std::string("x"));
    CHECK_EQ(v.find("e")->as_double(), -2500.0);
    CHECK(v.find("e")->as_int() == -2500);  // exact integer even with exponent
    CHECK(v.find("f")->as_string().empty());
    CHECK(v.find("missing") == nullptr);

    CHECK(parses("0"));
    CHECK(parses("-0"));
    CHECK(parses("  \t\r\n true \n"));
    CHECK(parses("\"\""));
    CHECK(parses("[]"));
    CHECK(parses("{}"));
    CHECK(parses("[[],{}]"));
    CHECK(parses("1.5E+2"));
}

TEST(json_invalid_documents) {
    const char* bad[] = {
        "",  "   ", "{", "}", "[", "[1,]", "{\"a\":1,}", "{a:1}", "{'a':1}", "[1 2]", "01", "1.", ".5", "-", "+1",
        "1e", "1e+", "NaN", "Infinity", "-Infinity", "tru", "nul", "true false", "{} x", "\"abc", "\"\\x\"",
        "\"\\u12\"", "\"\\u12G4\"", "\"a\nb\"", "\"tab\there\"", "[1]]", "// c\n1", "/*c*/1", "{\"a\" 1}",
        "{\"a\":}", "[,]", "\xEF\xBB\xBF{}",  // BOM is not whitespace
    };
    for (const char* s : bad) {
        if (parses(s)) ::testfw::report_failure(__FILE__, __LINE__, std::string("accepted invalid JSON: ") + s);
    }
    // Error messages never echo input.
    auto r = parse("{\"secret\":");
    CHECK(!r.ok());
    CHECK(r.error.find("secret") == std::string::npos);
}

TEST(json_duplicate_keys_rejected) {
    CHECK(!parses(R"({"a":1,"a":2})"));
    CHECK(parses(R"({"a":1,"b":{"a":2}})"));
}

TEST(json_depth_limit) {
    CHECK(parses(nested('[', ']', 32)));
    CHECK(!parses(nested('[', ']', 33)));
    CHECK(!parses(nested('[', ']', 100000)));  // must not overflow the stack

    std::string obj;
    for (int i = 0; i < 32; ++i) obj += "{\"a\":";
    obj += "1";
    for (int i = 0; i < 32; ++i) obj += "}";
    CHECK(parses(obj));
    std::string obj33 = "{\"a\":" + obj + "}";
    CHECK(!parses(obj33));

    ParseOptions o;
    o.max_depth = 2;
    CHECK(parse("[[1]]", o).ok());
    CHECK(!parse("[[[1]]]", o).ok());
}

TEST(json_size_limit) {
    ParseOptions o;
    o.max_size = 10;
    CHECK(parse("[1,2,3,4]", o).ok());     // 9 bytes
    CHECK(parse("[1,2,3,45]", o).ok());    // 10 bytes
    CHECK(!parse("[1,2,3,456]", o).ok());  // 11 bytes
}

TEST(json_utf8_strict) {
    // Valid multi-byte UTF-8 passes through unchanged.
    auto r = parse("\"caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80\"");
    CHECK(r.ok());
    CHECK_EQ(r.value->as_string(), std::string("caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80"));

    CHECK(!parses("\"\xC0\xAF\""));          // overlong '/'
    CHECK(!parses("\"\xE0\x80\xAF\""));      // overlong 3-byte
    CHECK(!parses("\"\xED\xA0\x80\""));      // encoded surrogate U+D800
    CHECK(!parses("\"\xF4\x90\x80\x80\""));  // > U+10FFFF
    CHECK(!parses("\"\x80\""));              // stray continuation
    CHECK(!parses("\"\xC3\""));              // truncated sequence
    CHECK(!parses("\"\xFF\""));
    CHECK(!parses("[\"ok\"]\xC3"));  // invalid UTF-8 outside strings

    CHECK(is_valid_utf8("plain"));
    CHECK(!is_valid_utf8("\xC3\x28"));
}

TEST(json_unicode_escapes) {
    auto r = parse(R"("\u0041\u00e9\u20AC\ud83d\ude00\/")");
    CHECK(r.ok());
    CHECK_EQ(r.value->as_string(), std::string("A\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80/"));
    CHECK(!parses(R"("\ud83d")"));         // lone high surrogate
    CHECK(!parses(R"("\ude00")"));         // lone low surrogate
    CHECK(!parses(R"("\ud83d\u0041")"));   // high surrogate + non-low
    CHECK(!parses(R"("\ud83dx")"));
    auto nul = parse(R"("a\u0000b")");
    CHECK(nul.ok());
    CHECK_EQ(nul.value->as_string().size(), 3u);
}

TEST(json_escaping_round_trip) {
    std::string s = "quote\" backslash\\ slash/ nl\n cr\r tab\t bs\b ff\f ctl\x01\x1f del\x7f utf8 \xC3\xA9";
    s.push_back('\0');
    s += "after-nul";
    Value v = Value::object();
    v.set("k\"ey", s);
    const std::string out = serialize(v);
    // No raw control characters in output.
    for (char c : out) CHECK(static_cast<unsigned char>(c) >= 0x20);
    CHECK(out.find("\\u0000") != std::string::npos);
    CHECK(out.find("\\u0001") != std::string::npos);
    CHECK(out.find("\\n") != std::string::npos);
    auto back = parse(out);
    CHECK(back.ok());
    CHECK(*back.value == v);
    CHECK_EQ(back.value->find("k\"ey")->as_string(), s);
}

TEST(json_serializer_replaces_invalid_utf8) {
    Value v(std::string("a\xFF" "b\xC3"));
    const std::string out = serialize(v);
    CHECK(is_valid_utf8(out));
    CHECK_EQ(out, std::string("\"a\xEF\xBF\xBD" "b\xEF\xBF\xBD\""));
}

TEST(json_numbers) {
    auto big = parse("9223372036854775807");
    CHECK(big.ok());
    CHECK(big.value->as_int() == INT64_MAX);
    auto neg = parse("-9223372036854775808");
    CHECK(neg.ok());
    CHECK(neg.value->as_int() == INT64_MIN);

    auto over = parse("12345678901234567890");  // > int64: kept as double only
    CHECK(over.ok());
    CHECK(!over.value->is_int());
    CHECK(std::fabs(over.value->as_double() - 1.2345678901234567e19) < 1e5);

    CHECK(!parses("1e400"));   // overflow to infinity rejected
    CHECK(!parses("-1e400"));
    auto tiny = parse("1e-400");  // underflow becomes 0
    CHECK(tiny.ok());
    CHECK_EQ(tiny.value->as_double(), 0.0);

    auto frac = parse("7.0");
    CHECK(frac.ok());
    CHECK(frac.value->as_int() == 7);
    auto half = parse("7.5");
    CHECK(!half.value->is_int());

    // Serialization.
    {
        // uint64 above INT64_MAX becomes a double (2^64); output must parse back to the same double.
        const Value u(std::uint64_t{18446744073709551615ull});
        CHECK(!u.is_int());
        auto rt = parse(serialize(u));
        CHECK(rt.ok());
        CHECK_EQ(rt.value->as_double(), 18446744073709551616.0);
    }
    CHECK_EQ(serialize(Value(INT64_MIN)), std::string("-9223372036854775808"));
    CHECK_EQ(serialize(Value(42)), std::string("42"));
    CHECK_EQ(serialize(Value(0.5)), std::string("0.5"));
    CHECK_EQ(serialize(Value(3.0)), std::string("3"));
    CHECK_EQ(serialize(Value(std::nan(""))), std::string("null"));
    CHECK_EQ(serialize(Value(INFINITY)), std::string("null"));

    // Round trip of a double.
    auto pi = parse(serialize(Value(3.141592653589793)));
    CHECK(pi.ok());
    CHECK_EQ(pi.value->as_double(), 3.141592653589793);
}

TEST(json_builder_and_serialize) {
    Value v = Value::object();
    v.set("b", true).set("n", nullptr).set("s", "x");
    Value arr = Value::array();
    arr.push_back(1).push_back("two");
    v.set("arr", arr);
    v.set("b", false);  // replace keeps position
    CHECK_EQ(serialize(v), std::string(R"({"b":false,"n":null,"s":"x","arr":[1,"two"]})"));
    CHECK_EQ(v.size(), 4u);
    // Type-mismatched accessors return defaults.
    CHECK(v.find("s")->as_int() == std::nullopt);
    CHECK(v.find("s")->as_array().empty());
    CHECK_EQ(v.find("b")->as_string(), std::string());
}
