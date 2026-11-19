// Kernel tracing aggregation (docs/core/etw.md "Aggregation"). Portable: no Windows headers.
//
// The Windows ETW consumer (src/win/etw.cpp) decodes SystemTraceProvider events and feeds them
// through the on_*() methods below; tests inject synthetic events the same way. Nothing is stored
// raw: every event is folded into bounded accumulators immediately.
//
//   * Address -> driver: kernel image load / rundown events give [base, base + size) ranges of
//     driver files (file name only). A DPC/ISR routine address is looked up in that sorted range
//     table (binary search); unmapped -> "unknown".
//   * Thread -> process (thread start / rundown) and process -> image name (process start /
//     rundown) attribute disk I/O (issuing thread) and hard faults (faulting thread).
//   * 1-minute windows (aligned to wall-clock minutes); the last 60 closed windows are kept. A closed
//     window keeps only the top 64 keys per table (drivers by total time, processes by bytes /
//     faults); the rest is counted in `keys_dropped`. The open window and the per-sample ("tick")
//     accumulator are bounded to kMaxOpenKeys keys each (overflow goes to "other").
//   * Histogram buckets (us): <50, <100, <250, <500, <1000, <2000, <4000, >=4000.
//   * System DPC % / ISR % = summed execution time / (elapsed x logical processors) x 100.
//
// SelfProtection implements the cost / loss rule of the contract (> 2 % of one CPU over 30 s, or
// events lost continuously for 30 s -> restart with fewer flags: DPC/ISR, then disk, then faults).
//
// Thread safety: none. The Windows session serialises access with its own mutex.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "json.hpp"
#include "telemetry.hpp"

