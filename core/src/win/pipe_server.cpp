#include "pipe_server.hpp"

#include <sddl.h>

#include <algorithm>
#include <cstddef>
#include <string>

#include "log.hpp"

namespace opensweep::win {

namespace {

constexpr DWORD kPipeBufferSize = 64 * 1024;  // kernel in/out buffer quota per instance
constexpr DWORD kReadChunk = 16 * 1024;
constexpr DWORD kDefaultWaitMs = 5000;  // used by clients calling WaitNamedPipe(NMPWAIT_USE_DEFAULT_WAIT)
constexpr auto kRequestTimeout = std::chrono::seconds(5);
constexpr auto kIdleTimeout = std::chrono::seconds(60);
constexpr auto kWriteTimeout = std::chrono::seconds(5);
constexpr auto kDrainLinger = std::chrono::seconds(1);
constexpr DWORD kCancelWaitMs = 2000;
constexpr int kMaxStepsPerWake = 64;

std::string pid_str(ULONG pid) { return "pid=" + std::to_string(pid); }

}  // namespace

PipeServer::PipeServer(const protocol::Dispatcher& dispatcher, commands::ServerCounters& counters)
    : dispatcher_(dispatcher), counters_(counters) {}

PipeServer::~PipeServer() {
    shutdown_all();
    if (sd_) {
        LocalFree(sd_);
        sd_ = nullptr;
    }
}

bool PipeServer::create_pipe_handle(Instance& inst, bool first) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = sd_;
    sa.bInheritHandle = FALSE;
    // FILE_FLAG_FIRST_PIPE_INSTANCE: fail if the name already exists, so a process that
    //   created the pipe before us (squatting) is detected instead of joined.
    // PIPE_REJECT_REMOTE_CLIENTS: refuse connections over SMB (the ACL also denies NU).
    // Byte mode: framing is done by the protocol's length prefix.
    const DWORD open_mode =
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? static_cast<DWORD>(FILE_FLAG_FIRST_PIPE_INSTANCE) : 0u);
    const DWORD pipe_mode = PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS;
    inst.pipe = CreateNamedPipeW(kPipeName, open_mode, pipe_mode, kMaxInstances, kPipeBufferSize, kPipeBufferSize,
                                 kDefaultWaitMs, &sa);
    if (inst.pipe == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        if (first && err == ERROR_ACCESS_DENIED) {
            log::error("CreateNamedPipe failed: the pipe name is already in use by another process "
                       "(another OpenSweepCore instance, or a squatter) or access was denied: " +
                       log::win32_error(err));
        } else {
            log::error("CreateNamedPipe failed: " + log::win32_error(err));
        }
        return false;
    }
    inst.state = State::Listening;
    inst.io_pending = false;
    return true;
}

bool PipeServer::open() {
    if (!sd_ && !ConvertStringSecurityDescriptorToSecurityDescriptorW(kPipeSddl, SDDL_REVISION_1, &sd_, nullptr)) {
        log::error("ConvertStringSecurityDescriptorToSecurityDescriptorW failed: " + log::win32_error(GetLastError()));
        return false;
    }
    for (DWORD i = 0; i < kMaxInstances; ++i) {
        auto inst = std::make_unique<Instance>();
        inst->event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!inst->event) {
            log::error("CreateEvent failed: " + log::win32_error(GetLastError()));
            break;
        }
        inst->rbuf.resize(kReadChunk);
        if (!create_pipe_handle(*inst, i == 0)) {
            CloseHandle(inst->event);
            // Additional instances need FILE_CREATE_PIPE_INSTANCE (SYSTEM/Administrators only);
            // a non-elevated --console run therefore gets a single instance.
            if (i > 0) log::warn("serving with " + std::to_string(i) + " pipe instance(s)");
            break;
        }
        instances_.push_back(std::move(inst));
    }
    if (instances_.empty()) return false;
    log::info("named pipe ready with " + std::to_string(instances_.size()) + " instance(s)");
    return true;
}

void PipeServer::recycle(Instance& inst) {
    if (inst.pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(inst.pipe);
        inst.pipe = INVALID_HANDLE_VALUE;
    }
    inst.io_pending = false;
    // If no other instance is alive, re-claim the name with FILE_FLAG_FIRST_PIPE_INSTANCE so a
    // squatter that grabbed it in the meantime is detected rather than joined.
    bool others = false;
    for (const auto& p : instances_) {
        if (p.get() != &inst && p->pipe != INVALID_HANDLE_VALUE) others = true;
    }
    if (!create_pipe_handle(inst, !others)) inst.state = State::Dead;
}

