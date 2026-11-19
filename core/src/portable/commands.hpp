// Phase 1 command handlers (hello, health, metrics, topology), written against an abstract
// SystemProvider so the result shapes can be unit-tested on any platform. The Windows build
// supplies a provider backed by documented Win32 APIs (src/win/).
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "json.hpp"
#include "protocol.hpp"

namespace opensweep::commands {

inline constexpr const char* kServiceName = "OpenSweepCore";
inline constexpr const char* kVersion = "0.1.0";
inline constexpr const char* kMode = "monitor";  // Phase 1 never changes Windows policy

struct MemoryMetrics {
    std::uint64_t total_phys = 0;
    std::uint64_t avail_phys = 0;
    std::uint32_t load_percent = 0;
    std::uint64_t commit_total = 0;  // bytes
    std::uint64_t commit_limit = 0;
    std::uint64_t commit_peak = 0;
    std::uint64_t system_cache = 0;
    std::uint64_t kernel_paged = 0;
    std::uint64_t kernel_nonpaged = 0;
};

struct MetricsSnapshot {
    std::int64_t timestamp_ms = 0;  // Unix epoch milliseconds (UTC)
    double cpu_percent = 0.0;       // 0..100 over the last sampling interval
    MemoryMetrics memory;
    std::uint32_t processes = 0;
    std::uint32_t threads = 0;
    std::uint32_t handles = 0;
    std::uint64_t page_size = 0;
};

struct TopologyInfo {
    std::uint32_t logical_processors = 0;
    std::uint32_t cores = 0;
    std::uint32_t packages = 0;
    std::uint32_t numa_nodes = 0;
    std::uint32_t groups = 0;
    // efficiency_classes[k] = number of physical cores whose EfficiencyClass is k.
    std::vector<std::uint32_t> efficiency_classes;
    std::uint32_t l3_caches = 0;
    // More than one distinct efficiency class present (e.g. P-cores + E-cores).
    bool hybrid() const noexcept;
};

struct SelfProcessInfo {
    std::uint64_t working_set = 0;
    std::uint64_t private_bytes = 0;
    std::uint64_t cpu_time_ms = 0;
};

// Implemented per platform. Methods return false when the data could not be collected;
// the handler then reports `internal`.
class SystemProvider {
public:
    virtual ~SystemProvider() = default;
    virtual bool metrics(MetricsSnapshot& out) = 0;
    virtual bool topology(TopologyInfo& out) = 0;
    virtual bool self_process(SelfProcessInfo& out) = 0;
    virtual std::uint32_t pid() = 0;
};

// Counters shared between the IPC server and the health command.
struct ServerCounters {
    std::atomic<std::uint64_t> requests{0};
    std::atomic<std::uint64_t> errors{0};
    std::atomic<std::uint32_t> clients{0};
};

struct ServiceInfo {
    std::int64_t started_at_ms = 0;  // Unix epoch milliseconds (UTC)
    std::chrono::steady_clock::time_point started_steady = std::chrono::steady_clock::now();
};

// Formats Unix epoch milliseconds as ISO 8601 UTC, e.g. "2026-11-18T09:30:00.123Z".
std::string format_iso8601_utc(std::int64_t unix_ms);

// Current wall clock as Unix epoch milliseconds.
std::int64_t now_unix_ms();

// `mode` is the engine's current operating mode ("monitor" | "balanced" | "advanced").
json::Value build_hello(const ServiceInfo& info, std::uint32_t pid, const char* mode = kMode);
json::Value build_health(const ServiceInfo& info, const SelfProcessInfo& self, const ServerCounters& counters,
                         const char* mode = kMode);
json::Value build_metrics(const MetricsSnapshot& m);
json::Value build_topology(const TopologyInfo& t);

// Returns the current mode string (static storage). Empty function = always kMode.
using ModeProvider = std::function<const char*()>;

// Registers hello/health/metrics/topology (all require Capability::Read). The referenced
// objects must outlive the dispatcher. `mode` feeds hello.mode / health.mode.
void register_phase1(protocol::Dispatcher& dispatcher, SystemProvider& provider, const ServiceInfo& info,
                     const ServerCounters& counters, ModeProvider mode = {});

}  // namespace opensweep::commands
