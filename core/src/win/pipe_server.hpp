// Overlapped named-pipe server implementing docs/core/ipc-protocol.md.
//
// One I/O thread drives up to kMaxInstances pipe instances with overlapped I/O and one
// manual-reset event per instance. Each instance is a small state machine:
//   Listening -> (ConnectNamedPipe) -> Connected: Read -> process frames -> Write -> Read ...
//   -> Disconnect -> Listening.
// Deadlines: a request must arrive completely within 5 s of its first byte (else `timeout`
// is sent and the connection closed), an idle connection is closed after 60 s, and a
// response write that does not complete within 5 s closes the connection.
#pragma once

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "commands.hpp"
#include "framing.hpp"
#include "protocol.hpp"

namespace opensweep::win {

inline constexpr const wchar_t* kPipeName = L"\\\\.\\pipe\\OpenSweepCore.v1";
// D:P protected DACL: deny network logons; SYSTEM and Administrators full access;
// interactive users read/write (enough to connect and exchange messages, but not
// FILE_CREATE_PIPE_INSTANCE, so they cannot add rogue server instances).
inline constexpr const wchar_t* kPipeSddl = L"D:P(D;;GA;;;NU)(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)";
inline constexpr DWORD kMaxInstances = 8;

class PipeServer {
public:
    PipeServer(const protocol::Dispatcher& dispatcher, commands::ServerCounters& counters);
    ~PipeServer();
    PipeServer(const PipeServer&) = delete;
    PipeServer& operator=(const PipeServer&) = delete;

    // Creates the pipe instances. The first one uses FILE_FLAG_FIRST_PIPE_INSTANCE, so this
    // fails if any other process already owns the pipe name (anti-squatting).
    bool open();

    // Serves clients until `stop_event` is signalled, then cancels all I/O and closes every
    // instance. Returns false if the server failed (e.g. all instances died).
    bool serve(HANDLE stop_event);

private:
    using Clock = std::chrono::steady_clock;

    // Draining: after a final error response the server keeps reading (and discarding) for a short
    // linger period so the client can read the response before DisconnectNamedPipe discards
    // unread pipe data.
    enum class State { Listening, Connecting, Connected, Reading, Writing, Draining, Dead };

    struct Instance {
        HANDLE pipe = INVALID_HANDLE_VALUE;
        HANDLE event = nullptr;  // manual-reset, used by OVERLAPPED
        OVERLAPPED ov{};
        State state = State::Listening;
        bool io_pending = false;
        bool close_after_write = false;
        bool identified = false;
        bool counted = false;  // included in counters.clients
        bool leaked = false;   // I/O could not be cancelled; memory intentionally kept alive
        std::vector<char> rbuf;
        std::string wbuf;
        framing::Decoder decoder{framing::kMaxRequestPayload};
        framing::FrameError frame_error = framing::FrameError::None;
        std::deque<std::string> queued;  // complete request payloads awaiting processing
        protocol::ClientContext ctx;
        ULONG client_pid = 0;
        std::uint64_t served = 0;
        bool partial = false;
        Clock::time_point last_activity{};
        Clock::time_point partial_since{};
        Clock::time_point write_started{};
        Clock::time_point drain_until{};
    };

    bool create_pipe_handle(Instance& inst, bool first);
    void recycle(Instance& inst);  // close + recreate the pipe handle after a failure
    void begin_connect(Instance& inst);
    void on_connected(Instance& inst);
    void begin_read(Instance& inst);
    void begin_write(Instance& inst, const std::string& payload);
    void begin_drain_read(Instance& inst);
    void complete_io(Instance& inst);
    void drive(Instance& inst);
    void process_one(Instance& inst);
    void identify_client(Instance& inst);
    void disconnect(Instance& inst);
    bool cancel_and_wait(Instance& inst, DWORD wait_ms);
    void check_deadlines(Instance& inst, Clock::time_point now);
    void send_error_and_close(Instance& inst, protocol::ErrorCode code, const char* message);
    Clock::time_point deadline(const Instance& inst) const;
    void shutdown_all();

    const protocol::Dispatcher& dispatcher_;
    commands::ServerCounters& counters_;
    PSECURITY_DESCRIPTOR sd_ = nullptr;
    std::vector<std::unique_ptr<Instance>> instances_;
};

}  // namespace opensweep::win