void PipeServer::begin_connect(Instance& inst) {
    ResetEvent(inst.event);
    inst.ov = OVERLAPPED{};
    inst.ov.hEvent = inst.event;
    if (ConnectNamedPipe(inst.pipe, &inst.ov)) {
        on_connected(inst);  // not expected in overlapped mode, but harmless
        return;
    }
    const DWORD err = GetLastError();
    switch (err) {
        case ERROR_IO_PENDING:
            inst.state = State::Connecting;
            inst.io_pending = true;
            return;
        case ERROR_PIPE_CONNECTED:  // client connected between CreateNamedPipe and ConnectNamedPipe
            on_connected(inst);
            return;
        case ERROR_NO_DATA:  // client connected and already closed its end
            DisconnectNamedPipe(inst.pipe);
            inst.state = State::Listening;
            return;
        default:
            log::warn("ConnectNamedPipe failed: " + log::win32_error(err));
            recycle(inst);
            return;
    }
}

void PipeServer::on_connected(Instance& inst) {
    inst.state = State::Connected;
    inst.io_pending = false;
    inst.client_pid = 0;
    if (!GetNamedPipeClientProcessId(inst.pipe, &inst.client_pid)) inst.client_pid = 0;
    inst.ctx = protocol::ClientContext{};
    inst.ctx.pid = inst.client_pid;
    inst.ctx.capabilities = protocol::cap_bit(protocol::Capability::Read);
    inst.identified = false;
    inst.served = 0;
    inst.decoder.reset();
    inst.queued.clear();
    inst.frame_error = framing::FrameError::None;
    inst.close_after_write = false;
    inst.partial = false;
    inst.wbuf.clear();
    inst.last_activity = Clock::now();
    inst.counted = true;
    counters_.clients.fetch_add(1, std::memory_order_relaxed);
    log::info("client connected " + pid_str(inst.client_pid));
}

// Records the client's user SID (audit) and whether it is an elevated administrator. Must be
// called after data has been read from the pipe (ImpersonateNamedPipeClient requirement).
void PipeServer::identify_client(Instance& inst) {
    inst.identified = true;
    std::string sid = "unknown";
    bool admin = false;
    std::uint32_t session_id = protocol::kUnknownSession;
    if (ImpersonateNamedPipeClient(inst.pipe)) {
        HANDLE token = nullptr;
        if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token)) {
            alignas(std::max_align_t) BYTE buf[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
            DWORD len = 0;
            if (GetTokenInformation(token, TokenUser, buf, sizeof(buf), &len)) {
                const auto* tu = reinterpret_cast<const TOKEN_USER*>(buf);
                LPWSTR str = nullptr;
                if (ConvertSidToStringSidW(tu->User.Sid, &str)) {
                    sid = log::narrow(str);
                    LocalFree(str);
                }
            }
            // admin = Administrators group *enabled* in the client token (CheckTokenMembership ignores
            // deny-only groups, so a UAC-filtered token fails) AND the token is elevated.
            BYTE admins[SECURITY_MAX_SID_SIZE];
            DWORD admins_size = sizeof(admins);
            BOOL member = FALSE;
            TOKEN_ELEVATION elevation{};
            DWORD elevation_len = 0;
            if (CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, admins, &admins_size) &&
                CheckTokenMembership(token, admins, &member) && member &&
                GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &elevation_len) &&
                elevation.TokenIsElevated) {
                admin = true;
            }
            // Session of the *token* (not of the process id, which a client could have recycled, and
            // never a value from the request): used by reportForeground.
            DWORD session = 0;
            DWORD session_len = 0;
            if (GetTokenInformation(token, TokenSessionId, &session, sizeof(session), &session_len))
                session_id = session;
            CloseHandle(token);
        }
        if (!RevertToSelf()) {
            // Continuing to run under the client's identity would be a security bug.
            log::error("RevertToSelf failed; terminating");
            log::shutdown();
            TerminateProcess(GetCurrentProcess(), 1);
        }
    }
    // Every client allowed by the pipe ACL gets `read`; `admin` (setMode, rollbackAll, disablePolicy,
    // approve, exportDiagnostics) only elevated Administrators. If the token cannot be read (e.g. an
    // anonymous-level client), the client keeps `read` only and an unknown session.
    if (admin) inst.ctx.capabilities |= protocol::cap_bit(protocol::Capability::Admin);
    inst.ctx.session_id = session_id;
    log::info("client identified " + pid_str(inst.client_pid) + " sid=" + sid + " admin=" + (admin ? "yes" : "no") +
              " session=" + (session_id == protocol::kUnknownSession ? std::string("unknown") : std::to_string(session_id)));
}

