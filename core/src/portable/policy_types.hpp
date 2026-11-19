// Types shared by the Phase 4/5 policy modules (memory_policy, scheduler_policy, control).
// Portable: no Windows headers. Owner: engine-act.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "telemetry.hpp"

namespace opensweep::policy {

// Disk: candidates from the kernel-tracing disk_hog detector (verified by disk latency).
enum class Area { Memory, Cpu, Disk };

inline const char* to_string(Area a) noexcept {
    switch (a) {
        case Area::Memory: return "memory";
        case Area::Disk: return "disk";
        case Area::Cpu: break;
    }
    return "cpu";
}

// Win32 priority class values (portable copies; the actuator uses the real constants).
inline constexpr std::uint32_t kPriorityIdle = 0x40;
inline constexpr std::uint32_t kPriorityBelowNormal = 0x4000;
inline constexpr std::uint32_t kPriorityNormal = 0x20;
inline constexpr std::uint32_t kPriorityAboveNormal = 0x8000;
inline constexpr std::uint32_t kPriorityHigh = 0x80;
inline constexpr std::uint32_t kPriorityRealtime = 0x100;

// ProcessMemoryPriority values (MEMORY_PRIORITY_VERY_LOW = 1 .. MEMORY_PRIORITY_NORMAL = 5).
inline constexpr std::int64_t kMemoryPriorityLow = 2;
inline constexpr std::int64_t kMemoryPriorityNormal = 5;

// A proposed, reversible intervention. Produced by SweepMemory / SweepScheduler, consumed by the
// Adaptive Control Engine, which decides whether (and how) it is applied.
struct Recommendation {
    std::string id;  // stable per (area, kind, pid): "cpu:ecoqos:1234"
    Area area = Area::Cpu;
    telemetry::PolicyTarget target;
    telemetry::WorkloadClass workload = telemetry::WorkloadClass::Unknown;
    telemetry::PolicyKind kind = telemetry::PolicyKind::EcoQoS;
    std::int64_t value = 0;
    std::uint32_t beneficiary_pid = 0;  // foreground process this is meant to help (0 = none)
    int risk = 1;                       // 1 (lowest) .. 5
    std::string reason;
    std::vector<std::string> evidence;
    std::string expected_effect;
};

// Metrics compared before/after an intervention. A negative value means "unknown".
struct VerificationMetrics {
    double fg_latency_ms = -1.0;   // foreground responsiveness proxy (e.g. input/message latency)
    double hard_faults = -1.0;     // hard faults per second
    double cpu_queue = -1.0;       // processor queue length per logical CPU
    double commit_percent = -1.0;  // commit charge / commit limit * 100
    double disk_latency_ms = -1.0; // max(avg read, avg write) disk latency (appended; Area::Disk primary)
};

inline VerificationMetrics verification_metrics(const telemetry::Sample& s, std::uint32_t logical_count,
                                                std::optional<double> fg_latency_ms) {
    VerificationMetrics m;
    if (fg_latency_ms && *fg_latency_ms >= 0.0) m.fg_latency_ms = *fg_latency_ms;
    m.hard_faults = s.hard_faults_per_sec >= 0.0 ? s.hard_faults_per_sec : -1.0;
    const double logical = logical_count > 0 ? static_cast<double>(logical_count) : 1.0;
    m.cpu_queue = s.cpu_queue_length >= 0.0 ? s.cpu_queue_length / logical : -1.0;
    if (s.commit_limit > 0)
        m.commit_percent = static_cast<double>(s.commit_total) * 100.0 / static_cast<double>(s.commit_limit);
    const double lat = s.disk_read_latency_ms > s.disk_write_latency_ms ? s.disk_read_latency_ms : s.disk_write_latency_ms;
    m.disk_latency_ms = lat >= 0.0 ? lat : -1.0;  // NaN compares false -> unknown
    return m;
}

}  // namespace opensweep::policy
