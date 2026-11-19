#include "etw.hpp"

// clang-format off
#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>
#include <psapi.h>
// clang-format on

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <utility>

#include "log.hpp"
#include "process_info.hpp"

// Documented constants mirrored for SDK / MinGW headers that lack them.
#ifndef EVENT_TRACE_SYSTEM_LOGGER_MODE
#define EVENT_TRACE_SYSTEM_LOGGER_MODE 0x02000000
#endif
#ifndef EVENT_TRACE_REAL_TIME_MODE
#define EVENT_TRACE_REAL_TIME_MODE 0x00000100
#endif
#ifndef PROCESS_TRACE_MODE_REAL_TIME
#define PROCESS_TRACE_MODE_REAL_TIME 0x00000100
#endif
#ifndef PROCESS_TRACE_MODE_RAW_TIMESTAMP
#define PROCESS_TRACE_MODE_RAW_TIMESTAMP 0x00001000
#endif
#ifndef PROCESS_TRACE_MODE_EVENT_RECORD
#define PROCESS_TRACE_MODE_EVENT_RECORD 0x10000000
#endif
#ifndef EVENT_HEADER_FLAG_32_BIT_HEADER
#define EVENT_HEADER_FLAG_32_BIT_HEADER 0x0020
#endif
#ifndef EVENT_HEADER_FLAG_64_BIT_HEADER
#define EVENT_HEADER_FLAG_64_BIT_HEADER 0x0040
#endif
#ifndef EVENT_TRACE_CONTROL_QUERY
#define EVENT_TRACE_CONTROL_QUERY 0
#endif
#ifndef EVENT_TRACE_CONTROL_STOP
#define EVENT_TRACE_CONTROL_STOP 1
#endif
#ifndef WNODE_FLAG_TRACED_GUID
#define WNODE_FLAG_TRACED_GUID 0x00020000
#endif
#ifndef INVALID_PROCESSTRACE_HANDLE
#define INVALID_PROCESSTRACE_HANDLE (static_cast<TRACEHANDLE>(INVALID_HANDLE_VALUE))
#endif
#ifndef ERROR_CTX_CLOSE_PENDING
#define ERROR_CTX_CLOSE_PENDING 7007L
#endif
#ifndef ERROR_WMI_INSTANCE_NOT_FOUND
#define ERROR_WMI_INSTANCE_NOT_FOUND 4201L
#endif

namespace opensweep::win {

namespace {

// SystemTraceControlGuid (evntrace.h) and the classic kernel event class GUIDs (MSDN "MSNT_SystemTrace"
// classes). Defined locally so no INITGUID / uuid.lib dependency is needed.
constexpr GUID kSystemTraceControlGuid = {0x9e814aad, 0x3204, 0x11d2, {0x9a, 0x82, 0x00, 0x60, 0x08, 0xa8, 0x69, 0x39}};
// Private session GUID, used only if this Windows build refuses SystemTraceControlGuid for a
// non-"NT Kernel Logger" system logger session.
constexpr GUID kOpenSweepSessionGuid = {0x6f2b5c3e, 0x8d1a, 0x4b7e, {0x9c, 0x42, 0x1e, 0x5a, 0x7d, 0x30, 0xc9, 0x81}};
constexpr GUID kPerfInfoGuid = {0xce1dbfb4, 0x137e, 0x4da6, {0x87, 0xb0, 0x3f, 0x59, 0xaa, 0x10, 0x2c, 0xbc}};
constexpr GUID kImageLoadGuid = {0x2cb15d1d, 0x5fc1, 0x11d2, {0xab, 0xe1, 0x00, 0xa0, 0xc9, 0x11, 0xf5, 0x18}};
constexpr GUID kProcessGuid = {0x3d6fa8d0, 0xfe05, 0x11d0, {0x9d, 0xda, 0x00, 0xc0, 0x4f, 0xd7, 0xba, 0x7c}};
constexpr GUID kThreadGuid = {0x3d6fa8d1, 0xfe05, 0x11d0, {0x9d, 0xda, 0x00, 0xc0, 0x4f, 0xd7, 0xba, 0x7c}};
constexpr GUID kPageFaultGuid = {0x3d6fa8d3, 0xfe05, 0x11d0, {0x9d, 0xda, 0x00, 0xc0, 0x4f, 0xd7, 0xba, 0x7c}};
constexpr GUID kDiskIoGuid = {0x3d6fa8d4, 0xfe05, 0x11d0, {0x9d, 0xda, 0x00, 0xc0, 0x4f, 0xd7, 0xba, 0x7c}};

// Opcodes (documented event types of the classes above).
constexpr UCHAR kOpThreadedDpc = 66, kOpIsr = 67, kOpDpc = 68, kOpTimerDpc = 69, kOpIsrMsi = 50;
constexpr UCHAR kOpStart = 1, kOpEnd = 2, kOpDcStart = 3;  // Process / Thread
constexpr UCHAR kOpImageLoad = 10, kOpImageUnload = 2, kOpImageDcStart = 3;
constexpr UCHAR kOpDiskRead = 10, kOpDiskWrite = 11;
constexpr UCHAR kOpHardFault = 32;

constexpr std::size_t kMaxNameChars = 260;

bool same_guid(const GUID& a, const GUID& b) noexcept { return std::memcmp(&a, &b, sizeof(GUID)) == 0; }

std::int64_t unix_now_ms() {
    FILETIME ft{};
    GetSystemTimePreciseAsFileTime(&ft);
    return procinfo::filetime_to_unix_ms(procinfo::filetime_u64(ft));
}

// Bounds-checked reader over an event payload.
struct Reader {
    const unsigned char* d = nullptr;
    std::size_t n = 0;
    std::size_t ps = 8;  // pointer size of the event

