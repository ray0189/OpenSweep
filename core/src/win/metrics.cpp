#include "metrics.hpp"

#include <windows.h>
#include <psapi.h>

#include <chrono>

#include "log.hpp"

namespace opensweep::win {

namespace {

std::uint64_t ft_to_u64(const FILETIME& ft) noexcept {
    return (static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

struct CpuTimes {
    std::uint64_t idle = 0;
    std::uint64_t kernel = 0;  // includes idle time
    std::uint64_t user = 0;
};

bool read_cpu_times(CpuTimes& t) {
    FILETIME idle{}, kernel{}, user{};
    if (!GetSystemTimes(&idle, &kernel, &user)) return false;
    t.idle = ft_to_u64(idle);
    t.kernel = ft_to_u64(kernel);
    t.user = ft_to_u64(user);
    return true;
}

}  // namespace

CpuSampler::~CpuSampler() { stop(); }

void CpuSampler::start() {
    std::lock_guard<std::mutex> lock(mu_);
    if (thread_.joinable()) return;
    stopping_ = false;
    thread_ = std::thread([this] { run(); });
}

void CpuSampler::stop() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stopping_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

double CpuSampler::percent() const {
    std::lock_guard<std::mutex> lock(mu_);
    return percent_;
}

void CpuSampler::run() {
    CpuTimes prev;
    bool have_prev = read_cpu_times(prev);
    if (!have_prev) log::warn("GetSystemTimes failed: " + log::win32_error(GetLastError()));
    std::unique_lock<std::mutex> lock(mu_);
    while (!stopping_) {
        if (cv_.wait_for(lock, std::chrono::seconds(1), [this] { return stopping_; })) break;
        lock.unlock();
        CpuTimes cur;
        const bool ok = read_cpu_times(cur);
        double pct = -1.0;
        if (ok && have_prev) {
            const std::uint64_t idle = cur.idle - prev.idle;
            const std::uint64_t total = (cur.kernel - prev.kernel) + (cur.user - prev.user);
            if (total > 0 && idle <= total) pct = 100.0 * static_cast<double>(total - idle) / static_cast<double>(total);
        }
        if (ok) {
            prev = cur;
            have_prev = true;
        }
        lock.lock();
        if (pct >= 0.0) percent_ = pct;
    }
}

bool WinSystemProvider::metrics(commands::MetricsSnapshot& out) {
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (!GlobalMemoryStatusEx(&ms)) return false;

    PERFORMANCE_INFORMATION pi{};
    pi.cb = sizeof(pi);
    if (!GetPerformanceInfo(&pi, sizeof(pi))) return false;

    const std::uint64_t page = pi.PageSize;
    out.timestamp_ms = commands::now_unix_ms();
    out.cpu_percent = cpu_.percent();
    out.memory.total_phys = ms.ullTotalPhys;
    out.memory.avail_phys = ms.ullAvailPhys;
    out.memory.load_percent = ms.dwMemoryLoad;
    out.memory.commit_total = static_cast<std::uint64_t>(pi.CommitTotal) * page;
    out.memory.commit_limit = static_cast<std::uint64_t>(pi.CommitLimit) * page;
    out.memory.commit_peak = static_cast<std::uint64_t>(pi.CommitPeak) * page;
    out.memory.system_cache = static_cast<std::uint64_t>(pi.SystemCache) * page;
    out.memory.kernel_paged = static_cast<std::uint64_t>(pi.KernelPaged) * page;
    out.memory.kernel_nonpaged = static_cast<std::uint64_t>(pi.KernelNonpaged) * page;
    out.processes = pi.ProcessCount;
    out.threads = pi.ThreadCount;
    out.handles = pi.HandleCount;
    out.page_size = page;
    return true;
}

bool WinSystemProvider::topology(commands::TopologyInfo& out) {
    if (topology_.logical_processors == 0) return false;
    out = topology_;
    return true;
}

bool WinSystemProvider::self_process(commands::SelfProcessInfo& out) {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc)))
        return false;
    FILETIME create{}, exit_time{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &create, &exit_time, &kernel, &user)) return false;
    out.working_set = pmc.WorkingSetSize;
    out.private_bytes = pmc.PrivateUsage;
    out.cpu_time_ms = (ft_to_u64(kernel) + ft_to_u64(user)) / 10000;  // 100 ns units -> ms
    return true;
}

std::uint32_t WinSystemProvider::pid() { return GetCurrentProcessId(); }

}  // namespace opensweep::win
