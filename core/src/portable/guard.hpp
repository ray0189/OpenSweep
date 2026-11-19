// SweepGuard: statistical stability detectors (Phase 3, Part 7).
//
// Guard::evaluate() runs once per sample (2 s) after Trace.append and Baselines.update. It keeps its
// own small, fixed-size state (per-process minute series for <= kMaxTrackedProcesses processes, a
// 60-minute system series, bounded event counters) and turns sustained, attributed anomalies into
// Incidents in the IncidentStore. Every detector:
//   * uses explicit thresholds (GuardConfig, documented below),
//   * opens only after the condition held for a sustained period and resolves only after a
//     sustained recovery (Hysteresis; a gap > max_gap_ms between samples, e.g. sleep/resume,
//     restarts the timers),
//   * merges repeats into the same incident (IncidentStore key = detector + subject),
//   * reports contributors as *correlated*, never as causes,
//   * scores confidence from evidence strength, duration, agreeing signals and baseline readiness
//     (incident::compute_confidence; low when the relevant baseline is not ready yet).
//
// Detectors (detector id -> rule):
//   memory_growth        per process, minute series of private bytes, last <= 30 min: Theil-Sen slope
//                        >= 2 MB/min, growth >= 50 MB, >= 70 % of minute steps increasing, no fall from
//                        a running peak larger than 25 % of the growth (sawtooth guard), span >= 10
//                        min. "suspected leak" once >= 90 % increasing over >= 20 min. Resolves after
//                        10 min with slope < 0.5 MB/min, or when the process is gone.
//   commit_exhaustion    commit % >= 90, or >= 80 with linear time-to-100 % <= 30 min (10-min slope),
//                        for 60 s. Resolves below 75 % for 5 min. Contributors: processes whose private
//                        bytes moved with commit (Pearson r >= 0.6, growth > 0).
//   cpu_runaway          a non-foreground process >= 90 % of one logical processor for 5 min.
//                        Resolves below 50 % (or foreground) for 2 min.
//   handle_growth /      per process: handles >= 10 000 growing >= 200/min (threads >= 500, >= 10/min),
//   thread_growth        Theil-Sen + monotonicity over >= 10 min.
//   handle_exhaustion /  system totals: baseline ready, robust z >= 5 and >= 1.5 x median, 5 min.
//   thread_exhaustion
//   kernel_pool          paged / nonpaged: (baseline ready, z >= 5, excess >= max(256 MB, 50 % of
//                        median)) or absolute >= 6 GB paged / 3 GB nonpaged; 5 min.
//   storage_latency      max(read, write) latency >= 50 ms and (baseline not ready or z >= 4); 20 s.
//                        Resolves after 60 s. Contributors: processes with hard faults in that sample.
//   dpc_isr              DPC or interrupt % >= 5 and (baseline not ready or z >= 5); 30 s.
//   thermal_throttling   CPU >= 50 % busy and processor performance <= 75 % of its busy-time median
//                        (or <= 60 % before that baseline is ready) for 2 min.
//   boot_degradation     BootPerformance duration >= 1.5 x boot median, >= +10 s and z >= 3
//                        (boot baseline ready: >= 3 boots).
//   event rules          service_crash_loop (>= 3 ServiceFailure, same service, 1 h),
//                        app_reliability (>= 3 AppCrash/AppHang, same app, 1 h),
//                        disk_deterioration (>= 3 DiskError same disk in 24 h, or any critical one),
//                        driver_failure, gpu_timeout, whea_error, unexpected_shutdown, bugcheck,
//                        resource_exhaustion, thermal_event (first event; severity escalates on
//                        repeats). Event incidents resolve after a quiet period (1 h .. 7 d).
// Level-detector incidents that are not refreshed for 30 min are resolved (e.g. after a restart).
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "baseline.hpp"
#include "incident.hpp"
#include "telemetry.hpp"

