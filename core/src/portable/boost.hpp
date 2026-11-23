// Game boost (docs/core/alpha4.md section 2): while the user's foreground app is fullscreen, verified
// background processes that use noticeable CPU get EcoQoS + a lower memory priority, and everything is
// restored when the session ends. Portable: no Windows headers; every change goes through
// telemetry::Actuator and is journaled write-ahead like any other OpenSweep policy.
//
// Session lifecycle (times are sample timestamps, i.e. engine ticks every 2 s):
//   start   boost enabled && the foreground hint (reportForeground { pid, fullscreen:true }) names the
//           sample's foreground process, continuously for >= start_after_ms (5 s)
//   end     the session's game has not been the fullscreen foreground for >= end_after_ms (10 s),
//           the game process exited, set_enabled(false), rollback_all (admin), or service stop.
//           Every end path restores every target (EcoQoS and memory priority back to their originals).
//   crash   recover() at start-up restores whatever the journal says a previous run left applied
//           (transactions with trigger "boost" that are not terminal).
// A rollbackAll during a session suppresses a new session until the game leaves fullscreen.
//
// Target selection (re-evaluated every tick while a session is active, at most max_targets):
//   * average CPU >= min_avg_cpu (1 %) of the whole machine over the last 30 s (15 samples; samples in
//     which the process was not in Sample::top count as 0, which is conservative);
//   * workload verified as Background or Throughput by sched::classify + sched::protection_reason
//     (so never: foreground, audio, anti-cheat, security software, system critical, the built-in
//     protection list, session-0 processes, Unknown / LatencyCritical / Interactive / Efficiency);
//   * never the session's game, the current foreground process, or any descendant of either (walked
//     through parent_pid within the sample; a pid once seen as a descendant stays excluded for the
//     whole session, also if its parent exits);
//   * never session 0, never OpenSweep's own processes, never a process with an unknown start time
//     (PID-reuse guard), never a process the Adaptive Control Engine currently holds a policy on.
// A target that becomes the foreground (or a descendant of it) during the session is restored at once.
//
// Journal: one transaction per (target, kind): kind "policy", trigger "boost", action "ecoqos" /
// "memory_priority", state intent -> applied (-> rolled_back | expired | failed at the end).
// Thread safety: none; the engine calls it with its mutex held.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "control.hpp"
#include "journal.hpp"
#include "scheduler_policy.hpp"
#include "telemetry.hpp"

namespace opensweep::boost {

inline constexpr const char* kTrigger = "boost";

struct BoostConfig {
    std::int64_t start_after_ms = 5'000;   // fullscreen this long before a session starts
    std::int64_t end_after_ms = 10'000;    // not fullscreen this long before a session ends
    std::int64_t hint_ttl_ms = 15'000;     // a foreground hint older than this is ignored
    double min_avg_cpu = 1.0;              // % of the whole machine, averaged over the window
    std::size_t window_samples = 15;       // 30 s at 2 s sampling
    std::size_t max_targets = 16;
    std::size_t max_tracked = 512;         // bounded per-process CPU history
    std::size_t max_family = 256;          // bounded set of excluded descendants
    int max_parent_depth = 32;
    std::int64_t memory_priority = 2;      // MEMORY_PRIORITY_LOW
    sched::ClassifierLists lists;          // protection / classification lists (shared with the scheduler)
};

struct ThrottledTarget {
    telemetry::PolicyTarget target;
    telemetry::WorkloadClass workload = telemetry::WorkloadClass::Unknown;
    double avg_cpu = 0.0;
    struct Applied {
        telemetry::PolicyKind kind;
        std::string tx_id;
        std::int64_t original = 0;
    };
    std::vector<Applied> applied;
};

struct SessionSummary {
    std::int64_t start = 0;
    std::int64_t end = 0;
    std::size_t throttled_count = 0;  // distinct targets throttled during the session
    std::string reason;               // why it ended
    control::RollbackSummary restore;
};

struct View {
    bool enabled = false;
    bool active = false;
    std::int64_t since = 0;            // session start (0 = inactive)
    std::uint32_t foreground_pid = 0;  // the session's game, else the current foreground hint (0 = none)
    std::string foreground_name;
    std::vector<ThrottledTarget> throttled;
    std::optional<SessionSummary> last_session;
};

// Lower-case workload name used by the IPC ("background", "throughput", ...).
const char* workload_name(telemetry::WorkloadClass c) noexcept;

class GameBoost {
public:
    GameBoost(telemetry::Actuator& act, journal::Journal& journal, BoostConfig cfg = {});

