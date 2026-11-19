#include "collectors.hpp"

#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <mutex>
#include <unordered_map>

#include "log.hpp"
#include "process_info.hpp"

namespace opensweep::win {

namespace {

using telemetry::ProcessSample;
using telemetry::Sample;

struct CounterDef {
    const wchar_t* path;
    double Sample::*field;
    double scale;
};

// English counter paths (PdhAddEnglishCounterW resolves them on localized systems).
const CounterDef kCounterDefs[] = {
    {L"\\Processor(_Total)\\% Processor Time", &Sample::cpu_percent, 1.0},
    {L"\\System\\Processor Queue Length", &Sample::cpu_queue_length, 1.0},
    {L"\\System\\Context Switches/sec", &Sample::context_switches_per_sec, 1.0},
    {L"\\Processor(_Total)\\% DPC Time", &Sample::dpc_percent, 1.0},
    {L"\\Processor(_Total)\\% Interrupt Time", &Sample::interrupt_percent, 1.0},
    {L"\\Memory\\Page Reads/sec", &Sample::hard_faults_per_sec, 1.0},
    {L"\\Memory\\Page Faults/sec", &Sample::page_faults_per_sec, 1.0},
    {L"\\PhysicalDisk(_Total)\\Avg. Disk sec/Read", &Sample::disk_read_latency_ms, 1000.0},
    {L"\\PhysicalDisk(_Total)\\Avg. Disk sec/Write", &Sample::disk_write_latency_ms, 1000.0},
    {L"\\PhysicalDisk(_Total)\\Current Disk Queue Length", &Sample::disk_queue_length, 1.0},
    {L"\\PhysicalDisk(_Total)\\Disk Bytes/sec", &Sample::disk_bytes_per_sec, 1.0},
    // Can exceed 100 (turbo), hence PDH_FMT_NOCAP100 below for every counter.
    {L"\\Processor Information(_Total)\\% Processor Performance", &Sample::cpu_freq_percent, 1.0},
};

constexpr int kFailuresBeforeWarn = 3;  // rate counters can be invalid right after start

std::string hex32(unsigned long v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08lX", v);
    return buf;
}

struct Counter {
    std::wstring path;
    double Sample::*field = nullptr;  // nullptr: extra counter (read but not stored)
    double scale = 1.0;
    PDH_HCOUNTER handle = nullptr;
    int consecutive_failures = 0;
    bool warned = false;
};

struct ProcState {
    std::uint64_t create = 0;  // FILETIME of creation: (pid, create) identifies a process instance
    std::uint64_t cpu = 0;     // kernel + user, 100 ns
    std::uint64_t faults = 0;  // PageFaultCount
    std::uint64_t epoch = 0;   // last collect() that saw it
    bool classified = false;   // the fields below are fixed for the life of the process
    bool under_root = false;
    bool critical = false;
    bool windows_protected = false;
    bool antimalware_protected = false;
};

struct Candidate {
    DWORD pid = 0;
    DWORD ppid = 0;
    DWORD threads = 0;
    HANDLE handle = nullptr;
    std::uint64_t create = 0;
    std::uint64_t private_bytes = 0;
    std::uint64_t working_set = 0;
    double cpu_percent = 0.0;
    double page_faults_per_sec = 0.0;
    wchar_t name[MAX_PATH] = {};
};

std::uint64_t ft_u64(const FILETIME& ft) noexcept { return procinfo::filetime_u64(ft); }

std::int64_t now_unix_ms() noexcept {
    FILETIME ft{};
    GetSystemTimePreciseAsFileTime(&ft);
    return procinfo::filetime_to_unix_ms(ft_u64(ft));
}

}  // namespace

struct SystemCollector::Impl {
    Options opt;

    // PDH
    PDH_HQUERY query = nullptr;
    std::vector<Counter> counters;
    std::size_t ok = 0;
    std::size_t missing = 0;
    bool collect_warned = false;

    // GetSystemTimes fallback for cpu_percent
    std::uint64_t prev_idle = 0, prev_kernel = 0, prev_user = 0;
    bool have_sys_times = false;

    // Interval timing
    LARGE_INTEGER qpc_freq{};
    LONGLONG prev_qpc = 0;
    std::uint32_t ncpu = 1;
    std::uint32_t self_pid = 0;

    // Per-process state and scratch buffers (capacity reused across ticks).
    std::unordered_map<std::uint32_t, ProcState> procs;
    std::uint64_t epoch = 0;
    std::vector<Candidate> cands;
    std::vector<std::uint32_t> by_cpu;
    std::vector<std::uint32_t> by_mem;
    std::vector<std::uint32_t> chosen;
    std::vector<std::uint8_t> picked;
    bool snapshot_warned = false;

