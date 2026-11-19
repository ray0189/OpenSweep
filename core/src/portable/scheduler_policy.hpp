// SweepScheduler (Part 5): workload classification, protection, CPU contention detection and
// conservative candidate selection. Portable: no Windows headers. Owner: engine-act.
//
// Policy order (Part 5): observe -> sustained contention -> verified background workload ->
// EcoQoS -> (hybrid & advanced only) CPU Sets to efficiency cores -> temporary BELOW_NORMAL.
// It never pins affinity and never assumes a fixed "best" core: efficiency cores are only those the
// OS reports with the lowest EfficiencyClass on a hybrid machine. High CPU alone never triggers.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "policy_types.hpp"
#include "telemetry.hpp"

namespace opensweep::sched {

using telemetry::ProcessSample;
using telemetry::WorkloadClass;

// Lower-case ASCII, path and trailing ".exe" removed: "C:\\x\\MsMpEng.EXE" -> "msmpeng".
std::string normalize_image(std::string_view name);

// Built-in lists (normalized names; an entry ending in '*' is a prefix match).
std::span<const std::string_view> builtin_protected_images() noexcept;
std::span<const std::string_view> anti_cheat_images() noexcept;
std::span<const std::string_view> latency_critical_images() noexcept;  // games, media, DAWs, streaming
std::span<const std::string_view> background_images() noexcept;        // indexers, sync, updaters
std::span<const std::string_view> throughput_images() noexcept;        // compilers, encoders, archivers

bool is_builtin_protected_image(std::string_view name);  // accepts raw or normalized names
bool is_anti_cheat_image(std::string_view name);

// Caller-extensible lists (normalized or raw names; '*' suffix = prefix match).
struct ClassifierLists {
    std::vector<std::string> extra_protected;
    std::vector<std::string> extra_latency_critical;
    std::vector<std::string> extra_background;
    std::vector<std::string> extra_throughput;
    std::vector<std::string> owned_images{"opensweepcore"};  // session 0 processes OpenSweep owns
};

// Per-process history summary used by the classifier.
struct HistoryView {
    int samples = 0;         // consecutive recent samples recorded (bounded by kHistoryLen)
    double avg_cpu = 0.0;    // % of the whole machine
    double min_cpu = 0.0;
    bool present = false;    // seen in the latest sample
};

inline constexpr std::size_t kHistoryLen = 15;  // 30 s at 2 s sampling

// Classification table (first match wins):
//   foreground + (game|anti-cheat|media|DAW)   -> LatencyCritical
//   foreground                                 -> Interactive
//   has_audio                                  -> LatencyCritical
//   game|anti-cheat|media|DAW image            -> LatencyCritical
//   already EcoQoS by itself / IDLE priority   -> Efficiency
//   throughput image (compiler, encoder...)    -> Throughput
//   background image (indexer, sync, updater)  -> Background
//   non-critical, non-security service         -> Background
//   sustained CPU (>= samples, min >= floor)   -> Throughput
//   anything else                              -> Unknown
WorkloadClass classify(const ProcessSample& p, const HistoryView& h, const ClassifierLists& lists,
                       bool self_ecoqos = false, double sustained_cpu_floor = 5.0, int min_samples = 5);

// Returns the reason a process must never be targeted, or nullopt if it may be considered.
// Covers: reserved pids, foreground, system critical, security software, audio, session 0
// services not owned by OpenSweep, the protection list, anti-cheat, and Unknown /
// LatencyCritical / Interactive / Efficiency workloads.
std::optional<std::string> protection_reason(const ProcessSample& p, WorkloadClass cls,
                                             const ClassifierLists& lists);

struct Topology {
    std::uint32_t logical_count = 1;
    bool hybrid = false;
    std::vector<std::uint8_t> efficiency_class;  // per CPU set as reported by the OS; may be empty
    // Number of CPU sets at the lowest reported efficiency class, only when the machine is hybrid
    // and reports more than one class. 0 otherwise (CPU Sets are then never recommended).
    std::uint32_t efficient_cpu_count() const noexcept;
};

struct SchedulerConfig {
    double contention_cpu_percent = 85.0;   // system CPU %
    double contention_queue_per_cpu = 1.0;  // processor queue length per logical CPU
    double severe_queue_per_cpu = 2.0;      // required when the responsiveness signal is unknown
    int sustain_ticks = 5;                  // consecutive samples (10 s)
    double fg_latency_degraded_ms = 50.0;   // foreground responsiveness proxy threshold
    double candidate_core_fraction = 0.5;   // candidate must use >= this share of one logical CPU
    int min_history = 5;
    std::size_t max_candidates = 3;
    std::size_t max_tracked = 512;          // bounded history
    std::uint64_t forget_after_ticks = 30;
    ClassifierLists lists;
};

struct SchedulerInputs {
    std::optional<double> fg_latency_ms;           // responsiveness proxy; nullopt = unknown
    std::vector<std::uint32_t> self_ecoqos_pids;   // processes that opted into EcoQoS themselves
    std::vector<std::uint32_t> opensweep_ecoqos_pids;   // EcoQoS currently applied by OpenSweep
    std::vector<std::uint32_t> opensweep_cpusets_pids;  // CPU Sets currently applied by OpenSweep
    bool allow_cpu_sets = false;                   // true only in advanced mode
};

struct ProcessClassification {
    std::uint32_t pid = 0;
    std::string name;
    WorkloadClass cls = WorkloadClass::Unknown;
    std::string protected_by;  // empty = not protected
};

struct SchedulerAssessment {
    bool contention = false;           // sustained CPU + queue pressure
    bool foreground_present = false;
    bool foreground_impacted = false;
    int contention_ticks = 0;
    double cpu_percent = 0.0;
    double queue_per_cpu = 0.0;
    std::string reason;  // why something is (or is not) recommended
    std::vector<ProcessClassification> classified;
    std::vector<policy::Recommendation> recommendations;
};

class SweepScheduler {
public:
    explicit SweepScheduler(SchedulerConfig cfg = {});

    SchedulerAssessment assess(const telemetry::Sample& s, const Topology& topo, const SchedulerInputs& in);

    HistoryView history(std::uint32_t pid) const;
    int contention_ticks() const noexcept { return contention_ticks_; }
    std::size_t tracked() const noexcept { return hist_.size(); }
    const SchedulerConfig& config() const noexcept { return cfg_; }

private:
    struct History {
        std::string name;
        std::int64_t start_ms = 0;
        std::array<float, kHistoryLen> cpu{};
        std::uint8_t count = 0;
        std::uint8_t next = 0;
        std::uint64_t last_tick = 0;
    };
    void record(const ProcessSample& p);
    void prune();
    HistoryView view(const History& h) const;

    SchedulerConfig cfg_;
    std::unordered_map<std::uint32_t, History> hist_;
    std::uint64_t tick_ = 0;
    int contention_ticks_ = 0;
};

}  // namespace opensweep::sched