namespace opensweep::guard {

using telemetry::EventRecord;
using telemetry::Sample;

// Two-threshold hysteresis: `on` must hold for open_after_ms to open; once open, `off` must hold for
// resolve_after_ms to resolve. Samples further apart than max_gap_ms restart both timers (the open
// state is kept).
class Hysteresis {
public:
    enum class Edge { None, Opened, Resolved };
    Hysteresis(std::int64_t open_after_ms = 0, std::int64_t resolve_after_ms = 0, std::int64_t max_gap_ms = 10'000)
        : open_after_(open_after_ms), resolve_after_(resolve_after_ms), max_gap_(max_gap_ms) {}
    Edge update(bool on, bool off, std::int64_t now_ms) noexcept;
    Edge update(bool condition, std::int64_t now_ms) noexcept { return update(condition, !condition, now_ms); }
    bool is_open() const noexcept { return open_; }
    // Start of the current `on` run, -1 if none.
    std::int64_t on_since() const noexcept { return on_since_; }
    void force_closed() noexcept;
    void reset() noexcept;

private:
    std::int64_t open_after_;
    std::int64_t resolve_after_;
    std::int64_t max_gap_;
    std::int64_t on_since_ = -1;
    std::int64_t off_since_ = -1;
    std::int64_t last_ = -1;
    bool open_ = false;
};

constexpr std::int64_t kMinute = 60'000;
constexpr std::int64_t kHour = 60 * kMinute;
constexpr std::int64_t kDay = 24 * kHour;
inline constexpr std::size_t kMaxTrackedProcesses = 64;
inline constexpr std::size_t kProcessMinutes = 60;

struct GuardConfig {
    // 0 = unknown: process cpu_percent (share of the machine) is then used as "% of one core",
    // which under-reports on multi-core machines. The engine sets this from the topology.
    unsigned logical_processors = 0;

    // memory_growth
    double leak_min_slope_mb_per_min = 2.0;
    double leak_min_growth_mb = 50.0;
    std::size_t leak_min_minutes = 10;
    std::size_t leak_window_minutes = 30;
    double leak_min_increasing = 0.7;
    double leak_suspect_increasing = 0.9;
    std::size_t leak_suspect_minutes = 20;
    double leak_resolve_slope_mb_per_min = 0.5;
    // Largest fall from a running peak inside the window, as a fraction of the window's net growth.
    // Rejects garbage-collector / cache sawtooth patterns (grow, release, grow ...).
    double growth_max_drawdown = 0.25;
    std::int64_t leak_resolve_ms = 10 * kMinute;

    // commit_exhaustion
    double commit_high_percent = 90.0;
    double commit_warn_percent = 80.0;
    double commit_tte_minutes = 30.0;
    double commit_resolve_percent = 75.0;
    std::int64_t commit_open_ms = kMinute;
    std::int64_t commit_resolve_ms = 5 * kMinute;

    // cpu_runaway
    double runaway_core_percent = 90.0;
    double runaway_resolve_core_percent = 50.0;
    std::int64_t runaway_open_ms = 5 * kMinute;
    std::int64_t runaway_resolve_ms = 2 * kMinute;

    // handle_growth / thread_growth (per process)
    double handle_min_count = 10'000;
    double handle_min_slope_per_min = 200;
    double thread_min_count = 500;
    double thread_min_slope_per_min = 10;
    std::size_t growth_min_minutes = 10;
    std::int64_t growth_resolve_ms = 10 * kMinute;

    // handle_exhaustion / thread_exhaustion (system), kernel_pool
    double system_z = 5.0;
    double system_ratio = 1.5;
    double pool_z = 5.0;
    double pool_min_excess_bytes = 256.0 * 1024 * 1024;
    double pool_min_excess_ratio = 0.5;
    double pool_abs_paged_bytes = 6.0 * 1024 * 1024 * 1024;
    double pool_abs_nonpaged_bytes = 3.0 * 1024 * 1024 * 1024;
    std::int64_t system_open_ms = 5 * kMinute;
    std::int64_t system_resolve_ms = 5 * kMinute;

