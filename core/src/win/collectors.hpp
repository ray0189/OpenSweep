// SystemCollector: fills a telemetry::Sample every engine tick (Windows only, read-only).
//
// Sources (documented APIs only):
//  * PDH with English counter paths (PdhAddEnglishCounterW), so localized Windows works:
//      \Processor(_Total)\% Processor Time, \System\Processor Queue Length,
//      \System\Context Switches/sec, \Processor(_Total)\% DPC Time, \Processor(_Total)\% Interrupt Time,
//      \Memory\Page Reads/sec (hard faults that hit disk), \Memory\Page Faults/sec,
//      \PhysicalDisk(_Total)\Avg. Disk sec/Read, \PhysicalDisk(_Total)\Avg. Disk sec/Write,
//      \PhysicalDisk(_Total)\Current Disk Queue Length, \PhysicalDisk(_Total)\Disk Bytes/sec,
//      \Processor Information(_Total)\% Processor Performance.
//    A counter that cannot be added or read leaves its field at the default (0; cpu_freq_percent
//    stays at its default 100 so a missing counter is never mistaken for throttling) and is logged
//    once. If the CPU counter is missing, cpu_percent falls back to GetSystemTimes deltas.
//  * GlobalMemoryStatusEx + GetPerformanceInfo: memory, commit, cache, kernel pools, counts.
//  * Per process: CreateToolhelp32Snapshot (pid, parent, threads, exe name), OpenProcess with
//    PROCESS_QUERY_LIMITED_INFORMATION, GetProcessTimes, GetProcessMemoryInfo
//    (PROCESS_MEMORY_COUNTERS_EX; the _EX2 struct is not in every SDK/MinGW header and adds nothing
//    we need), GetProcessHandleCount, GetPriorityClass, ProcessIdToSessionId,
//    QueryFullProcessImageNameW (file name kept; path used only to test "under %SystemRoot%"),
//    GetProcessInformation(ProcessProtectionLevelInfo), IsProcessCritical.
//
// Not collected (documented in docs + report):
//  * compressed_bytes: there is no documented counter/API for the memory compression store size
//    (the "Memory Compression" process working set is an implementation detail), so it stays 0.
//  * has_audio: a session-0 service has no cheap documented way to see per-process audio
//    rendering in user sessions (WASAPI session enumeration is per user/endpoint and needs COM in
//    the user's session). TODO: let the user-session client report it like the foreground hint.
//  * signed_by_microsoft: WinVerifyTrust (+ catalog lookup) is too expensive for the 2 s loop and
//    is not done; the field stays false. "Under %SystemRoot%" is used for criticality instead.
//  * ProcessSample::hard_faults_per_sec: per-process hard faults need ETW; stays 0.
//    page_faults_per_sec (soft + hard, from PageFaultCount deltas) is filled instead.
//
// Privacy: never reads command lines, window titles, environment or process memory.
// Threading: collect() must be called from one thread; report_foreground()/foreground_pid() are
// thread-safe.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "telemetry.hpp"

namespace opensweep::win {

class SystemCollector {
public:
    struct Options {
        std::size_t max_top = 40;                   // bound for Sample::top (also the contract maximum)
        std::vector<std::wstring> extra_counters;   // extra English PDH paths (tests: exercise the missing path)
    };

    SystemCollector();
    explicit SystemCollector(Options options);
    ~SystemCollector();
    SystemCollector(const SystemCollector&) = delete;
    SystemCollector& operator=(const SystemCollector&) = delete;

    // Fills every field of `out` (out.top is reused, its capacity is kept). Returns false only if
    // the basic memory APIs fail; missing PDH counters do not fail the sample.
    bool collect(telemetry::Sample& out);

    // Wall time of the last / the slowest collect() in microseconds.
    std::uint64_t last_collect_us() const noexcept { return last_us_.load(std::memory_order_relaxed); }
    std::uint64_t max_collect_us() const noexcept { return max_us_.load(std::memory_order_relaxed); }
    // PDH counters that were added successfully / could not be added.
    std::size_t counters_ok() const noexcept;
    std::size_t counters_missing() const noexcept;

    // Foreground hint from the logged-on user's client (session 0 cannot see the foreground window).
    // Valid only if the pid exists, is not in session 0, and (unless kAnySession) is in
    // `caller_session`. The hint expires after kForegroundTtlMs without a refresh.
    static constexpr std::uint32_t kAnySession = 0xFFFFFFFFu;
    static constexpr std::uint64_t kForegroundTtlMs = 10000;
    bool report_foreground(std::uint32_t pid, std::uint32_t caller_session = kAnySession);
    std::uint32_t foreground_pid() const;  // 0 when unknown, expired or the process exited

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic<std::uint64_t> last_us_{0};
    std::atomic<std::uint64_t> max_us_{0};
};

}  // namespace opensweep::win
