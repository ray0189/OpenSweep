// SweepMemory (Part 4): memory pressure scoring with hysteresis, contributor identification and
// conservative background recommendations. Portable: no Windows headers. Owner: engine-act.
//
// Behaviour by level:
//   low         -> nothing
//   elevated    -> identify contributors (no recommendation)
//   significant -> recommend MemoryPriority lowering (and EcoQoS) for verified background contributors
//   critical    -> additionally a diagnostic recommendation for the user
// Never: kill a process, EmptyWorkingSet, purge the standby list.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "policy_types.hpp"
#include "scheduler_policy.hpp"
#include "telemetry.hpp"

namespace opensweep::memory {

enum class PressureLevel { Low = 0, Elevated = 1, Significant = 2, Critical = 3 };
const char* to_string(PressureLevel l) noexcept;

// Baseline numbers come from the Phase 2 baseline module (passed in as plain numbers).
struct MemoryBaseline {
    bool ready = false;
    double hard_faults_median = 0.0;  // per second
    double hard_faults_mad = 0.0;
};

struct MemoryConfig {
    // Score -> level thresholds (score is 0..100).
    double elevated = 35.0;
    double significant = 60.0;
    double critical = 80.0;
    double hysteresis_margin = 10.0;  // must fall this far below a threshold to step down
    int enter_ticks = 2;              // consecutive samples above a higher threshold to step up
    int exit_ticks = 3;               // consecutive samples below (threshold - margin) to step down
    // Component weights (sum = 100).
    double w_available = 35.0, w_commit = 25.0, w_faults = 20.0, w_compression = 10.0, w_latency = 10.0;
    std::uint64_t contributor_min_bytes = 256ull * 1024 * 1024;
    double contributor_min_fraction = 0.02;  // of physical memory
    std::size_t max_contributors = 8;
    std::size_t max_candidates = 3;
    bool recommend_ecoqos = true;
    sched::ClassifierLists lists;
};

struct PressureComponents {
    double available_percent = 100.0;
    double commit_percent = 0.0;
    double hard_faults = 0.0;
    double hard_fault_ratio = 0.0;  // vs baseline median (0 when baseline not ready)
    double compressed_percent = 0.0;
    double paging_latency_ms = 0.0;
    // normalised 0..1 sub-scores
    double s_available = 0.0, s_commit = 0.0, s_faults = 0.0, s_compression = 0.0, s_latency = 0.0;
};

struct PressureScore {
    double score = 0.0;  // 0..100
    PressureComponents c;
};

// Pure scoring function (no state).
PressureScore pressure_score(const telemetry::Sample& s, const MemoryBaseline& b, const MemoryConfig& cfg);
PressureLevel level_for(double score, const MemoryConfig& cfg) noexcept;

struct Contributor {
    std::uint32_t pid = 0;
    std::string name;
    std::uint64_t private_bytes = 0;
    std::uint64_t working_set = 0;
    telemetry::WorkloadClass cls = telemetry::WorkloadClass::Unknown;
    bool suspected_leak = false;
    std::string protected_by;  // empty = may be acted on
};

struct Diagnostic {
    std::string summary;
    std::vector<std::string> evidence;
    std::vector<std::string> recommended_actions;  // for the user; never executed automatically
    std::string risk = "high";
};

struct MemoryInputs {
    MemoryBaseline baseline;
    std::vector<std::uint32_t> suspected_leak_pids;  // from growth/leak classification (Phase 3)
    std::vector<std::uint32_t> self_ecoqos_pids;
};

struct MemoryAssessment {
    double score = 0.0;
    PressureLevel level = PressureLevel::Low;      // after hysteresis
    PressureLevel raw_level = PressureLevel::Low;  // instantaneous
    PressureComponents components;
    std::string reason;
    std::vector<Contributor> contributors;
    std::vector<policy::Recommendation> recommendations;
    std::optional<Diagnostic> diagnostic;
};

class SweepMemory {
public:
    explicit SweepMemory(MemoryConfig cfg = {});
    MemoryAssessment assess(const telemetry::Sample& s, const MemoryInputs& in);
    PressureLevel level() const noexcept { return level_; }
    const MemoryConfig& config() const noexcept { return cfg_; }

private:
    PressureLevel step_level(double score);

    MemoryConfig cfg_;
    PressureLevel level_ = PressureLevel::Low;
    int up_ticks_ = 0;
    int down_ticks_ = 0;
};

}  // namespace opensweep::memory