    bool fits(std::size_t off, std::size_t len) const noexcept { return d && off <= n && n - off >= len; }
    bool u8(std::size_t off, std::uint8_t& v) const noexcept {
        if (!fits(off, 1)) return false;
        v = d[off];
        return true;
    }
    bool u32(std::size_t off, std::uint32_t& v) const noexcept {
        if (!fits(off, 4)) return false;
        std::memcpy(&v, d + off, 4);
        return true;
    }
    bool u64(std::size_t off, std::uint64_t& v) const noexcept {
        if (!fits(off, 8)) return false;
        std::memcpy(&v, d + off, 8);
        return true;
    }
    bool ptr(std::size_t off, std::uint64_t& v) const noexcept {
        if (ps == 8) return u64(off, v);
        std::uint32_t x = 0;
        if (!u32(off, x)) return false;
        v = x;
        return true;
    }
    // NUL-terminated UTF-16 string; false if no terminator within bounds / max chars.
    bool wstr(std::size_t off, std::wstring& out) const {
        out.clear();
        for (std::size_t i = 0; i < kMaxNameChars; ++i) {
            if (!fits(off + 2 * i, 2)) return false;
            wchar_t c = 0;
            std::memcpy(&c, d + off + 2 * i, 2);
            if (c == 0) return true;
            out.push_back(c);
        }
        return false;
    }
    // NUL-terminated ANSI string (process image names are ASCII in practice).
    bool astr(std::size_t off, std::string& out) const {
        out.clear();
        for (std::size_t i = 0; i < kMaxNameChars; ++i) {
            std::uint8_t c = 0;
            if (!u8(off + i, c)) return false;
            if (c == 0) return true;
            out.push_back(c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '?');
        }
        return false;
    }
};

bool is_kernel_address(std::uint64_t a, std::size_t ps) noexcept {
    return ps == 8 ? a >= 0xFFFF800000000000ull : a >= 0x80000000ull;
}

bool enable_privilege(const wchar_t* name) {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) return false;
    LUID luid{};
    bool ok = false;
    if (LookupPrivilegeValueW(nullptr, name, &luid)) {
        TOKEN_PRIVILEGES tp{};
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Luid = luid;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        ok = AdjustTokenPrivileges(tok, FALSE, &tp, sizeof(tp), nullptr, nullptr) && GetLastError() == ERROR_SUCCESS;
    }
    CloseHandle(tok);
    return ok;
}

// EVENT_TRACE_PROPERTIES + room for the logger name (and a log file name, unused).
struct Props {
    std::vector<unsigned long long> buf;  // 8-byte aligned storage
    EVENT_TRACE_PROPERTIES* p = nullptr;
    explicit Props(const std::wstring& name) {
        const std::size_t bytes = sizeof(EVENT_TRACE_PROPERTIES) + (name.size() + 1) * sizeof(wchar_t) +
                                  (kMaxNameChars + 1) * sizeof(wchar_t);
        buf.assign((bytes + 7) / 8, 0);
        p = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(buf.data());
        p->Wnode.BufferSize = static_cast<ULONG>(buf.size() * 8);
        p->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
        p->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        p->LogFileNameOffset = static_cast<ULONG>(sizeof(EVENT_TRACE_PROPERTIES) + (name.size() + 1) * sizeof(wchar_t));
    }
};

std::uint64_t thread_cpu_us(HANDLE h) {
    if (!h) return 0;
    FILETIME c{}, e{}, k{}, u{};
    if (!GetThreadTimes(h, &c, &e, &k, &u)) return 0;
    return (procinfo::filetime_u64(k) + procinfo::filetime_u64(u)) / 10;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
struct KernelSession::Shared {
    std::mutex mu;  // guards stats
    kstats::KernelStats stats;
    std::atomic<bool> accepting{false};
    // Clock mapping, written before a consumer thread is created, read by its callbacks.
    double qpc_freq = 1.0;
    std::int64_t base_qpc = 0;
    std::int64_t base_unix_ms = 0;
    std::atomic<HANDLE> thread{nullptr};       // consumer thread (CPU-time measurement)
    std::atomic<std::uint64_t> raw_events{0};
    std::atomic<std::uint64_t> kernel_images{0};
    std::atomic<std::uint64_t> exceptions{0};

    explicit Shared(kstats::Config c) : stats(c) {}

    std::int64_t to_ms(std::int64_t qpc) const noexcept {
        return base_unix_ms + static_cast<std::int64_t>(static_cast<double>(qpc - base_qpc) * 1000.0 / qpc_freq);
    }
};

namespace {

void decode(KernelSession::Shared& sh, const EVENT_RECORD& r) {
    const EVENT_HEADER& h = r.EventHeader;
    const UCHAR op = h.EventDescriptor.Opcode;
    const UCHAR ver = h.EventDescriptor.Version;
    Reader rd;
    rd.d = static_cast<const unsigned char*>(r.UserData);
    rd.n = rd.d ? static_cast<std::size_t>(r.UserDataLength) : std::size_t{0};
    rd.ps = (h.Flags & EVENT_HEADER_FLAG_64_BIT_HEADER) ? 8 : (h.Flags & EVENT_HEADER_FLAG_32_BIT_HEADER) ? 4 : sizeof(void*);
    const std::size_t ps = rd.ps;
    sh.raw_events.fetch_add(1, std::memory_order_relaxed);
    auto malformed = [&] {
        std::lock_guard<std::mutex> lock(sh.mu);
        sh.stats.on_malformed();
    };

    if (same_guid(h.ProviderId, kPerfInfoGuid)) {
        kstats::Routine kind = kstats::Routine::Dpc;
        if (op == kOpDpc || op == kOpThreadedDpc || op == kOpTimerDpc)
            kind = kstats::Routine::Dpc;
        else if (op == kOpIsr || op == kOpIsrMsi)
            kind = kstats::Routine::Isr;
        else
            return;
        // { InitialTime: uint64 (entry timestamp, session clock), Routine: pointer, ... }
        std::uint64_t initial = 0, routine = 0;
        if (!rd.u64(0, initial) || !rd.ptr(8, routine)) return malformed();
        const auto ts = static_cast<std::uint64_t>(h.TimeStamp.QuadPart);
        if (initial == 0 || initial > ts) return malformed();
        const double us = static_cast<double>(ts - initial) * 1e6 / sh.qpc_freq;
        std::lock_guard<std::mutex> lock(sh.mu);
        sh.stats.on_dpc_isr(kind, routine, us, sh.to_ms(h.TimeStamp.QuadPart));
        return;
    }
    if (same_guid(h.ProviderId, kDiskIoGuid)) {
        if (op != kOpDiskRead && op != kOpDiskWrite) return;
        if (ver < 2) return malformed();
        // { DiskNumber u32, IrpFlags u32, TransferSize u32, Reserved u32, ByteOffset u64, FileObject ptr,
        //   Irp ptr, HighResResponseTime u64 (QPC ticks), IssuingThreadId u32 (v3+) }
        std::uint32_t size = 0, tid = 0;
        std::uint64_t resp = 0;
        if (!rd.u32(8, size) || !rd.u64(24 + 2 * ps, resp)) return malformed();
        if (!rd.u32(32 + 2 * ps, tid)) tid = 0;
        const double ms = static_cast<double>(resp) * 1000.0 / sh.qpc_freq;
        std::lock_guard<std::mutex> lock(sh.mu);
        sh.stats.on_disk_io(tid, h.ProcessId, size, op == kOpDiskWrite, ms, sh.to_ms(h.TimeStamp.QuadPart));
        return;
    }
    if (same_guid(h.ProviderId, kPageFaultGuid)) {
        if (op != kOpHardFault) return;
        // { InitialTime u64, ReadOffset u64, VirtualAddress ptr, FileObject ptr, TThreadId u32, ByteCount u32 }
        std::uint32_t tid = 0;
        if (!rd.u32(16 + 2 * ps, tid)) return malformed();
        std::lock_guard<std::mutex> lock(sh.mu);
        sh.stats.on_hard_fault(tid, h.ProcessId, sh.to_ms(h.TimeStamp.QuadPart));
        return;
    }
    if (same_guid(h.ProviderId, kThreadGuid)) {
        if (op != kOpStart && op != kOpDcStart && op != kOpEnd) return;
        // { ProcessId u32, TThreadId u32, ... } (v1+)
        std::uint32_t pid = 0, tid = 0;
        if (ver < 1 || !rd.u32(0, pid) || !rd.u32(4, tid)) return malformed();
        std::lock_guard<std::mutex> lock(sh.mu);
        if (op == kOpEnd)
            sh.stats.on_thread_end(tid);
        else
            sh.stats.on_thread_start(tid, pid);
        return;
    }
    if (same_guid(h.ProviderId, kImageLoadGuid)) {
        if (op != kOpImageLoad && op != kOpImageDcStart && op != kOpImageUnload) return;
        // v2/v3: { ImageBase ptr, ImageSize ptr, ProcessId u32, ImageCheckSum u32, TimeDateStamp u32,
        //          Reserved0 / SignatureLevel+Type u32, DefaultBase ptr, Reserved1..4 u32, FileName wstr }
        if (ver < 2) return malformed();
        std::uint64_t base = 0, size = 0;
        std::uint32_t pid = 0;
        if (!rd.ptr(0, base) || !rd.ptr(ps, size) || !rd.u32(2 * ps, pid)) return malformed();
        if (pid != 0 && !is_kernel_address(base, ps)) return;  // user-mode module: not a driver
        if (op == kOpImageUnload) {
            std::lock_guard<std::mutex> lock(sh.mu);
            sh.stats.on_image_unload(base);
            return;
        }
        std::wstring path;
        if (!rd.wstr(3 * ps + 32, path)) return malformed();
        const std::string name = log::narrow(procinfo::file_name_of(path));
        sh.kernel_images.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(sh.mu);
        sh.stats.on_image_load(base, size, name);
        return;
    }
    if (same_guid(h.ProviderId, kProcessGuid)) {
        if (op != kOpStart && op != kOpDcStart && op != kOpEnd) return;
        // v3/v4: { UniqueProcessKey ptr, ProcessId u32, ParentId u32, SessionId u32, ExitStatus i32,
        //          DirectoryTableBase ptr, [v4+: Flags u32], UserSID (TOKEN_USER + SID), ImageFileName ansi, ... }
        if (ver < 3) return malformed();
        std::uint32_t pid = 0;
        if (!rd.u32(ps, pid)) return malformed();
        if (op == kOpEnd) {
            std::lock_guard<std::mutex> lock(sh.mu);
            sh.stats.on_process_end(pid);
            return;
        }
        std::size_t off = 2 * ps + (ver >= 4 ? 20 : 16);
        std::uint32_t sid_head = 0;
        std::string name;
        bool ok = rd.u32(off, sid_head);
        if (ok) {
            if (sid_head == 0) {
                off += 4;
            } else {
                std::uint8_t sub = 0;
                ok = rd.u8(off + 2 * ps + 1, sub) && sub <= 15;  // SID: Revision, SubAuthorityCount, ...
                off += 2 * ps + 8 + 4 * static_cast<std::size_t>(sub);
            }
        }
        ok = ok && rd.astr(off, name) && !name.empty();
        if (!ok) name.clear();  // unknown layout: the name resolver fills it in
        std::lock_guard<std::mutex> lock(sh.mu);
        if (!ok) sh.stats.on_malformed();
        sh.stats.on_process_start(pid, name);
        return;
    }
}

void WINAPI on_event(PEVENT_RECORD r) {
    if (!r) return;
    auto* sh = static_cast<KernelSession::Shared*>(r->UserContext);
    if (!sh || !sh->accepting.load(std::memory_order_relaxed)) return;
    try {
        decode(*sh, *r);
    } catch (...) {
        sh->exceptions.fetch_add(1, std::memory_order_relaxed);  // never let an exception cross ETW
    }
}

}  // namespace

// ---------------------------------------------------------------------------------------------
struct KernelSession::Consumer {
    struct Sync {
        HANDLE done = nullptr;
        std::atomic<unsigned long> status{ERROR_SUCCESS};
        Sync() : done(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
        ~Sync() {
            if (done) CloseHandle(done);
        }
        Sync(const Sync&) = delete;
        Sync& operator=(const Sync&) = delete;
    };
    TRACEHANDLE trace = INVALID_PROCESSTRACE_HANDLE;
    std::shared_ptr<Sync> sync = std::make_shared<Sync>();
    std::thread thread;
    bool finished() const { return !sync->done || WaitForSingleObject(sync->done, 0) == WAIT_OBJECT_0; }
};

KernelSession::KernelSession() : KernelSession(Options{}) {}

KernelSession::KernelSession(Options o) : opt_(std::move(o)), protect_(opt_.protection) {
    kstats::Config c;
    c.logical_processors = opt_.logical_processors > 0 ? opt_.logical_processors : 1;
    sh_ = std::make_shared<Shared>(c);
    if (opt_.resolve_names) {
        sh_->stats.set_name_resolver([](std::uint32_t pid) -> std::string {
            if (pid == 0 || pid == 4) return {};
            procinfo::ProcessHandle h = procinfo::open_process(pid, PROCESS_QUERY_LIMITED_INFORMATION);
            std::wstring path;
            if (!h || !procinfo::image_path(h.get(), path)) return {};
            return log::narrow(procinfo::file_name_of(path));
        });
    }
    reason_ = "not started";
}

KernelSession::~KernelSession() { stop(); }

bool KernelSession::session_exists(const std::wstring& name) {
    Props props(name);
    return ControlTraceW(0, name.c_str(), props.p, EVENT_TRACE_CONTROL_QUERY) == ERROR_SUCCESS;
}

bool KernelSession::stop_session(const std::wstring& name, unsigned long* error) {
    Props props(name);
    const ULONG err = ControlTraceW(0, name.c_str(), props.p, EVENT_TRACE_CONTROL_STOP);
    if (error) *error = err;
    return err == ERROR_SUCCESS;
}

bool KernelSession::start_level_locked(int level) {
    level_ = level;
    flags_ = kstats::flags_for_level(level);
    if (flags_ == 0) {
        running_ = false;
        return false;
    }
    static std::once_flag priv_once;
    std::call_once(priv_once, [] {
        // Kernel (SystemTraceProvider) sessions: enable SeSystemProfilePrivilege when the token has it.
        if (!enable_privilege(L"SeSystemProfilePrivilege"))
            log::info("etw: SeSystemProfilePrivilege not enabled (not in the token); continuing");
    });

    // Crash recovery: a session with our name left over from a previous run is stopped first.
    unsigned long stale_err = 0;
    if (stop_session(opt_.session_name, &stale_err)) log::warn("etw: stopped a stale OpenSweepCoreKernel session");

    const std::wstring& name = opt_.session_name;
    auto try_start = [&](const GUID& guid, TRACEHANDLE& session) -> ULONG {
        Props props(name);
        props.p->Wnode.Guid = guid;
        props.p->LogFileNameOffset = 0;  // real-time only: no log file
        props.p->Wnode.ClientContext = 1;  // QueryPerformanceCounter timestamps
        props.p->BufferSize = opt_.buffer_kb;
        props.p->MinimumBuffers = opt_.min_buffers;
        props.p->MaximumBuffers = opt_.max_buffers;
        props.p->FlushTimer = opt_.flush_seconds;
        props.p->LogFileMode = EVENT_TRACE_REAL_TIME_MODE | EVENT_TRACE_SYSTEM_LOGGER_MODE;
        props.p->EnableFlags = flags_;
        return StartTraceW(&session, name.c_str(), props.p);
    };
    TRACEHANDLE session = 0;
    const char* guid_used = "SystemTraceControlGuid";
    ULONG err = try_start(kSystemTraceControlGuid, session);
    if (err == ERROR_ALREADY_EXISTS) {
        stop_session(name);
        err = try_start(kSystemTraceControlGuid, session);
    }
    if (err == ERROR_INVALID_PARAMETER) {
        guid_used = "private session GUID";
        err = try_start(kOpenSweepSessionGuid, session);
    }
    if (err != ERROR_SUCCESS) {
        running_ = false;
        reason_ = "StartTrace failed: " + log::win32_error(err) +
                  (err == ERROR_ACCESS_DENIED ? " (needs an elevated administrator or LocalSystem)" : "");
        log::warn("etw: " + reason_);
        return false;
    }

    LARGE_INTEGER f{}, q{};
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&q);
    sh_->qpc_freq = f.QuadPart > 0 ? static_cast<double>(f.QuadPart) : 1.0;
    sh_->base_qpc = q.QuadPart;
    sh_->base_unix_ms = unix_now_ms();
    sh_->accepting.store(true);

    std::vector<wchar_t> logger(name.begin(), name.end());
    logger.push_back(L'\0');
    EVENT_TRACE_LOGFILEW lf{};
    lf.LoggerName = logger.data();
    lf.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD | PROCESS_TRACE_MODE_RAW_TIMESTAMP;
    lf.EventRecordCallback = on_event;
    lf.Context = sh_.get();
    auto c = std::make_unique<Consumer>();
    c->trace = OpenTraceW(&lf);
    if (c->trace == INVALID_PROCESSTRACE_HANDLE) {
        const DWORD oe = GetLastError();
        Props stop_props(name);
        ControlTraceW(session, nullptr, stop_props.p, EVENT_TRACE_CONTROL_STOP);
        running_ = false;
        reason_ = "OpenTrace failed: " + log::win32_error(oe);
        log::warn("etw: " + reason_);
        return false;
    }
    std::shared_ptr<Shared> sh = sh_;
    std::shared_ptr<Consumer::Sync> sync = c->sync;
    const TRACEHANDLE th = c->trace;
    c->thread = std::thread([sh, sync, th] {
        HANDLE self = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentThreadId());
        if (HANDLE old = sh->thread.exchange(self)) CloseHandle(old);
        TRACEHANDLE handle = th;
        sync->status.store(ProcessTrace(&handle, 1, nullptr, nullptr));
        if (sync->done) SetEvent(sync->done);
    });
    consumer_ = std::move(c);
    session_handle_ = session;
    running_ = true;
    reason_.clear();
    started_ms_ = unix_now_ms();
    seeded_ = false;
    lost_now_ = 0;
    protect_.reset();
    std::string fl;
    for (const auto& n : kstats::flag_names(flags_)) fl += (fl.empty() ? "" : "|") + n;
    log::info("etw: kernel session started (level " + std::to_string(level) + ", flags " + fl + ", " + guid_used + ")");
    return true;
}

void KernelSession::stop_session_locked(const char* why) {
    // By handle: never stops a session with the same name that this instance did not start.
    if (session_handle_ != 0) {
        Props props(opt_.session_name);
        const ULONG err = ControlTraceW(static_cast<TRACEHANDLE>(session_handle_), nullptr, props.p, EVENT_TRACE_CONTROL_STOP);
        if (err != ERROR_SUCCESS && err != static_cast<ULONG>(ERROR_WMI_INSTANCE_NOT_FOUND))
            log::warn("etw: ControlTrace(STOP) failed: " + log::win32_error(err));
        session_handle_ = 0;
    }
    if (consumer_) {
        const ULONG ce = CloseTrace(consumer_->trace);
        if (ce != ERROR_SUCCESS && ce != static_cast<ULONG>(ERROR_CTX_CLOSE_PENDING))
            log::warn("etw: CloseTrace failed: " + log::win32_error(ce));
        const DWORD w = consumer_->sync->done
                            ? WaitForSingleObject(consumer_->sync->done, static_cast<DWORD>(opt_.join_timeout_ms))
                            : WAIT_FAILED;
        if (w == WAIT_OBJECT_0) {
            consumer_->thread.join();
            if (HANDLE t = sh_->thread.exchange(nullptr)) CloseHandle(t);
        } else {
            // ProcessTrace did not return in time: never block the SCM stop. The thread keeps its own
            // references (shared state, sync event) and ends when ETW releases it.
            consumer_->thread.detach();
            log::warn("etw: consumer thread did not finish within the stop budget; detached");
        }
        consumer_.reset();
    }
    sh_->accepting.store(false);
    running_ = false;
    lost_base_ += lost_now_;
    lost_now_ = 0;
    log::info(std::string("etw: kernel session stopped (") + why + ")");
}

bool KernelSession::start() {
    bool ok = false;
    {
        std::lock_guard<std::mutex> lock(ctl_mu_);
        enabled_ = true;
        ok = running_ || start_level_locked(0);
    }
    {
        std::lock_guard<std::mutex> lock(sup_mu_);
        if (!supervisor_.joinable()) {
            sup_stop_ = false;
            supervisor_ = std::thread([this] { supervise(); });
        }
    }
    return ok;
}

void KernelSession::stop() {
    {
        std::lock_guard<std::mutex> lock(sup_mu_);
        sup_stop_ = true;
    }
    sup_cv_.notify_all();
    if (supervisor_.joinable()) supervisor_.join();
    std::lock_guard<std::mutex> lock(ctl_mu_);
    if (running_ || consumer_) stop_session_locked("stop");
    if (reason_.empty()) reason_ = "stopped";
}

kstats::EtwStatus KernelSession::set_enabled(bool enabled) {
    bool need_supervisor = false;
    {
        std::lock_guard<std::mutex> lock(ctl_mu_);
        enabled_ = enabled;
        if (enabled && !running_) {
            start_level_locked(0);
            need_supervisor = true;
        } else if (!enabled) {
            if (running_ || consumer_) stop_session_locked("disabled");
            reason_ = "disabled (setEtw / config.json etw:false)";
        }
    }
    if (need_supervisor) {
        std::lock_guard<std::mutex> lock(sup_mu_);
        if (!supervisor_.joinable()) {
            sup_stop_ = false;
            supervisor_ = std::thread([this] { supervise(); });
        }
    }
    return status();
}

kstats::EtwStatus KernelSession::status() const {
    kstats::EtwStatus st;
    {
        std::lock_guard<std::mutex> lock(ctl_mu_);
        st.enabled = enabled_;
        st.running = running_;
        st.flags = flags_;
        st.level = level_;
        st.events_lost = lost_base_ + lost_now_;
        st.cost_percent = running_ ? protect_.cost_percent() : 0.0;
        st.reason = reason_;
    }
    std::lock_guard<std::mutex> lock(sh_->mu);
    st.events = sh_->stats.counters().events;
    return st;
}

kstats::LatencyView KernelSession::latency() {
    const std::int64_t now = unix_now_ms();
    std::lock_guard<std::mutex> lock(sh_->mu);
    return sh_->stats.latency(now);
}

std::vector<kstats::DiskProcessView> KernelSession::disk_activity() {
    const std::int64_t now = unix_now_ms();
    std::lock_guard<std::mutex> lock(sh_->mu);
    return sh_->stats.disk_activity(now);
}

std::vector<kstats::FaultProcessView> KernelSession::hard_faults() {
    const std::int64_t now = unix_now_ms();
    std::lock_guard<std::mutex> lock(sh_->mu);
    return sh_->stats.hard_faults(now);
}

void KernelSession::fill_sample(telemetry::Sample& s) {
    bool running = false;
    double cost = 0.0;
    {
        std::lock_guard<std::mutex> lock(ctl_mu_);
        running = running_;
        cost = running_ ? protect_.cost_percent() : 0.0;
    }
    const std::int64_t now = unix_now_ms();
    {
        std::lock_guard<std::mutex> lock(sh_->mu);
        sh_->stats.fill_sample(now, s, running, cost);
    }
    // Per-process hard faults are only available from the kernel trace.
    for (auto& p : s.top)
        for (const auto& k : s.kernel_processes)
            if (k.pid == p.pid && k.hard_faults_per_sec > 0.0)
                p.hard_faults_per_sec = static_cast<std::uint64_t>(k.hard_faults_per_sec + 0.5);
}

kstats::Counters KernelSession::counters() const {
    std::lock_guard<std::mutex> lock(sh_->mu);
    return sh_->stats.counters();
}

std::size_t KernelSession::kernel_images() const {
    std::lock_guard<std::mutex> lock(sh_->mu);
    return sh_->stats.images();
}

json::Value KernelSession::diagnostics() const {
    json::Value o = kstats::etw_status_json(status());
    const kstats::Counters c = counters();
    json::Value cj = json::Value::object();
    cj.set("events", c.events);
    cj.set("dpcIsr", c.dpc_isr);
    cj.set("disk", c.disk);
    cj.set("hardFaults", c.hard_faults);
    cj.set("malformed", c.malformed);
    cj.set("unknownRoutine", c.unknown_routine);
    cj.set("unattributed", c.unattributed);
    cj.set("mapResets", c.map_resets);
    cj.set("rawEvents", sh_->raw_events.load());
    cj.set("kernelImageEvents", sh_->kernel_images.load());
    cj.set("callbackExceptions", sh_->exceptions.load());
    o.set("counters", std::move(cj));
    o.set("kernelImages", kernel_images());
    return o;
}

// Fallback when no kernel image rundown arrived (e.g. a Windows build that does not send it to a
// system logger session): driver ranges from EnumDeviceDrivers. Sizes are not reported by that API,
// so each range ends at the next driver's base (capped at 64 MB).
void KernelSession::seed_drivers_if_needed() {
    if (seeded_ || !running_ || unix_now_ms() - started_ms_ < 5000) return;
    seeded_ = true;
    if (sh_->kernel_images.load() > 0) return;
    std::vector<LPVOID> bases(2048);
    DWORD needed = 0;
    if (!EnumDeviceDrivers(bases.data(), static_cast<DWORD>(bases.size() * sizeof(LPVOID)), &needed)) {
        log::warn("etw: no kernel image events and EnumDeviceDrivers failed: " + log::win32_error(GetLastError()));
        return;
    }
    bases.resize(std::min<std::size_t>(bases.size(), needed / sizeof(LPVOID)));
    std::vector<std::pair<std::uint64_t, std::string>> drivers;
    for (LPVOID b : bases) {
        const auto base = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(b));
        if (base == 0) continue;  // addresses hidden from this caller
        wchar_t nm[MAX_PATH] = {};
        if (!GetDeviceDriverBaseNameW(b, nm, MAX_PATH)) continue;
        drivers.emplace_back(base, log::narrow(nm));
    }
    std::sort(drivers.begin(), drivers.end());
    std::size_t seeded = 0;
    std::lock_guard<std::mutex> lock(sh_->mu);
    for (std::size_t i = 0; i < drivers.size(); ++i) {
        const std::uint64_t next = i + 1 < drivers.size() ? drivers[i + 1].first : drivers[i].first + (16ull << 20);
        const std::uint64_t size = std::min<std::uint64_t>(next - drivers[i].first, 64ull << 20);
        if (size == 0) continue;
        sh_->stats.on_image_load(drivers[i].first, size, drivers[i].second);
        ++seeded;
    }
    log::warn("etw: no kernel image rundown seen; seeded " + std::to_string(seeded) +
              " driver ranges from EnumDeviceDrivers (approximate sizes)");
}