void PipeServer::begin_read(Instance& inst) {
    ResetEvent(inst.event);
    inst.ov = OVERLAPPED{};
    inst.ov.hEvent = inst.event;
    if (ReadFile(inst.pipe, inst.rbuf.data(), static_cast<DWORD>(inst.rbuf.size()), nullptr, &inst.ov) ||
        GetLastError() == ERROR_IO_PENDING) {
        inst.state = State::Reading;
        inst.io_pending = true;  // completion (sync or async) is observed via the event
        return;
    }
    disconnect(inst);  // ERROR_BROKEN_PIPE etc.
}

void PipeServer::begin_drain_read(Instance& inst) {
    ResetEvent(inst.event);
    inst.ov = OVERLAPPED{};
    inst.ov.hEvent = inst.event;
    if (ReadFile(inst.pipe, inst.rbuf.data(), static_cast<DWORD>(inst.rbuf.size()), nullptr, &inst.ov) ||
        GetLastError() == ERROR_IO_PENDING) {
        inst.state = State::Draining;
        inst.io_pending = true;
        return;
    }
    disconnect(inst);
}

void PipeServer::begin_write(Instance& inst, const std::string& payload) {
    if (!framing::encode(payload, inst.wbuf, framing::kMaxResponsePayload)) {
        log::error("response could not be framed; closing connection");
        disconnect(inst);
        return;
    }
    ResetEvent(inst.event);
    inst.ov = OVERLAPPED{};
    inst.ov.hEvent = inst.event;
    if (WriteFile(inst.pipe, inst.wbuf.data(), static_cast<DWORD>(inst.wbuf.size()), nullptr, &inst.ov) ||
        GetLastError() == ERROR_IO_PENDING) {
        inst.state = State::Writing;
        inst.io_pending = true;
        inst.write_started = Clock::now();
        return;
    }
    disconnect(inst);
}

void PipeServer::complete_io(Instance& inst) {
    DWORD n = 0;
    const BOOL ok = GetOverlappedResult(inst.pipe, &inst.ov, &n, FALSE);
    const DWORD err = ok ? ERROR_SUCCESS : GetLastError();
    inst.io_pending = false;
    const auto now = Clock::now();

    switch (inst.state) {
        case State::Connecting:
            if (ok || err == ERROR_PIPE_CONNECTED) {
                on_connected(inst);
            } else {
                log::debug("connect completion failed: " + log::win32_error(err));
                recycle(inst);
            }
            return;

        case State::Reading: {
            if (!ok) {
                disconnect(inst);  // ERROR_BROKEN_PIPE: client closed
                return;
            }
            inst.state = State::Connected;
            if (n == 0) return;
            inst.last_activity = now;
            if (!inst.identified) identify_client(inst);
            std::vector<std::string> frames;
            inst.frame_error = inst.decoder.feed(inst.rbuf.data(), n, frames);
            for (auto& f : frames) inst.queued.push_back(std::move(f));
            if (inst.decoder.in_progress()) {
                // A new partial frame started in this chunk: its 5 s budget starts now.
                if (!inst.partial || !frames.empty()) inst.partial_since = now;
                inst.partial = true;
            } else {
                inst.partial = false;
            }
            return;
        }

        case State::Writing:
            if (!ok || n != inst.wbuf.size()) {
                disconnect(inst);
                return;
            }
            inst.last_activity = now;
            inst.wbuf.clear();
            if (inst.close_after_write) {
                inst.state = State::Draining;
                inst.drain_until = now + kDrainLinger;
            } else {
                inst.state = State::Connected;
            }
            return;

        case State::Draining:
            if (!ok) disconnect(inst);  // client closed (or error): done
            // else: data discarded; drive() issues the next drain read
            return;

        default:
            return;
    }
}