    // storage_latency
    double latency_floor_ms = 50.0;
    double latency_z = 4.0;
    double latency_min_mad_ms = 1.0;
    double latency_error_ms = 500.0;
    std::int64_t latency_open_ms = 20'000;
    std::int64_t latency_resolve_ms = 60'000;

    // dpc_isr
    double dpc_floor_percent = 5.0;
    double dpc_z = 5.0;
    double dpc_min_mad = 0.25;
    std::int64_t dpc_open_ms = 30'000;
    std::int64_t dpc_resolve_ms = 60'000;

    // thermal_throttling
    double thermal_busy_cpu_percent = 50.0;
    double thermal_freq_ratio = 0.75;
    double thermal_abs_freq_percent = 60.0;
    std::int64_t thermal_open_ms = 2 * kMinute;
    std::int64_t thermal_resolve_ms = 2 * kMinute;

    // boot_degradation
    double boot_ratio = 1.5;
    double boot_min_excess_ms = 10'000;
    double boot_z = 3.0;
    double boot_min_mad_ms = 2'000;

    // event rules
    std::uint32_t service_loop_count = 3;
    std::int64_t service_loop_window_ms = kHour;
    std::uint32_t app_repeat_count = 3;
    std::int64_t app_repeat_window_ms = kHour;
    std::uint32_t disk_error_count = 3;
    std::int64_t disk_error_window_ms = kDay;

    // housekeeping
    std::int64_t max_gap_ms = 10'000;
    std::int64_t process_forget_ms = 5 * kMinute;
    std::int64_t stale_resolve_ms = 30 * kMinute;
    std::int64_t merge_window_ms = 10 * kMinute;
};

// ---------------------------------------------------------------------------------------------
// Stability score (0..100, higher is better). Pure function; weights (points deducted, capped):
//   open incidents: critical 25 each (max 50), error 12 (max 36), warning 4 (max 16), info 1 (max 4)
//   bugchecks + unexpected shutdowns in 7 d: 8 each (max 24)
//   WHEA hardware errors in 7 d: 5 each (max 15)
//   disk errors in 7 d: 3 each (max 12)
//   app crashes + hangs in 24 h: 2 each (max 10)
//   service failures in 24 h: 1 each (max 8)
//   pressure now: commit >= 95 %: 10, >= 85 %: 5; memory load >= 95 %: 5, >= 90 %: 2
// The result is clamped to [0, 100]; every deduction adds a human-readable reason.
struct ScoreInputs {
    std::size_t open_critical = 0;
    std::size_t open_error = 0;
    std::size_t open_warning = 0;
    std::size_t open_info = 0;
    std::size_t crashes_24h = 0;            // AppCrash + AppHang
    std::size_t service_failures_24h = 0;
    std::size_t shutdowns_7d = 0;           // UnexpectedShutdown + Bugcheck
    std::size_t whea_7d = 0;
    std::size_t disk_errors_7d = 0;
    double commit_percent = 0.0;            // NaN/unknown ignored
    double mem_load_percent = 0.0;
};

struct StabilityScore {
    int score = 100;
    std::vector<std::string> reasons;
};

StabilityScore compute_stability_score(const ScoreInputs& in);

// ---------------------------------------------------------------------------------------------
class Guard {
public:
    explicit Guard(GuardConfig cfg = {});
    ~Guard();
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;

    void evaluate(const Sample& s, std::span<const EventRecord> new_events, const baseline::BaselineSet& baselines,
                  incident::IncidentStore& store);

    // Ids of incidents created or re-opened by the last evaluate() (for incident capture).
    const std::vector<std::string>& last_opened() const noexcept;

    // Gathers ScoreInputs from the store and the guard's bounded event history.
    ScoreInputs score_inputs(const Sample& latest, const incident::IncidentStore& store) const;
    StabilityScore score(const Sample& latest, const incident::IncidentStore& store) const {
        return compute_stability_score(score_inputs(latest, store));
    }

    const GuardConfig& config() const noexcept;
    std::size_t tracked_processes() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace opensweep::guard