void KernelSession::supervise() {
    int restarts = 0;
    std::int64_t next_retry = 0;
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(sup_mu_);
            if (sup_cv_.wait_for(lock, std::chrono::milliseconds(opt_.supervise_ms), [this] { return sup_stop_; })) return;
        }
        std::lock_guard<std::mutex> lock(ctl_mu_);
        const std::int64_t now = unix_now_ms();
        if (!running_) {
            // Unexpected end (not disabled, not degraded to the last level): bounded retries.
            if (enabled_ && level_ < kstats::kMaxLevel && next_retry != 0 && now >= next_retry && restarts < 3) {
                ++restarts;
                next_retry = 0;
                start_level_locked(level_);
            }
            continue;
        }
        if (consumer_ && consumer_->finished()) {
            const unsigned long st = consumer_->sync->status.load();
            stop_session_locked("consumer ended");
            reason_ = "the kernel trace session ended unexpectedly (ProcessTrace: " + log::win32_error(st) + ")";
            log::warn("etw: " + reason_);
            next_retry = now + 60'000;
            continue;
        }
        Props props(opt_.session_name);
        const ULONG qe = ControlTraceW(0, opt_.session_name.c_str(), props.p, EVENT_TRACE_CONTROL_QUERY);
        if (qe != ERROR_SUCCESS) {
            stop_session_locked("query failed");
            reason_ = "the kernel trace session disappeared (" + log::win32_error(qe) + ")";
            log::warn("etw: " + reason_);
            next_retry = now + 60'000;
            continue;
        }
        lost_now_ = static_cast<std::uint64_t>(props.p->EventsLost) + props.p->RealTimeBuffersLost;
        {
            std::lock_guard<std::mutex> sl(sh_->mu);
            sh_->stats.set_events_lost(lost_base_ + lost_now_);
        }
        const auto verdict = protect_.update(now, thread_cpu_us(sh_->thread.load()), lost_now_);
        if (verdict != kstats::SelfProtection::Verdict::Ok) {
            const double cost = protect_.cost_percent();
            const int next = level_ + 1;
            char cost_text[32];
            std::snprintf(cost_text, sizeof cost_text, "%.2f", cost);
            std::string why = verdict == kstats::SelfProtection::Verdict::TooExpensive
                                  ? std::string("processing cost ") + cost_text + "% of one CPU over 30 s"
                                  : std::string("events lost continuously for 30 s");
            stop_session_locked("self-protection");
            if (next >= kstats::kMaxLevel || !start_level_locked(next)) {
                level_ = std::max(level_, next);
                running_ = false;
                reason_ = "self-protection: " + why + "; kernel tracing stopped";
            } else {
                reason_ = "self-protection: " + why + "; restarted with fewer flags (level " + std::to_string(next) + ")";
            }
            log::warn("etw: " + reason_);
            continue;
        }
        seed_drivers_if_needed();
    }
}

}  // namespace opensweep::win