    // Foreground hint
    mutable std::mutex fg_mu;
    std::uint32_t fg_pid = 0;
    std::uint64_t fg_create = 0;
    ULONGLONG fg_tick = 0;

    explicit Impl(Options o) : opt(std::move(o)) {
        if (opt.max_top == 0 || opt.max_top > 40) opt.max_top = 40;
        QueryPerformanceFrequency(&qpc_freq);
        const DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
        ncpu = n > 0 ? n : 1;
        self_pid = GetCurrentProcessId();
        procs.reserve(512);
        cands.reserve(512);
        by_cpu.reserve(512);
        by_mem.reserve(512);
        chosen.reserve(64);
        picked.reserve(512);
        open_pdh();
        read_sys_times();  // prime the fallback
    }

    ~Impl() {
        if (query) PdhCloseQuery(query);
    }

    void open_pdh() {
        const PDH_STATUS st = PdhOpenQueryW(nullptr, 0, &query);
        if (st != ERROR_SUCCESS) {
            query = nullptr;
            log::warn("PdhOpenQuery failed: " + hex32(static_cast<unsigned long>(st)) +
                      "; PDH-based fields stay at their defaults");
            missing = std::size(kCounterDefs) + opt.extra_counters.size();
            return;
        }
        counters.reserve(std::size(kCounterDefs) + opt.extra_counters.size());
        for (const CounterDef& d : kCounterDefs) add_counter(d.path, d.field, d.scale);
        for (const std::wstring& p : opt.extra_counters) add_counter(p, nullptr, 1.0);
        // Prime rate counters so the first collect() has a reference point.
        PdhCollectQueryData(query);
        log::info("pdh: " + std::to_string(ok) + " counter(s) available, " + std::to_string(missing) + " missing");
    }

    void add_counter(const std::wstring& path, double Sample::*field, double scale) {
        Counter c;
        c.path = path;
        c.field = field;
        c.scale = scale;
        const PDH_STATUS st = PdhAddEnglishCounterW(query, path.c_str(), 0, &c.handle);
        if (st != ERROR_SUCCESS) {
            // Logged once here; the counter is simply skipped from now on.
            log::warn("PDH counter unavailable: " + log::narrow(path) + " (" + hex32(static_cast<unsigned long>(st)) + ")");
            c.handle = nullptr;
            ++missing;
        } else {
            ++ok;
        }
        counters.push_back(std::move(c));
    }

    // Returns busy percent over the interval since the previous call, or -1.
    double read_sys_times() {
        FILETIME idle{}, kernel{}, user{};
        if (!GetSystemTimes(&idle, &kernel, &user)) return -1.0;
        const std::uint64_t i = ft_u64(idle), k = ft_u64(kernel), u = ft_u64(user);
        double pct = -1.0;
        if (have_sys_times) {
            const std::uint64_t di = i - prev_idle;
            const std::uint64_t total = (k - prev_kernel) + (u - prev_user);  // kernel includes idle
            if (total > 0 && di <= total) pct = 100.0 * static_cast<double>(total - di) / static_cast<double>(total);
        }
        prev_idle = i;
        prev_kernel = k;
        prev_user = u;
        have_sys_times = true;
        return pct;
    }

    // Returns true if the cpu counter produced a value.
    bool read_pdh(Sample& out) {
        if (!query) return false;
        const PDH_STATUS st = PdhCollectQueryData(query);
        if (st != ERROR_SUCCESS) {
            if (!collect_warned) {
                collect_warned = true;
                log::warn("PdhCollectQueryData failed: " + hex32(static_cast<unsigned long>(st)));
            }
            return false;
        }
        bool cpu_ok = false;
        for (Counter& c : counters) {
            if (!c.handle) continue;
            PDH_FMT_COUNTERVALUE v{};
            const PDH_STATUS s = PdhGetFormattedCounterValue(c.handle, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, nullptr, &v);
            const bool valid =
                s == ERROR_SUCCESS && (v.CStatus == PDH_CSTATUS_VALID_DATA || v.CStatus == PDH_CSTATUS_NEW_DATA);
            if (!valid) {
                if (++c.consecutive_failures >= kFailuresBeforeWarn && !c.warned) {
                    c.warned = true;
                    log::warn("PDH counter not readable: " + log::narrow(c.path) + " (" +
                              hex32(s != ERROR_SUCCESS ? static_cast<unsigned long>(s) : v.CStatus) + ")");
                }
                continue;
            }
            c.consecutive_failures = 0;
            double value = v.doubleValue * c.scale;
            if (!(value >= 0.0)) value = 0.0;  // also catches NaN
            if (c.field) {
                out.*(c.field) = value;
                if (c.field == &Sample::cpu_percent) cpu_ok = true;
            }
        }
        return cpu_ok;
    }