void PipeServer::process_one(Instance& inst) {
    const std::string payload = std::move(inst.queued.front());
    inst.queued.pop_front();
    counters_.requests.fetch_add(1, std::memory_order_relaxed);
    ++inst.served;
    const auto out = dispatcher_.handle(payload, inst.ctx, framing::kMaxResponsePayload);
    // Only the registered command name is ever logged, never arguments or raw input.
    const std::string cmd = out.log_cmd.empty() ? std::string("<unregistered>") : out.log_cmd;
    if (out.is_error) {
        counters_.errors.fetch_add(1, std::memory_order_relaxed);
        log::warn("request failed " + pid_str(inst.client_pid) + " cmd=" + cmd +
                  " code=" + std::string(protocol::error_code_name(out.code)));
    } else {
        log::debug("request ok " + pid_str(inst.client_pid) + " cmd=" + cmd);
    }
    begin_write(inst, out.response);
}

void PipeServer::send_error_and_close(Instance& inst, protocol::ErrorCode code, const char* message) {
    counters_.errors.fetch_add(1, std::memory_order_relaxed);
    log::warn("closing connection " + pid_str(inst.client_pid) + " code=" + std::string(protocol::error_code_name(code)));
    inst.queued.clear();
    inst.frame_error = framing::FrameError::None;
    inst.close_after_write = true;
    begin_write(inst, json::serialize(protocol::make_error(0, code, message)));
}

void PipeServer::drive(Instance& inst) {
    for (int i = 0; i < kMaxStepsPerWake && !inst.io_pending; ++i) {
        switch (inst.state) {
            case State::Listening:
                begin_connect(inst);
                break;
            case State::Connected:
                if (!inst.queued.empty()) {
                    process_one(inst);
                } else if (inst.frame_error == framing::FrameError::TooLarge) {
                    send_error_and_close(inst, protocol::ErrorCode::TooLarge, "request frame exceeds 65536 bytes");
                } else if (inst.frame_error == framing::FrameError::BadFrame) {
                    send_error_and_close(inst, protocol::ErrorCode::BadFrame, "zero-length frame");
                } else {
                    begin_read(inst);
                }
                break;
            case State::Draining:
                if (Clock::now() >= inst.drain_until) {
                    disconnect(inst);
                } else {
                    begin_drain_read(inst);
                }
                break;
            default:
                return;  // Dead, or an I/O state (which always has io_pending set)
        }
    }
}

bool PipeServer::cancel_and_wait(Instance& inst, DWORD wait_ms) {
    if (!inst.io_pending) return true;
    CancelIoEx(inst.pipe, &inst.ov);  // ERROR_NOT_FOUND just means it already completed
    DWORD n = 0;
    if (GetOverlappedResultEx(inst.pipe, &inst.ov, &n, wait_ms, FALSE)) {
        inst.io_pending = false;
        return true;
    }
    const DWORD err = GetLastError();
    if (err == WAIT_TIMEOUT || err == WAIT_IO_COMPLETION) return false;
    inst.io_pending = false;  // completed with an error (typically ERROR_OPERATION_ABORTED)
    return true;
}

void PipeServer::disconnect(Instance& inst) {
    if (inst.io_pending && !cancel_and_wait(inst, kCancelWaitMs)) {
        // The kernel may still write into this instance's buffers: never free or reuse them.
        log::error("pipe I/O could not be cancelled; retiring instance");
        CloseHandle(inst.pipe);
        inst.pipe = INVALID_HANDLE_VALUE;
        inst.leaked = true;
        inst.state = State::Dead;
        if (inst.counted) {
            counters_.clients.fetch_sub(1, std::memory_order_relaxed);
            inst.counted = false;
        }
        return;
    }
    DisconnectNamedPipe(inst.pipe);
    if (inst.counted) {
        counters_.clients.fetch_sub(1, std::memory_order_relaxed);
        inst.counted = false;
        log::info("client disconnected " + pid_str(inst.client_pid) + " requests=" + std::to_string(inst.served));
    }
    inst.decoder.reset();
    inst.queued.clear();
    inst.frame_error = framing::FrameError::None;
    inst.close_after_write = false;
    inst.partial = false;
    inst.wbuf.clear();
    inst.wbuf.shrink_to_fit();
    inst.client_pid = 0;
    inst.state = State::Listening;
}

PipeServer::Clock::time_point PipeServer::deadline(const Instance& inst) const {
    switch (inst.state) {
        case State::Reading: {
            auto d = inst.last_activity + kIdleTimeout;
            if (inst.partial) d = std::min(d, inst.partial_since + kRequestTimeout);
            return d;
        }
        case State::Writing: return inst.write_started + kWriteTimeout;
        case State::Draining: return inst.drain_until;
        default: return Clock::time_point::max();
    }
}

