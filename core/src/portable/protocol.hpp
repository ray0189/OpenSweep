// OpenSweep Core IPC protocol v1: request validation, response building and command dispatch.
// See docs/core/ipc-protocol.md (binding specification).
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>

#include "json.hpp"

namespace opensweep::protocol {

inline constexpr std::int64_t kProtocolVersion = 1;
// Request ids must be integers. They are restricted to the range a JSON/JavaScript client can
// represent exactly (|id| <= 2^53 - 1) so the echoed id always matches.
inline constexpr std::int64_t kMaxSafeInteger = 9007199254740991;  // 2^53 - 1
inline constexpr std::size_t kMaxCmdLength = 64;

enum class ErrorCode {
    BadFrame,
    TooLarge,
    BadJson,
    BadVersion,
    UnknownCmd,
    BadArgs,
    Forbidden,
    Internal,
    Timeout,
};

std::string_view error_code_name(ErrorCode code) noexcept;

struct Request {
    std::int64_t id = 0;
    std::string cmd;
    json::Value args = json::Value::object();  // always an object (empty when omitted)
};

struct ValidationResult {
    bool ok = false;
    Request request;            // valid when ok
    ErrorCode code = ErrorCode::Internal;  // valid when !ok
    std::string message;        // valid when !ok; never contains request bytes
    std::int64_t id = 0;        // id to echo in the error response (0 if unknown)
};

// Parses and validates one request payload (the bytes of one frame).
ValidationResult validate_request(std::string_view payload);

json::Value make_ok(std::int64_t id, json::Value result);
json::Value make_error(std::int64_t id, ErrorCode code, std::string_view message);

// Capabilities are a bit set.
enum class Capability : std::uint32_t {
    None = 0,
    Read = 1u << 0,
    Admin = 1u << 1,
};

constexpr std::uint32_t cap_bit(Capability c) noexcept { return static_cast<std::uint32_t>(c); }

// Session of a client whose token could not be queried (handlers that need the session refuse).
inline constexpr std::uint32_t kUnknownSession = 0xFFFFFFFFu;

struct ClientContext {
    std::uint32_t pid = 0;
    // Terminal Services session of the client, taken from the client's *token* by the server
    // (TokenSessionId while impersonating), never from request data.
    std::uint32_t session_id = kUnknownSession;
    std::uint32_t capabilities = cap_bit(Capability::Read);
    bool has(Capability c) const noexcept { return (capabilities & cap_bit(c)) == cap_bit(c); }
};

// Handlers may throw CommandError to return a specific error code (e.g. bad_args).
// Any other exception becomes `internal` (its what() text is NOT sent to the client).
class CommandError : public std::runtime_error {
public:
    CommandError(ErrorCode code, const std::string& message)
        : std::runtime_error(message), code_(code) {}
    ErrorCode code() const noexcept { return code_; }

private:
    ErrorCode code_;
};

using Handler = std::function<json::Value(const Request&, const ClientContext&)>;

struct HandleOutcome {
    std::string response;   // serialized JSON response payload (not framed)
    bool is_error = false;  // response has ok:false
    ErrorCode code = ErrorCode::Internal;  // when is_error
    // Command name safe for logging: the cmd if it is a registered command, else empty.
    std::string log_cmd;
};

class Dispatcher {
public:
    // Registers (or replaces) a command. Throws std::invalid_argument for an empty/too long name.
    void add(std::string name, Capability required, Handler handler);
    bool has(std::string_view name) const;

    // Dispatches an already validated request.
    json::Value dispatch(const Request& req, const ClientContext& ctx) const;

    // Full pipeline for one frame payload: validate, check capability, run handler, serialize.
    // Guarantees that the serialized response is <= max_response bytes (falls back to an
    // `internal` error when a handler result would be larger).
    HandleOutcome handle(std::string_view payload, const ClientContext& ctx,
                         std::size_t max_response) const;

private:
    struct Entry {
        Capability required;
        Handler handler;
    };
    std::map<std::string, Entry, std::less<>> commands_;
};

}  // namespace opensweep::protocol
