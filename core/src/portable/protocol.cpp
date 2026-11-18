#include "protocol.hpp"

#include <exception>
#include <utility>

#include "framing.hpp"

namespace opensweep::protocol {

std::string_view error_code_name(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::BadFrame: return "bad_frame";
        case ErrorCode::TooLarge: return "too_large";
        case ErrorCode::BadJson: return "bad_json";
        case ErrorCode::BadVersion: return "bad_version";
        case ErrorCode::UnknownCmd: return "unknown_cmd";
        case ErrorCode::BadArgs: return "bad_args";
        case ErrorCode::Forbidden: return "forbidden";
        case ErrorCode::Internal: return "internal";
        case ErrorCode::Timeout: return "timeout";
    }
    return "internal";
}

namespace {

ValidationResult fail(ErrorCode code, std::string message, std::int64_t id) {
    ValidationResult r;
    r.ok = false;
    r.code = code;
    r.message = std::move(message);
    r.id = id;
    return r;
}

bool valid_id(const json::Value* v, std::int64_t& out) {
    if (!v) return false;
    const auto i = v->as_int();
    if (!i) return false;
    if (*i > kMaxSafeInteger || *i < -kMaxSafeInteger) return false;
    out = *i;
    return true;
}

}  // namespace

ValidationResult validate_request(std::string_view payload) {
    json::ParseOptions opts;
    opts.max_depth = 32;
    opts.max_size = framing::kMaxRequestPayload;
    auto parsed = json::parse(payload, opts);
    if (!parsed.ok()) return fail(ErrorCode::BadJson, "request is not valid JSON: " + parsed.error, 0);

    const json::Value& root = *parsed.value;
    if (!root.is_object()) return fail(ErrorCode::BadJson, "request must be a JSON object", 0);

    std::int64_t id = 0;
    const bool id_ok = valid_id(root.find("id"), id);
    const std::int64_t echo_id = id_ok ? id : 0;

    const json::Value* v = root.find("v");
    if (!v || v->as_int() != kProtocolVersion)
        return fail(ErrorCode::BadVersion, "unsupported protocol version (expected v: 1)", echo_id);

    if (!id_ok) return fail(ErrorCode::BadJson, "id must be an integer with |id| <= 2^53-1", 0);

    const json::Value* cmd = root.find("cmd");
    if (!cmd || !cmd->is_string() || cmd->as_string().empty())
        return fail(ErrorCode::BadJson, "cmd must be a non-empty string", echo_id);
    if (cmd->as_string().size() > kMaxCmdLength)
        return fail(ErrorCode::UnknownCmd, "unknown command", echo_id);

    const json::Value* args = root.find("args");
    if (args && !args->is_object()) return fail(ErrorCode::BadArgs, "args must be an object", echo_id);

    ValidationResult r;
    r.ok = true;
    r.id = id;
    r.request.id = id;
    r.request.cmd = cmd->as_string();
    r.request.args = args ? *args : json::Value::object();
    return r;
}

json::Value make_ok(std::int64_t id, json::Value result) {
    json::Value r = json::Value::object();
    r.set("v", kProtocolVersion);
    r.set("id", id);
    r.set("ok", true);
    r.set("result", std::move(result));
    return r;
}

json::Value make_error(std::int64_t id, ErrorCode code, std::string_view message) {
    json::Value err = json::Value::object();
    err.set("code", error_code_name(code));
    err.set("message", message);
    json::Value r = json::Value::object();
    r.set("v", kProtocolVersion);
    r.set("id", id);
    r.set("ok", false);
    r.set("error", std::move(err));
    return r;
}

void Dispatcher::add(std::string name, Capability required, Handler handler) {
    if (name.empty() || name.size() > kMaxCmdLength) throw std::invalid_argument("bad command name");
    if (!handler) throw std::invalid_argument("null handler");
    commands_[std::move(name)] = Entry{required, std::move(handler)};
}

bool Dispatcher::has(std::string_view name) const { return commands_.find(name) != commands_.end(); }

json::Value Dispatcher::dispatch(const Request& req, const ClientContext& ctx) const {
    const auto it = commands_.find(req.cmd);
    if (it == commands_.end()) return make_error(req.id, ErrorCode::UnknownCmd, "unknown command");
    if (!ctx.has(it->second.required))
        return make_error(req.id, ErrorCode::Forbidden, "client lacks the capability required by this command");
    try {
        return make_ok(req.id, it->second.handler(req, ctx));
    } catch (const CommandError& e) {
        return make_error(req.id, e.code(), e.what());
    } catch (const std::exception&) {
        return make_error(req.id, ErrorCode::Internal, "internal error");
    } catch (...) {
        return make_error(req.id, ErrorCode::Internal, "internal error");
    }
}

HandleOutcome Dispatcher::handle(std::string_view payload, const ClientContext& ctx,
                                 std::size_t max_response) const {
    HandleOutcome out;
    json::Value response;
    std::int64_t id = 0;
    const ValidationResult vr = validate_request(payload);
    if (!vr.ok) {
        response = make_error(vr.id, vr.code, vr.message);
        id = vr.id;
    } else {
        id = vr.request.id;
        if (has(vr.request.cmd)) out.log_cmd = vr.request.cmd;
        response = dispatch(vr.request, ctx);
    }
    out.response = json::serialize(response);
    if (out.response.size() > max_response) {
        response = make_error(id, ErrorCode::Internal, "response exceeds size limit");
        out.response = json::serialize(response);
    }
    const json::Value* ok = response.find("ok");
    out.is_error = !(ok && ok->as_bool());
    if (out.is_error) {
        const json::Value* err = response.find("error");
        const json::Value* code = err ? err->find("code") : nullptr;
        const std::string_view name = code ? std::string_view(code->as_string()) : std::string_view();
        out.code = ErrorCode::Internal;
        for (ErrorCode c : {ErrorCode::BadFrame, ErrorCode::TooLarge, ErrorCode::BadJson, ErrorCode::BadVersion,
                            ErrorCode::UnknownCmd, ErrorCode::BadArgs, ErrorCode::Forbidden, ErrorCode::Internal,
                            ErrorCode::Timeout}) {
            if (error_code_name(c) == name) out.code = c;
        }
    }
    return out;
}

}  // namespace opensweep::protocol
