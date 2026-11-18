#include <stdexcept>
#include <string>

#include "framing.hpp"
#include "json.hpp"
#include "protocol.hpp"
#include "test_framework.hpp"

using namespace opensweep;
using namespace opensweep::protocol;

namespace {

json::Value parse_ok(const std::string& s) {
    auto r = json::parse(s);
    if (!r.ok()) throw std::runtime_error("response is not JSON");
    return *r.value;
}

std::string code_of(const json::Value& resp) {
    const json::Value* e = resp.find("error");
    return e && e->find("code") ? e->find("code")->as_string() : std::string();
}

std::int64_t id_of(const json::Value& resp) { return resp.find("id")->as_int().value_or(-999); }

Dispatcher make_dispatcher() {
    Dispatcher d;
    d.add("echo", Capability::Read, [](const Request& r, const ClientContext&) {
        json::Value v = json::Value::object();
        v.set("args", r.args);
        return v;
    });
    d.add("secret", Capability::Admin, [](const Request&, const ClientContext&) { return json::Value("s3cret"); });
    d.add("badargs", Capability::Read, [](const Request&, const ClientContext&) -> json::Value {
        throw CommandError(ErrorCode::BadArgs, "x must be positive");
    });
    d.add("boom", Capability::Read, [](const Request&, const ClientContext&) -> json::Value {
        throw std::runtime_error("sensitive internal detail");
    });
    d.add("huge", Capability::Read, [](const Request&, const ClientContext&) {
        return json::Value(std::string(2000, 'x'));
    });
    return d;
}

json::Value run(const Dispatcher& d, const std::string& payload, const ClientContext& ctx = {},
                std::size_t max = framing::kMaxResponsePayload) {
    return parse_ok(d.handle(payload, ctx, max).response);
}

}  // namespace

TEST(protocol_error_code_names) {
    CHECK_EQ(error_code_name(ErrorCode::BadFrame), std::string_view("bad_frame"));
    CHECK_EQ(error_code_name(ErrorCode::TooLarge), std::string_view("too_large"));
    CHECK_EQ(error_code_name(ErrorCode::BadJson), std::string_view("bad_json"));
    CHECK_EQ(error_code_name(ErrorCode::BadVersion), std::string_view("bad_version"));
    CHECK_EQ(error_code_name(ErrorCode::UnknownCmd), std::string_view("unknown_cmd"));
    CHECK_EQ(error_code_name(ErrorCode::BadArgs), std::string_view("bad_args"));
    CHECK_EQ(error_code_name(ErrorCode::Forbidden), std::string_view("forbidden"));
    CHECK_EQ(error_code_name(ErrorCode::Internal), std::string_view("internal"));
    CHECK_EQ(error_code_name(ErrorCode::Timeout), std::string_view("timeout"));
}

TEST(protocol_ok_response_shape) {
    auto d = make_dispatcher();
    auto out = d.handle(R"({"v":1,"id":7,"cmd":"echo","args":{"k":[1,2]},"extra":"ignored"})", {},
                        framing::kMaxResponsePayload);
    CHECK(!out.is_error);
    CHECK_EQ(out.log_cmd, std::string("echo"));
    CHECK_EQ(out.response, std::string(R"({"v":1,"id":7,"ok":true,"result":{"args":{"k":[1,2]}}})"));

    // args omitted -> handler sees an empty object.
    auto r = run(d, R"({"v":1,"id":8,"cmd":"echo"})");
    CHECK(r.find("ok")->as_bool());
    CHECK(r.find("result")->find("args")->is_object());
    CHECK_EQ(r.find("result")->find("args")->size(), 0u);
}

TEST(protocol_error_response_shape) {
    const std::string s = json::serialize(make_error(5, ErrorCode::Timeout, "slow"));
    CHECK_EQ(s, std::string(R"({"v":1,"id":5,"ok":false,"error":{"code":"timeout","message":"slow"}})"));
}

