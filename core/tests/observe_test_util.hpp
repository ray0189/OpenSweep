// Shared helpers for the trace / baseline / incident / guard tests.
#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <random>
#include <string>
#include <string_view>

#include "telemetry.hpp"
#include "trace.hpp"

namespace obs_test {

inline constexpr double kMB = 1024.0 * 1024.0;
inline constexpr std::uint64_t kMBu = 1024ull * 1024ull;
inline constexpr std::uint64_t kGBu = 1024ull * kMBu;

// In-memory FileIO with failure injection.
class MemFileIO final : public opensweep::trace::FileIO {
public:
    std::map<std::string, std::string> files;
    bool fail_writes = false;
    int atomic_writes = 0;
    int appends = 0;

    bool read_all(const std::string& path, std::string& out, std::size_t max_bytes) override {
        auto it = files.find(path);
        if (it == files.end() || it->second.size() > max_bytes) return false;
        out = it->second;
        return true;
    }
    bool write_atomic(const std::string& path, std::string_view data) override {
        if (fail_writes) return false;
        ++atomic_writes;
        files[path] = std::string(data);
        return true;
    }
    bool append(const std::string& path, std::string_view data) override {
        if (fail_writes) return false;
        ++appends;
        files[path] += std::string(data);
        return true;
    }
};

inline std::size_t count_lines(const std::string& s) {
    std::size_t n = 0;
    for (char c : s)
        if (c == '\n') ++n;
    return n;
}

inline opensweep::telemetry::ProcessSample proc(std::uint32_t pid, const char* name, double cpu, double priv_mb,
                                                std::uint32_t handles = 500, std::uint32_t threads = 20) {
    opensweep::telemetry::ProcessSample p;
    p.pid = pid;
    p.name = name;
    p.cpu_percent = cpu;
    p.private_bytes = static_cast<std::uint64_t>(priv_mb * kMB);
    p.working_set = p.private_bytes;
    p.handles = handles;
    p.threads = threads;
    return p;
}

// A plausible, noisy "normal" sample of an idle-ish desktop.
class NormalGen {
public:
    explicit NormalGen(unsigned seed = 42) : rng_(seed) {}

    double u(double lo, double hi) { return std::uniform_real_distribution<double>(lo, hi)(rng_); }
    double n(double mean, double sd) { return std::normal_distribution<double>(mean, sd)(rng_); }

    opensweep::telemetry::Sample sample(std::int64_t ts) {
        opensweep::telemetry::Sample s;
        s.ts_ms = ts;
        s.cpu_percent = u(3.0, 30.0);
        s.cpu_queue_length = u(0.0, 2.0);
        s.context_switches_per_sec = n(8000, 800);
        s.dpc_percent = std::max(0.0, n(0.5, 0.15));
        s.interrupt_percent = std::max(0.0, n(0.3, 0.1));
        s.mem_total = 16 * kGBu;
        s.mem_avail = 8 * kGBu;
        s.mem_load_percent = static_cast<std::uint32_t>(n(50, 1.5));
        s.commit_limit = 20 * kGBu;
        s.commit_total = static_cast<std::uint64_t>(n(10.0, 0.1) * static_cast<double>(kGBu));
        s.system_cache = 2 * kGBu;
        s.kernel_paged = static_cast<std::uint64_t>(n(500, 4) * kMB);
        s.kernel_nonpaged = static_cast<std::uint64_t>(n(300, 3) * kMB);
        s.hard_faults_per_sec = std::max(0.0, n(5, 3));
        s.page_faults_per_sec = std::max(0.0, n(2000, 300));
        s.disk_read_latency_ms = std::max(0.1, n(2.0, 0.8));
        s.disk_write_latency_ms = std::max(0.1, n(3.0, 1.0));
        s.disk_queue_length = std::max(0.0, n(0.2, 0.1));
        s.disk_bytes_per_sec = std::max(0.0, n(2e6, 5e5));
        s.cpu_freq_percent = n(100, 4);
        s.processes = 200;
        s.threads = static_cast<std::uint32_t>(n(3000, 30));
        s.handles = static_cast<std::uint32_t>(n(100000, 800));
        s.top.push_back(proc(4, "System", u(0.1, 1.0), 0.2, 3000, 150));
        s.top.push_back(proc(1200, "explorer.exe", u(0.0, 2.0), n(100, 1), 2500, 60));
        s.top.push_back(proc(3000, "chrome.exe", u(0.5, 6.0), n(800, 25), 4000, 40));
        return s;
    }

private:
    std::mt19937 rng_;
};

}  // namespace obs_test