    bool foreground_snapshot(std::uint32_t& pid, std::uint64_t& create) const {
        std::lock_guard<std::mutex> lock(fg_mu);
        if (fg_pid == 0 || GetTickCount64() - fg_tick > kForegroundTtlMs) return false;
        pid = fg_pid;
        create = fg_create;
        return true;
    }

    void collect_processes(Sample& out, LONGLONG now_qpc);
    void classify(const Candidate& c, ProcState* st, ProcessSample& ps);
};

namespace {

// Closes every candidate handle on scope exit (also on exceptions).
struct HandleCloser {
    std::vector<Candidate>& cands;
    ~HandleCloser() {
        for (Candidate& c : cands) {
            if (c.handle) {
                CloseHandle(c.handle);
                c.handle = nullptr;
            }
        }
    }
};

}  // namespace

void SystemCollector::Impl::classify(const Candidate& c, ProcState* st, ProcessSample& ps) {
    std::uint32_t session = 0;
    const bool have_session = procinfo::session_of(c.pid, session);
    ps.session_id = have_session ? session : 0;

    bool under_root = false, critical = false, win_prot = false, am_prot = false;
    if (c.handle) {
        DWORD hc = 0;
        if (GetProcessHandleCount(c.handle, &hc)) ps.handles = hc;
        ps.priority_class = GetPriorityClass(c.handle);  // 0 on failure
        if (st && !st->classified) {
            std::wstring path;
            if (procinfo::image_path(c.handle, path)) st->under_root = procinfo::is_under_system_root(path);
            unsigned long level = procinfo::kProtectionLevelNone;
            if (procinfo::protection_level(c.handle, level)) {
                st->windows_protected = procinfo::is_windows_protection(level);
                st->antimalware_protected = level == procinfo::kProtectionLevelAntimalwareLight;
            }
            bool crit = false;
            if (procinfo::is_critical(c.handle, crit)) st->critical = crit;
            st->classified = true;
        }
        if (st) {
            under_root = st->under_root;
            critical = st->critical;
            win_prot = st->windows_protected;
            am_prot = st->antimalware_protected;
        }
    }

    ps.is_security_software = am_prot || procinfo::is_security_software_name(ps.name);
    ps.is_service = have_session && session == 0;
    // Conservative: anything we cannot open, critical, Windows-protected, on the protection list,
    // or a session-0 image from %SystemRoot% is treated as a system-critical component.
    ps.is_system_critical = !c.handle || critical || win_prot || c.pid == 4 || procinfo::is_protected_name(ps.name) ||
                            (have_session && session == 0 && under_root) || !have_session;
    ps.has_audio = false;            // not available to a session-0 service; see header
    ps.signed_by_microsoft = false;  // WinVerifyTrust not run in the sampling loop; see header
}

void SystemCollector::Impl::collect_processes(Sample& out, LONGLONG now_qpc) {
    ++epoch;
    cands.clear();
    HandleCloser closer{cands};

    double elapsed_100ns = 0.0;
    if (prev_qpc != 0 && qpc_freq.QuadPart > 0 && now_qpc > prev_qpc)
        elapsed_100ns = static_cast<double>(now_qpc - prev_qpc) * 1e7 / static_cast<double>(qpc_freq.QuadPart);
    const double cpu_capacity = elapsed_100ns * static_cast<double>(ncpu);

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        if (!snapshot_warned) {
            snapshot_warned = true;
            log::warn("CreateToolhelp32Snapshot failed: " + log::win32_error(GetLastError()));
        }
        return;
    }
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    for (BOOL more = Process32FirstW(snap, &pe); more; more = Process32NextW(snap, &pe)) {
        if (pe.th32ProcessID == 0) continue;  // System Idle Process
        cands.emplace_back();
        Candidate& c = cands.back();
        c.pid = pe.th32ProcessID;
        c.ppid = pe.th32ParentProcessID;
        c.threads = pe.cntThreads;
        std::copy(std::begin(pe.szExeFile), std::end(pe.szExeFile), std::begin(c.name));
        c.name[MAX_PATH - 1] = L'\0';

        c.handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, c.pid);
        if (!c.handle) continue;  // protected / access denied / exited: classified as critical later