    // Crash recovery: restores every non-terminal boost transaction of a previous run.
    control::RollbackSummary recover(std::int64_t now_ms);

    // Persisted user opt-in. Turning it off ends an active session (everything restored).
    control::RollbackSummary set_enabled(bool on, std::int64_t now_ms);
    bool enabled() const noexcept { return enabled_; }

    // Foreground hint from the user's app (already validated by the platform: pid exists and is in
    // the caller's session). `now_ms` is the wall clock of the report.
    void report_foreground(std::uint32_t pid, bool fullscreen, std::int64_t now_ms);

    // One engine tick. `control_pids`: processes the Adaptive Control Engine currently holds.
    void tick(const telemetry::Sample& s, std::span<const std::uint32_t> control_pids);

    // Ends an active session now (no-op otherwise). `suppress` blocks a new session until the
    // foreground leaves fullscreen (used by rollbackAll).
    control::RollbackSummary end(std::int64_t now_ms, std::string_view reason, bool suppress = false);

    bool active() const noexcept { return active_; }
    bool fullscreen_now() const noexcept { return fullscreen_now_; }
    std::vector<std::uint32_t> held_pids() const;
    View view() const;
    std::size_t tracked() const noexcept { return hist_.size(); }
    const BoostConfig& config() const noexcept { return cfg_; }

private:
    struct History {
        std::int64_t start_ms = 0;
        std::vector<float> cpu;              // window_samples slots
        std::vector<std::uint64_t> stamp;    // tick number of each slot (0 = empty)
        std::uint64_t last_tick = 0;
    };
    struct Hint {
        std::uint32_t pid = 0;
        bool fullscreen = false;
        std::int64_t ts = 0;
    };

    void record(const telemetry::Sample& s);
    sched::HistoryView history_view(std::uint32_t pid) const;
    bool fullscreen_foreground(const telemetry::Sample& s) const;
    void start_session(const telemetry::Sample& s, std::int64_t t);
    void select_targets(const telemetry::Sample& s, std::span<const std::uint32_t> control_pids, std::int64_t t);
    bool throttle(const telemetry::ProcessSample& p, telemetry::WorkloadClass cls, double avg, std::int64_t t);
    void restore_target(ThrottledTarget& tt, std::int64_t t, std::string_view reason, control::RollbackSummary& sum);
    void update_family(const telemetry::Sample& s);

    telemetry::Actuator& act_;
    journal::Journal& journal_;
    BoostConfig cfg_;
    bool enabled_ = false;

    Hint hint_;
    std::uint64_t tick_ = 0;
    std::unordered_map<std::uint32_t, History> hist_;
    std::uint32_t last_fg_pid_ = 0;
    std::string last_fg_name_;

    bool fullscreen_now_ = false;
    std::uint32_t fs_pid_ = 0;        // fullscreen candidate (not yet a session)
    std::int64_t fs_since_ = 0;
    bool suppressed_ = false;

    bool active_ = false;
    std::int64_t since_ = 0;
    telemetry::PolicyTarget game_;
    std::int64_t not_fs_since_ = 0;   // 0 = the game is the fullscreen foreground
    std::unordered_set<std::uint32_t> family_;  // descendants of the game / foreground seen this session
    std::vector<ThrottledTarget> throttled_;
    std::size_t throttled_count_ = 0;
    std::optional<SessionSummary> last_;
};

}  // namespace opensweep::boost