namespace opensweep::kstats {

inline constexpr std::size_t kHistogramBuckets = 8;
// Exclusive upper bounds of the first 7 buckets; the last bucket is >= 4000 us.
inline constexpr std::array<double, kHistogramBuckets - 1> kBucketUpperUs = {50, 100, 250, 500, 1000, 2000, 4000};
std::size_t histogram_bucket(double us) noexcept;

inline constexpr std::int64_t kWindowMs = 60'000;
inline constexpr std::size_t kMaxWindows = 60;          // 1 h of closed windows
inline constexpr std::size_t kTopKeysPerWindow = 64;
inline constexpr std::size_t kMaxOpenKeys = 2048;       // per table of the open window / tick accumulator
inline constexpr std::size_t kMaxImages = 4096;
inline constexpr std::size_t kMaxDriverNames = 2048;
inline constexpr std::size_t kMaxThreads = 65536;
inline constexpr std::size_t kMaxProcesses = 8192;
inline constexpr double kMaxPlausibleUs = 1'000'000.0;  // a single DPC/ISR > 1 s is a decoding error
inline constexpr double kMaxPlausibleLatencyMs = 600'000.0;
inline constexpr std::uint32_t kOtherPid = 0xFFFFFFFFu; // overflow bucket key

inline constexpr const char* kUnknownName = "unknown";
inline constexpr const char* kOtherName = "other";

enum class Routine { Dpc, Isr };

// SystemTraceProvider enable flags (portable copies of the documented EVENT_TRACE_FLAG_* values).
namespace flag {
inline constexpr std::uint32_t kProcess = 0x00000001;
inline constexpr std::uint32_t kThread = 0x00000002;
inline constexpr std::uint32_t kImageLoad = 0x00000004;
inline constexpr std::uint32_t kDpc = 0x00000020;
inline constexpr std::uint32_t kInterrupt = 0x00000040;
inline constexpr std::uint32_t kDiskIo = 0x00000100;
inline constexpr std::uint32_t kHardFaults = 0x00002000;
}  // namespace flag

// Degradation levels: 0 everything; 1 without DPC/ISR; 2 also without disk; 3 also without hard
// faults (nothing useful left: the session is not started at this level).
inline constexpr int kMaxLevel = 3;
std::uint32_t flags_for_level(int level) noexcept;
// Contract names: PROCESS, THREAD, IMAGE_LOAD, DPC, INTERRUPT, DISK_IO, HARD_FAULTS.
std::vector<std::string> flag_names(std::uint32_t flags);

// Plain-language class of well-known drivers ("ndis.sys" -> "network driver"); empty if unknown.
std::string driver_description(std::string_view file_name);

struct DriverStats {
    std::string name;
    std::uint64_t dpc_count = 0;
    std::uint64_t isr_count = 0;
    double dpc_us = 0.0;
    double isr_us = 0.0;
    double max_us = 0.0;
    std::array<std::uint64_t, kHistogramBuckets> histogram{};
    double total_us() const noexcept { return dpc_us + isr_us; }
    std::uint64_t count() const noexcept { return dpc_count + isr_count; }
    void merge(const DriverStats& o);
};

struct ProcessDisk {
    std::uint32_t pid = 0;
    std::string name;
    std::uint64_t read_bytes = 0;
    std::uint64_t write_bytes = 0;
    std::uint64_t ios = 0;
    double latency_sum_ms = 0.0;
    double max_latency_ms = 0.0;
    void merge(const ProcessDisk& o);
};

struct ProcessFaults {
    std::uint32_t pid = 0;
    std::string name;
    std::uint64_t faults = 0;
};

struct Window {
    std::int64_t start_ms = 0;
    std::int64_t end_ms = 0;   // exclusive; for the open window: the latest activity
    double dpc_us = 0.0;
    double isr_us = 0.0;
    double worst_dpc_us = 0.0;
    double worst_isr_us = 0.0;
    std::uint64_t dpc_count = 0;
    std::uint64_t isr_count = 0;
    std::uint64_t hard_faults = 0;
    std::uint64_t disk_bytes = 0;
    std::uint64_t keys_dropped = 0;
    std::vector<DriverStats> drivers;  // <= kTopKeysPerWindow, largest total first
    std::vector<ProcessDisk> disk;     // <= kTopKeysPerWindow, most bytes first
    std::vector<ProcessFaults> faults; // <= kTopKeysPerWindow, most faults first
};

struct LatencyView {
    double dpc_percent = 0.0;
    double isr_percent = 0.0;
    double worst_dpc_us = 0.0;
    double worst_isr_us = 0.0;
    std::int64_t covered_ms = 0;
    std::vector<DriverStats> drivers;  // worst (max) first
};

struct DiskProcessView {
    std::uint32_t pid = 0;
    std::string name;
    double read_bps = 0.0;
    double write_bps = 0.0;
    std::uint64_t ios = 0;
    double mean_latency_ms = 0.0;
    double max_latency_ms = 0.0;
};

struct FaultProcessView {
    std::uint32_t pid = 0;
    std::string name;
    double faults_per_sec = 0.0;
};

struct Counters {
    std::uint64_t events = 0;            // decoded events fed in
    std::uint64_t dpc_isr = 0;
    std::uint64_t disk = 0;
    std::uint64_t hard_faults = 0;
    std::uint64_t malformed = 0;         // rejected by the decoder or by plausibility checks
    std::uint64_t unknown_routine = 0;   // DPC/ISR routine not in any known image range
    std::uint64_t unattributed = 0;      // disk / fault events whose thread is unknown
    std::uint64_t map_resets = 0;        // bounded maps cleared because they were full
};

struct Config {
    unsigned logical_processors = 1;
    std::size_t top_keys = kTopKeysPerWindow;
    std::size_t windows = kMaxWindows;
};

class KernelStats {
public:
    explicit KernelStats(Config cfg = {});