        FILETIME cr{}, ex{}, k{}, u{};
        if (!GetProcessTimes(c.handle, &cr, &ex, &k, &u)) continue;
        c.create = ft_u64(cr);
        const std::uint64_t cpu = ft_u64(k) + ft_u64(u);

        PROCESS_MEMORY_COUNTERS_EX pmc{};
        pmc.cb = sizeof(pmc);
        std::uint64_t faults = 0;
        if (GetProcessMemoryInfo(c.handle, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
            c.private_bytes = pmc.PrivateUsage;
            c.working_set = pmc.WorkingSetSize;
            faults = pmc.PageFaultCount;
        }

        auto [it, inserted] = procs.try_emplace(c.pid);
        ProcState& st = it->second;
        if (inserted || st.create != c.create) {
            st = ProcState{};  // new process or PID reuse: no delta this tick
            st.create = c.create;
        } else if (elapsed_100ns > 0.0) {
            if (cpu >= st.cpu && cpu_capacity > 0.0)
                c.cpu_percent = std::clamp(100.0 * static_cast<double>(cpu - st.cpu) / cpu_capacity, 0.0, 100.0);
            if (faults >= st.faults)
                c.page_faults_per_sec = static_cast<double>(faults - st.faults) * 1e7 / elapsed_100ns;
        }
        st.cpu = cpu;
        st.faults = faults;
        st.epoch = epoch;
    }
    CloseHandle(snap);

    // Forget processes that are gone (bounded map).
    for (auto it = procs.begin(); it != procs.end();) {
        if (it->second.epoch != epoch)
            it = procs.erase(it);
        else
            ++it;
    }

    // ---- selection: pinned (self, foreground) + interleaved top by CPU and by private bytes ----
    const std::size_t n = cands.size();
    const std::size_t limit = opt.max_top;
    by_cpu.clear();
    by_mem.clear();
    for (std::uint32_t i = 0; i < n; ++i) {
        if (cands[i].cpu_percent > 0.0) by_cpu.push_back(i);
        if (cands[i].private_bytes > 0) by_mem.push_back(i);
    }
    const std::size_t kc = std::min(limit, by_cpu.size());
    const std::size_t km = std::min(limit, by_mem.size());
    std::partial_sort(by_cpu.begin(), by_cpu.begin() + static_cast<std::ptrdiff_t>(kc), by_cpu.end(),
                      [&](std::uint32_t a, std::uint32_t b) { return cands[a].cpu_percent > cands[b].cpu_percent; });
    std::partial_sort(by_mem.begin(), by_mem.begin() + static_cast<std::ptrdiff_t>(km), by_mem.end(),
                      [&](std::uint32_t a, std::uint32_t b) { return cands[a].private_bytes > cands[b].private_bytes; });

    chosen.clear();
    picked.assign(n, 0);
    auto pick = [&](std::uint32_t i) {
        if (chosen.size() < limit && !picked[i]) {
            picked[i] = 1;
            chosen.push_back(i);
        }
    };

    std::uint32_t fg = 0;
    std::uint64_t fg_cr = 0;
    const bool have_fg = foreground_snapshot(fg, fg_cr);
    for (std::uint32_t i = 0; i < n; ++i) {
        if (cands[i].pid == self_pid) pick(i);  // OpenSweep's own footprint is always reported
        if (have_fg && cands[i].pid == fg && cands[i].create == fg_cr) {
            pick(i);
            out.foreground_pid = fg;
        }
    }
    for (std::size_t r = 0; r < std::max(kc, km) && chosen.size() < limit; ++r) {
        if (r < kc) pick(by_cpu[r]);
        if (r < km) pick(by_mem[r]);
    }

    out.top.clear();
    for (std::uint32_t i : chosen) {
        const Candidate& c = cands[i];
        out.top.emplace_back();
        ProcessSample& ps = out.top.back();
        ps.pid = c.pid;
        ps.parent_pid = c.ppid;
        ps.name = log::narrow(c.name);
        ps.cpu_percent = c.cpu_percent;
        ps.private_bytes = c.private_bytes;
        ps.working_set = c.working_set;
        ps.threads = c.threads;
        ps.page_faults_per_sec = c.page_faults_per_sec;
        ps.start_ms = c.create ? procinfo::filetime_to_unix_ms(c.create) : 0;
        ps.foreground = out.foreground_pid != 0 && c.pid == out.foreground_pid;
        auto it = c.handle ? procs.find(c.pid) : procs.end();
        classify(c, it != procs.end() ? &it->second : nullptr, ps);
    }
}