TEST(protocol_bad_json) {
    auto d = make_dispatcher();
    for (const char* p : {"not json", "{\"v\":1,", "[1,2]", "\"str\"", "42", "null", "{\"v\":1,\"id\":1,\"cmd\":\"echo\"}x"}) {
        auto out = d.handle(p, {}, framing::kMaxResponsePayload);
        CHECK(out.is_error);
        CHECK(out.code == ErrorCode::BadJson);
        auto r = parse_ok(out.response);
        CHECK_EQ(code_of(r), std::string("bad_json"));
        CHECK_EQ(id_of(r), 0);
        CHECK(out.log_cmd.empty());
    }
    // Deep nesting in args is rejected as bad_json.
    std::string deep = R"({"v":1,"id":3,"cmd":"echo","args":)";
    for (int i = 0; i < 40; ++i) deep += "{\"a\":";
    deep += "1";
    for (int i = 0; i < 40; ++i) deep += "}";
    deep += "}";
    CHECK_EQ(code_of(run(d, deep)), std::string("bad_json"));
    // Oversized request payload is rejected by the JSON size limit too.
    std::string big = R"({"v":1,"id":3,"cmd":"echo","args":{"x":")" + std::string(70000, 'a') + "\"}}";
    CHECK_EQ(code_of(run(d, big)), std::string("bad_json"));
}

TEST(protocol_bad_version) {
    auto d = make_dispatcher();
    auto r = run(d, R"({"v":2,"id":9,"cmd":"echo"})");
    CHECK_EQ(code_of(r), std::string("bad_version"));
    CHECK_EQ(id_of(r), 9);  // id echoed when it is valid
    CHECK_EQ(code_of(run(d, R"({"id":9,"cmd":"echo"})")), std::string("bad_version"));
    CHECK_EQ(code_of(run(d, R"({"v":"1","id":9,"cmd":"echo"})")), std::string("bad_version"));
    CHECK_EQ(code_of(run(d, R"({"v":1.5,"id":9,"cmd":"echo"})")), std::string("bad_version"));
    auto r2 = run(d, R"({"v":0,"id":"x","cmd":"echo"})");
    CHECK_EQ(code_of(r2), std::string("bad_version"));
    CHECK_EQ(id_of(r2), 0);
}

TEST(protocol_bad_id) {
    auto d = make_dispatcher();
    for (const char* p : {R"({"v":1,"cmd":"echo"})", R"({"v":1,"id":"7","cmd":"echo"})", R"({"v":1,"id":1.5,"cmd":"echo"})",
                          R"({"v":1,"id":null,"cmd":"echo"})", R"({"v":1,"id":9007199254740992,"cmd":"echo"})"}) {
        auto r = run(d, p);
        CHECK_EQ(code_of(r), std::string("bad_json"));
        CHECK_EQ(id_of(r), 0);
    }
    // Largest safe integer and negatives are accepted and echoed.
    CHECK_EQ(id_of(run(d, R"({"v":1,"id":9007199254740991,"cmd":"echo"})")), 9007199254740991);
    CHECK_EQ(id_of(run(d, R"({"v":1,"id":-4,"cmd":"echo"})")), -4);
    CHECK_EQ(id_of(run(d, R"({"v":1,"id":0,"cmd":"echo"})")), 0);
}

TEST(protocol_bad_cmd_field) {
    auto d = make_dispatcher();
    auto r = run(d, R"({"v":1,"id":4,"cmd":5})");
    CHECK_EQ(code_of(r), std::string("bad_json"));
    CHECK_EQ(id_of(r), 4);
    CHECK_EQ(code_of(run(d, R"({"v":1,"id":4})")), std::string("bad_json"));
    CHECK_EQ(code_of(run(d, R"({"v":1,"id":4,"cmd":""})")), std::string("bad_json"));
}

TEST(protocol_unknown_cmd) {
    auto d = make_dispatcher();
    auto out = d.handle(R"({"v":1,"id":11,"cmd":"rm -rf /"})", {}, framing::kMaxResponsePayload);
    auto r = parse_ok(out.response);
    CHECK_EQ(code_of(r), std::string("unknown_cmd"));
    CHECK_EQ(id_of(r), 11);
    CHECK(out.log_cmd.empty());  // unregistered names are never logged
    CHECK(out.response.find("rm -rf") == std::string::npos);  // never echoed
    const std::string long_cmd = R"({"v":1,"id":12,"cmd":")" + std::string(65, 'a') + "\"}";
    CHECK_EQ(code_of(run(d, long_cmd)), std::string("unknown_cmd"));
}