    // ---- decoded events (ts_ms = Unix epoch ms of the event) -------------------------------------
    void on_image_load(std::uint64_t base, std::uint64_t size, std::string_view file_name);
    void on_image_unload(std::uint64_t base);
    void on_thread_start(std::uint32_t tid, std::uint32_t pid);
    void on_thread_end(std::uint32_t tid);
    void on_process_start(std::uint32_t pid, std::string_view image_name);
    void on_process_end(std::uint32_t pid);
    void on_dpc_isr(Routine kind, std::uint64_t routine, double duration_us, std::int64_t ts_ms);
    // `fallback_pid`: event-header process id when the issuing thread is unknown (0xFFFFFFFF = none).
    void on_disk_io(std::uint32_t issuing_tid, std::uint32_t fallback_pid, std::uint64_t bytes, bool write,
                    double latency_ms, std::int64_t ts_ms);
    void on_hard_fault(std::uint32_t tid, std::uint32_t fallback_pid, std::int64_t ts_ms);
    void on_malformed() noexcept { ++counters_.malformed; }
    void set_events_lost(std::uint64_t total) noexcept { events_lost_ = total; }

    // Optional: resolves a pid without a process event (e.g. started before the session and missed
    // by the rundown). Called at most once per pid until the process map is reset.
    void set_name_resolver(std::function<std::string(std::uint32_t)> r) { resolver_ = std::move(r); }

    // Closes windows up to `now_ms` (also done by every event and query).
    void advance(std::int64_t now_ms);

