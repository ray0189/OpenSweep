// Private kernel trace session (docs/core/etw.md "Session"). Windows only; documented APIs only.
//
//  * StartTraceW with EVENT_TRACE_SYSTEM_LOGGER_MODE | EVENT_TRACE_REAL_TIME_MODE (Windows 8+),
//    session name "OpenSweepCoreKernel", Wnode.Guid = SystemTraceControlGuid (falls back to a private
//    session GUID if this Windows build rejects that), ClientContext = 1 (QueryPerformanceCounter
//    timestamps), EnableFlags = PROCESS | THREAD | IMAGE_LOAD | DPC | INTERRUPT | DISK_IO |
//    HARD_FAULTS (fewer after self-protection), 64 KB x 4..16 buffers, FlushTimer 1 s.
//    Never touches the global "NT Kernel Logger".
//  * A stale session with the same name (crash) is stopped first (ControlTraceW STOP).
//  * OpenTraceW (PROCESS_TRACE_MODE_REAL_TIME | EVENT_RECORD | RAW_TIMESTAMP) + ProcessTrace on a
//    dedicated thread. The EventRecordCallback decodes classic (MOF) SystemTraceProvider events by
//    class GUID + opcode with the documented fixed layouts, with bounds checks on every field and
//    the pointer size taken from EVENT_HEADER_FLAG_32_BIT_HEADER / _64_BIT_HEADER. Malformed events
//    are counted and skipped. Nothing is stored raw: events go straight into kstats::KernelStats.
//  * DPC / ISR duration: the classic PerfInfo DPC (66 ThreadedDPC, 68 DPC, 69 TimerDPC) and ISR
//    (67 ISR, 50 ISR-MSI) events are logged when the routine returns; their payload starts with
//    InitialTime (the entry timestamp, same clock as the event header) followed by Routine. The
//    duration is therefore EventHeader.TimeStamp - InitialTime, converted with the QPC frequency.
//    This needs raw (QPC) header timestamps, hence ClientContext = 1 + PROCESS_TRACE_MODE_RAW_TIMESTAMP.
//  * Disk latency: DiskIo Read/Write (opcodes 10/11) HighResResponseTime is in QPC ticks.
//  * A supervisor thread (1 s) queries the session (events lost, real-time buffers lost), measures
//    the consumer thread's CPU time (GetThreadTimes) and applies kstats::SelfProtection: > 2 % of
//    one CPU over 30 s, or loss in every check for 30 s -> stop and restart with fewer flags (DPC/ISR
//    first, then disk, then hard faults; at the last level the session stays stopped and the status
//    says why).
//  * stop(): ControlTraceW(STOP) + CloseTrace + consumer join (bounded wait, 5 s), supervisor join.
//  * Privilege: SeSystemProfilePrivilege is enabled when present (best effort); see service.cpp.
//  * Privacy: only driver file names, process image names, byte counts and timings are kept.
#pragma once

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "json.hpp"
#include "kernel_stats.hpp"
#include "telemetry.hpp"

namespace opensweep::win {

inline constexpr const wchar_t* kEtwSessionName = L"OpenSweepCoreKernel";

class KernelSession {
public:
    struct Options {
        unsigned logical_processors = 1;
        std::wstring session_name = kEtwSessionName;
        unsigned long buffer_kb = 64;
        unsigned long min_buffers = 4;
        unsigned long max_buffers = 16;
        unsigned long flush_seconds = 1;
        kstats::ProtectionConfig protection;
        std::int64_t supervise_ms = 1000;
        std::int64_t join_timeout_ms = 5000;
        bool resolve_names = true;   // OpenProcess + QueryFullProcessImageNameW for pids without a name
    };

    KernelSession();
    explicit KernelSession(Options o);
    ~KernelSession();
    KernelSession(const KernelSession&) = delete;
    KernelSession& operator=(const KernelSession&) = delete;

    // Starts the session (level 0) and the supervisor. False if StartTrace/OpenTrace failed; the
    // reason is in status().reason. Safe to call again after a failure or stop().
    bool start();
    // Stops session, consumer and supervisor. Idempotent; bounded by ~join_timeout_ms.
    void stop();
    // setEtw: start (true) or stop (false) and remember the choice for the supervisor.
    kstats::EtwStatus set_enabled(bool enabled);

    kstats::EtwStatus status() const;
    kstats::LatencyView latency();
    std::vector<kstats::DiskProcessView> disk_activity();
    std::vector<kstats::FaultProcessView> hard_faults();
    // Fills the kernel fields of `s` (interval since the previous call) and copies per-process hard
    // faults into s.top[*].hard_faults_per_sec for matching pids.
    void fill_sample(telemetry::Sample& s);
    json::Value diagnostics() const;
    kstats::Counters counters() const;
    std::size_t kernel_images() const;

    // Session helpers (also used by tests). exists: ControlTraceW(QUERY) succeeds for that name.
    // stop_session stops by *name* (stale-session cleanup); the running session is stopped by handle.
    static bool session_exists(const std::wstring& name);
    static bool stop_session(const std::wstring& name, unsigned long* error = nullptr);

    struct Shared;  // consumer-side state (outlives a consumer thread that misses the join timeout)

private:
    struct Consumer;
    bool start_level_locked(int level);
    void stop_session_locked(const char* why);
    void supervise();
    void seed_drivers_if_needed();

    Options opt_;
    std::shared_ptr<Shared> sh_;
    mutable std::mutex ctl_mu_;          // session control state below
    std::unique_ptr<Consumer> consumer_;
    std::uint64_t session_handle_ = 0;   // TRACEHANDLE from StartTraceW (0 = none)
    bool enabled_ = true;
    bool running_ = false;
    int level_ = 0;
    std::uint32_t flags_ = 0;
    std::string reason_;
    std::uint64_t lost_base_ = 0;        // events lost by previous session generations
    std::uint64_t lost_now_ = 0;         // current generation
    std::int64_t started_ms_ = 0;
    bool seeded_ = false;
    kstats::SelfProtection protect_;
    std::thread supervisor_;
    std::mutex sup_mu_;
    std::condition_variable sup_cv_;
    bool sup_stop_ = false;
};

}  // namespace opensweep::win
