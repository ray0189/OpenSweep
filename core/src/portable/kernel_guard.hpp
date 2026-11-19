// SweepGuard additions fed by kernel tracing (docs/core/etw.md "Detection"). Portable.
//
// Detectors (detector id -> rule; all thresholds in KernelGuardConfig):
//   driver_latency  per driver, per wall-clock minute: the driver's longest DPC/ISR >= 1000 us in
//                   >= 3 of the last 10 minutes, or its DPC/ISR share of all CPUs far above its own
//                   baseline (>= 30 minutes of history; share >= max(0.5 %, 4 x median, median +
//                   0.3) in each of the last 2 minutes). Opens immediately on that (already
//                   windowed) condition, resolves after 5 minutes without it. Recommendations for
//                   the user only (update / roll back the driver, check power settings); never an
//                   automatic action and never a policy candidate. "unknown" routines are ignored.
//   disk_hog        per process: a non-foreground process issuing >= 20 MB/s of disk I/O for 2 min
//                   while disk latency is above baseline (robust z >= 3 and >= 5 ms; >= 20 ms while
//                   the baseline is not ready). Resolves after 2 min below half the rate or with
//                   normal latency. While it holds: a Balanced-mode candidate on that process if it
//                   is a verified background workload (EcoQoS first, then BELOW_NORMAL).
//   paging_hog      per process: during memory pressure (SweepMemory level >= elevated), one
//                   process accounts for >= 50 % of the attributed hard faults and >= 100 faults/s
//                   for 60 s. Resolves after 2 min below 30 % or without pressure. The process is
//                   added as a contributor to open commit_exhaustion / memory_growth /
//                   storage_latency incidents; candidate as above (memory priority first, then
//                   EcoQoS) when it is a verified background workload.
// Candidates go through the normal control engine (gates, hysteresis, cooldown, rate limit,
// journal, verification, rollback; Monitor mode = simulation only). Foreground and protected
// processes (sched::protection_reason) are never candidates. Incidents say "correlated with".
// State is bounded: 64 tracked drivers, 64 tracked processes.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "baseline.hpp"
#include "incident.hpp"
#include "memory_policy.hpp"
#include "policy_types.hpp"
#include "scheduler_policy.hpp"
#include "telemetry.hpp"

namespace opensweep::kguard {

inline constexpr std::size_t kMaxTrackedDrivers = 64;
inline constexpr std::size_t kMaxTrackedProcs = 64;

struct KernelGuardConfig {
    unsigned logical_processors = 1;

    // driver_latency
    double driver_spike_us = 1000.0;
    std::size_t driver_spike_minutes = 3;
    std::size_t driver_lookback_minutes = 10;
    double driver_share_min_percent = 0.5;
    double driver_share_ratio = 4.0;
    double driver_share_min_excess = 0.3;
    std::size_t driver_share_ready_minutes = 30;
    std::size_t driver_share_minutes = 2;
    std::int64_t driver_resolve_ms = 5 * 60'000;
    std::int64_t driver_forget_ms = 15 * 60'000;

    // disk_hog
    double disk_hog_bps = 20.0 * 1024 * 1024;
    std::int64_t disk_hog_open_ms = 2 * 60'000;
    std::int64_t disk_hog_resolve_ms = 2 * 60'000;
    double disk_latency_z = 3.0;
    double disk_latency_min_ms = 5.0;
    double disk_latency_floor_ms = 20.0;   // used while the latency baseline is not ready
    double disk_latency_min_mad_ms = 1.0;

    // paging_hog
    double paging_min_share = 0.5;
    double paging_resolve_share = 0.3;
    double paging_min_faults = 100.0;
    std::int64_t paging_open_ms = 60'000;
    std::int64_t paging_resolve_ms = 2 * 60'000;

    std::int64_t max_gap_ms = 10'000;
    std::int64_t process_forget_ms = 5 * 60'000;
    std::int64_t merge_window_ms = 10 * 60'000;
    sched::ClassifierLists lists;
};

struct KernelInputs {
    memory::PressureLevel memory_level = memory::PressureLevel::Low;
    std::vector<std::uint32_t> ecoqos_active;           // EcoQoS currently applied by OpenSweep
    std::vector<std::uint32_t> memory_priority_active;  // memory priority currently applied by OpenSweep
};

struct KernelAssessment {
    std::vector<policy::Recommendation> recommendations;
    std::vector<std::string> opened;   // incident ids created / re-opened by this evaluation
    std::vector<std::string> notes;    // why a hog was not made a candidate (bounded)
};

class KernelGuard {
public:
    explicit KernelGuard(KernelGuardConfig cfg = {});
    ~KernelGuard();
    KernelGuard(const KernelGuard&) = delete;
    KernelGuard& operator=(const KernelGuard&) = delete;

    // Once per sample, after Guard::evaluate and SweepMemory::assess. Samples without kernel data
    // (s.kernel_valid == false) leave the state untouched.
    KernelAssessment evaluate(const telemetry::Sample& s, const baseline::BaselineSet& baselines,
                              incident::IncidentStore& store, const KernelInputs& in);

    std::size_t tracked_drivers() const noexcept;
    std::size_t tracked_processes() const noexcept;
    const KernelGuardConfig& config() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace opensweep::kguard