TEST(protocol_bad_args) {
    auto d = make_dispatcher();
    for (const char* p : {R"({"v":1,"id":2,"cmd":"echo","args":[]})", R"({"v":1,"id":2,"cmd":"echo","args":null})",
                          R"({"v":1,"id":2,"cmd":"echo","args":"x"})", R"({"v":1,"id":2,"cmd":"echo","args":1})"}) {
        auto r = run(d, p);
        CHECK_EQ(code_of(r), std::string("bad_args"));
        CHECK_EQ(id_of(r), 2);
    }
    // Handler-raised bad_args.
    auto r = run(d, R"({"v":1,"id":3,"cmd":"badargs"})");
    CHECK_EQ(code_of(r), std::string("bad_args"));
    CHECK_EQ(r.find("error")->find("message")->as_string(), std::string("x must be positive"));
}

TEST(protocol_forbidden_capability) {
    auto d = make_dispatcher();
    ClientContext reader;  // default: read only
    auto r = run(d, R"({"v":1,"id":21,"cmd":"secret"})", reader);
    CHECK_EQ(code_of(r), std::string("forbidden"));
    CHECK_EQ(id_of(r), 21);
    CHECK(json::serialize(r).find("s3cret") == std::string::npos);

    ClientContext admin;
    admin.capabilities = cap_bit(Capability::Read) | cap_bit(Capability::Admin);
    auto ok = run(d, R"({"v":1,"id":22,"cmd":"secret"})", admin);
    CHECK(ok.find("ok")->as_bool());
    CHECK_EQ(ok.find("result")->as_string(), std::string("s3cret"));

    ClientContext none;
    none.capabilities = 0;
    CHECK_EQ(code_of(run(d, R"({"v":1,"id":23,"cmd":"echo"})", none)), std::string("forbidden"));
    // Unknown command wins over capability checks (no information about admin commands leaks).
    CHECK_EQ(code_of(run(d, R"({"v":1,"id":24,"cmd":"nope"})", none)), std::string("unknown_cmd"));
}

TEST(protocol_internal_errors) {
    auto d = make_dispatcher();
    auto r = run(d, R"({"v":1,"id":31,"cmd":"boom"})");
    CHECK_EQ(code_of(r), std::string("internal"));
    CHECK(json::serialize(r).find("sensitive") == std::string::npos);
    // Response exceeding the limit is replaced by an internal error.
    auto out = d.handle(R"({"v":1,"id":32,"cmd":"huge"})", {}, 1000);
    CHECK(out.is_error);
    CHECK(out.code == ErrorCode::Internal);
    CHECK(out.response.size() <= 1000u);
    CHECK_EQ(id_of(parse_ok(out.response)), 32);
    // Without the limit it succeeds.
    CHECK(!d.handle(R"({"v":1,"id":33,"cmd":"huge"})", {}, framing::kMaxResponsePayload).is_error);
}

TEST(protocol_dispatcher_registration) {
    Dispatcher d;
    CHECK_THROWS(d.add("", Capability::Read, [](const Request&, const ClientContext&) { return json::Value(); }));
    CHECK_THROWS(d.add(std::string(65, 'a'), Capability::Read,
                       [](const Request&, const ClientContext&) { return json::Value(); }));
    CHECK_THROWS(d.add("x", Capability::Read, Handler{}));
    d.add("x", Capability::Read, [](const Request&, const ClientContext&) { return json::Value(1); });
    CHECK(d.has("x"));
    CHECK(!d.has("y"));
}

TEST(protocol_validate_request_direct) {
    auto v = validate_request(R"({"v":1,"id":1,"cmd":"hello","args":{}})");
    CHECK(v.ok);
    CHECK_EQ(v.request.cmd, std::string("hello"));
    CHECK_EQ(v.request.id, 1);
    // Messages never contain request bytes.
    auto bad = validate_request(R"({"v":1,"id":1,"cmd":"hello","args":"TOPSECRET"})");
    CHECK(!bad.ok);
    CHECK(bad.message.find("TOPSECRET") == std::string::npos);
}