void PipeServer::check_deadlines(Instance& inst, Clock::time_point now) {
    if (!inst.io_pending || now < deadline(inst)) return;
    switch (inst.state) {
        case State::Reading:
            if (inst.partial) {
                if (!cancel_and_wait(inst, kCancelWaitMs)) {
                    disconnect(inst);
                    return;
                }
                inst.state = State::Connected;
                send_error_and_close(inst, protocol::ErrorCode::Timeout, "request not completed within 5 s");
            } else {
                log::info("idle timeout " + pid_str(inst.client_pid));
                disconnect(inst);
            }
            return;
        case State::Writing:
            log::warn("response write timed out " + pid_str(inst.client_pid));
            disconnect(inst);
            return;
        case State::Draining:
            disconnect(inst);
            return;
        default:
            return;
    }
}

bool PipeServer::serve(HANDLE stop_event) {
    for (auto& p : instances_) drive(*p);
    bool ok = true;
    for (;;) {
        HANDLE handles[1 + kMaxInstances];
        DWORD count = 0;
        handles[count++] = stop_event;
        auto now = Clock::now();
        auto next = Clock::time_point::max();
        bool runnable = false;
        bool any_alive = false;
        for (auto& p : instances_) {
            if (p->state == State::Dead) continue;
            any_alive = true;
            if (p->io_pending) {
                handles[count++] = p->event;
                next = std::min(next, deadline(*p));
            } else {
                runnable = true;  // needs another drive() pass
            }
        }
        if (!any_alive) {
            log::error("all pipe instances failed; stopping the IPC server");
            ok = false;
            break;
        }
        DWORD timeout = INFINITE;
        if (runnable) {
            timeout = 0;
        } else if (next != Clock::time_point::max()) {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(next - now).count();
            timeout = ms <= 0 ? 0 : static_cast<DWORD>(std::min<long long>(ms + 1, 120000));
        }
        const DWORD r = WaitForMultipleObjects(count, handles, FALSE, timeout);
        if (r == WAIT_OBJECT_0) break;  // stop requested
        if (r == WAIT_FAILED) {
            log::error("WaitForMultipleObjects failed: " + log::win32_error(GetLastError()));
            ok = false;
            break;
        }
        now = Clock::now();
        for (auto& p : instances_) {
            Instance& inst = *p;
            if (inst.state == State::Dead) continue;
            if (inst.io_pending && HasOverlappedIoCompleted(&inst.ov)) complete_io(inst);
            if (inst.io_pending) check_deadlines(inst, now);
            drive(inst);
        }
    }
    shutdown_all();
    return ok;
}

void PipeServer::shutdown_all() {
    if (instances_.empty()) return;
    for (auto& p : instances_) {
        if (p->io_pending && p->pipe != INVALID_HANDLE_VALUE) CancelIoEx(p->pipe, &p->ov);
    }
    const auto until = Clock::now() + std::chrono::milliseconds(kCancelWaitMs);
    for (auto& p : instances_) {
        Instance& inst = *p;
        if (!inst.io_pending || inst.pipe == INVALID_HANDLE_VALUE) continue;
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - Clock::now()).count();
        DWORD n = 0;
        if (GetOverlappedResultEx(inst.pipe, &inst.ov, &n, left > 0 ? static_cast<DWORD>(left) : 0, FALSE)) {
            inst.io_pending = false;
        } else {
            const DWORD err = GetLastError();
            if (err == WAIT_TIMEOUT || err == WAIT_IO_COMPLETION)
                inst.leaked = true;
            else
                inst.io_pending = false;
        }
    }
    for (auto& p : instances_) {
        Instance& inst = *p;
        if (inst.pipe != INVALID_HANDLE_VALUE) {
            if (inst.counted) DisconnectNamedPipe(inst.pipe);
            CloseHandle(inst.pipe);
            inst.pipe = INVALID_HANDLE_VALUE;
        }
        if (inst.counted) {
            counters_.clients.fetch_sub(1, std::memory_order_relaxed);
            inst.counted = false;
        }
        if (!inst.leaked && inst.event) {
            CloseHandle(inst.event);
            inst.event = nullptr;
        }
        inst.state = State::Dead;
        if (inst.leaked) {
            // Pending kernel I/O may still reference the OVERLAPPED and buffers: keep them alive.
            (void)p.release();
        }
    }
    instances_.clear();
    log::info("named pipe server stopped");
}

}  // namespace opensweep::win