SystemCollector::SystemCollector() : SystemCollector(Options{}) {}

SystemCollector::SystemCollector(Options options) : impl_(std::make_unique<Impl>(std::move(options))) {}

SystemCollector::~SystemCollector() = default;

std::size_t SystemCollector::counters_ok() const noexcept { return impl_->ok; }
std::size_t SystemCollector::counters_missing() const noexcept { return impl_->missing; }

bool SystemCollector::collect(telemetry::Sample& out) {
    LARGE_INTEGER t0{};
    QueryPerformanceCounter(&t0);
    Impl& m = *impl_;

    // Reset every field to its default but keep the capacity of `top`.
    std::vector<ProcessSample> top = std::move(out.top);
    out = Sample{};
    out.top = std::move(top);
    out.top.clear();
    out.ts_ms = now_unix_ms();

    const bool pdh_cpu = m.read_pdh(out);
    const double fallback_cpu = m.read_sys_times();
    if (!pdh_cpu && fallback_cpu >= 0.0) out.cpu_percent = fallback_cpu;
    out.cpu_percent = std::clamp(out.cpu_percent, 0.0, 100.0);

    bool ok = true;
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        out.mem_total = ms.ullTotalPhys;
        out.mem_avail = ms.ullAvailPhys;
        out.mem_load_percent = ms.dwMemoryLoad;
    } else {
        ok = false;
    }
    PERFORMANCE_INFORMATION pi{};
    pi.cb = sizeof(pi);
    if (GetPerformanceInfo(&pi, sizeof(pi))) {
        const std::uint64_t page = pi.PageSize;
        out.commit_total = static_cast<std::uint64_t>(pi.CommitTotal) * page;
        out.commit_limit = static_cast<std::uint64_t>(pi.CommitLimit) * page;
        out.system_cache = static_cast<std::uint64_t>(pi.SystemCache) * page;
        out.kernel_paged = static_cast<std::uint64_t>(pi.KernelPaged) * page;
        out.kernel_nonpaged = static_cast<std::uint64_t>(pi.KernelNonpaged) * page;
        out.processes = pi.ProcessCount;
        out.threads = pi.ThreadCount;
        out.handles = pi.HandleCount;
    } else {
        ok = false;
    }
    out.compressed_bytes = 0;  // no documented source; see header

    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    m.collect_processes(out, now.QuadPart);
    m.prev_qpc = now.QuadPart;

    LARGE_INTEGER t1{};
    QueryPerformanceCounter(&t1);
    if (m.qpc_freq.QuadPart > 0) {
        const auto us = static_cast<std::uint64_t>((t1.QuadPart - t0.QuadPart) * 1000000 / m.qpc_freq.QuadPart);
        last_us_.store(us, std::memory_order_relaxed);
        if (us > max_us_.load(std::memory_order_relaxed)) max_us_.store(us, std::memory_order_relaxed);
    }
    return ok;
}

bool SystemCollector::report_foreground(std::uint32_t pid, std::uint32_t caller_session) {
    if (pid == 0 || pid == 4) return false;
    procinfo::ProcessHandle h = procinfo::open_process(pid, PROCESS_QUERY_LIMITED_INFORMATION);
    if (!h || !procinfo::is_running(h.get())) return false;
    std::uint64_t create = 0;
    if (!procinfo::creation_time_100ns(h.get(), create)) return false;
    std::uint32_t session = 0;
    if (!procinfo::session_of(pid, session) || session == 0) return false;  // must be an interactive session
    if (caller_session != kAnySession && session != caller_session) return false;
    std::lock_guard<std::mutex> lock(impl_->fg_mu);
    impl_->fg_pid = pid;
    impl_->fg_create = create;
    impl_->fg_tick = GetTickCount64();
    return true;
}

std::uint32_t SystemCollector::foreground_pid() const {
    std::uint32_t pid = 0;
    std::uint64_t create = 0;
    if (!impl_->foreground_snapshot(pid, create)) return 0;
    procinfo::ProcessHandle h = procinfo::open_process(pid, PROCESS_QUERY_LIMITED_INFORMATION);
    std::uint64_t now_create = 0;
    if (!h || !procinfo::is_running(h.get()) || !procinfo::creation_time_100ns(h.get(), now_create) ||
        now_create != create)
        return 0;
    return pid;
}

}  // namespace opensweep::win