    // ---- queries -----------------------------------------------------------------------------
    LatencyView latency(std::int64_t now_ms, std::int64_t span_ms = 10 * 60'000, std::size_t top = 20);
    std::vector<DiskProcessView> disk_activity(std::int64_t now_ms, std::int64_t span_ms = 2 * 60'000,
                                               std::size_t top = 20);
    std::vector<FaultProcessView> hard_faults(std::int64_t now_ms, std::int64_t span_ms = 2 * 60'000,
                                              std::size_t top = 20);
    // Fills the kernel fields of `s` with the activity since the previous call (rates over that
    // interval) and starts a new interval. `running` / `cost_percent` come from the session.
    void fill_sample(std::int64_t now_ms, telemetry::Sample& s, bool running, double cost_percent);

    // Driver name for an address ("unknown" when unmapped).
    std::string driver_for(std::uint64_t address) const;
    std::uint32_t pid_for_thread(std::uint32_t tid) const;   // 0xFFFFFFFF when unknown
    std::string process_name(std::uint32_t pid);             // resolves (once) when unknown

    const Counters& counters() const noexcept { return counters_; }
    std::uint64_t events_lost() const noexcept { return events_lost_; }
    std::size_t images() const noexcept { return images_.size(); }
    std::size_t kernel_images_named() const noexcept { return names_.size(); }
    std::size_t threads() const noexcept { return threads_.size(); }
    std::size_t processes() const noexcept { return procs_.size(); }
    std::size_t closed_windows() const noexcept { return windows_.size(); }
    const std::deque<Window>& windows() const noexcept { return windows_; }
    const Config& config() const noexcept { return cfg_; }
    // Forgets threads/processes/images (session restart: the rundown refills them).
    void reset_maps();

private:
    struct Acc {
        std::int64_t start_ms = -1;
        std::int64_t last_ms = -1;
        double dpc_us = 0.0, isr_us = 0.0, worst_dpc_us = 0.0, worst_isr_us = 0.0;
        std::uint64_t dpc_count = 0, isr_count = 0, hard_faults = 0, disk_bytes = 0, dropped = 0;
        std::unordered_map<std::uint32_t, DriverStats> drivers;   // by driver id
        std::unordered_map<std::uint32_t, ProcessDisk> disk;      // by pid
        std::unordered_map<std::uint32_t, ProcessFaults> faults;  // by pid
        void clear();
    };
    struct Image {
        std::uint64_t base = 0;
        std::uint64_t end = 0;  // exclusive
        std::uint32_t id = 0;   // driver name id
    };

    std::uint32_t driver_id(std::string_view file_name);
    std::uint32_t lookup(std::uint64_t address) const;  // driver id, 0 = unknown
    std::uint32_t attribute(std::uint32_t tid, std::uint32_t fallback_pid);
    void add_dpc_isr(Acc& a, std::uint32_t id, Routine kind, double us);
    void add_disk(Acc& a, std::uint32_t pid, std::uint64_t bytes, bool write, double latency_ms);
    void add_fault(Acc& a, std::uint32_t pid);
    void close_window();
    void note_time(std::int64_t ts_ms);
    Window to_window(const Acc& a, std::int64_t end_ms) const;
    // Closed windows overlapping [now - span, now] plus the open window, merged.
    Window merged(std::int64_t now_ms, std::int64_t span_ms, std::int64_t& covered_ms);

    Config cfg_;
    std::vector<std::string> names_;                           // id -> display name (id 0 = unknown)
    std::unordered_map<std::string, std::uint32_t> name_ids_;  // lower-case name -> id
    std::vector<Image> images_;                                // sorted by base, non-overlapping
    std::unordered_map<std::uint32_t, std::uint32_t> threads_; // tid -> pid
    std::unordered_map<std::uint32_t, std::string> procs_;     // pid -> image name
    std::function<std::string(std::uint32_t)> resolver_;
    Acc cur_;
    Acc tick_;
    std::deque<Window> windows_;
    Counters counters_;
    std::uint64_t events_lost_ = 0;
    std::int64_t first_ms_ = -1;  // first activity: rates are never computed over time before it
};

// ---------------------------------------------------------------------------------------------
// Self-protection of the kernel session (cost / loss rule). Fed about once per second with the
// consumer thread's cumulative CPU time and the session's cumulative lost-event count.
struct ProtectionConfig {
    double max_cost_percent = 2.0;          // of one CPU
    std::int64_t cost_window_ms = 30'000;
    std::int64_t loss_window_ms = 30'000;   // lost events in every check for this long -> degrade
};

class SelfProtection {
public:
    enum class Verdict { Ok, TooExpensive, LosingEvents };
    explicit SelfProtection(ProtectionConfig cfg = {}) : cfg_(cfg) {}
    // Call after every (re)start of the session.
    void reset() noexcept;
    Verdict update(std::int64_t now_ms, std::uint64_t cpu_us_total, std::uint64_t lost_total);
    // Rolling cost over the window (% of one CPU); 0 until two samples exist.
    double cost_percent() const noexcept { return cost_; }
    const ProtectionConfig& config() const noexcept { return cfg_; }

private:
    struct Point {
        std::int64_t t;
        std::uint64_t cpu;
    };
    ProtectionConfig cfg_;
    std::deque<Point> points_;
    double cost_ = 0.0;
    bool have_lost_ = false;
    std::uint64_t last_lost_ = 0;
    std::int64_t losing_since_ = -1;
};
const char* to_string(SelfProtection::Verdict v) noexcept;

// ---------------------------------------------------------------------------------------------
// Session status as reported over IPC.
struct EtwStatus {
    bool enabled = true;             // config.json `etw` / setEtw
    bool running = false;
    std::uint32_t flags = 0;         // currently enabled SystemTraceProvider flags
    int level = 0;                   // degradation level (0 = all flags)
    std::uint64_t events_lost = 0;
    std::uint64_t events = 0;        // decoded events since the service started
    double cost_percent = 0.0;
    std::string reason;              // why it is not running / was degraded; empty when healthy
};

// JSON builders (docs/core/etw.md "IPC additions"; camelCase).
json::Value etw_status_json(const EtwStatus& st);   // { running, flags[], eventsLost, costPercent, reason?, enabled(+), level(+), events(+) }
json::Value latency_json(const EtwStatus& st, const LatencyView& v);
json::Value disk_activity_json(const std::vector<DiskProcessView>& v);
json::Value hard_faults_json(const std::vector<FaultProcessView>& v);
json::Value driver_json(const DriverStats& d);

}  // namespace opensweep::kstats
