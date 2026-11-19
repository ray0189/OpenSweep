// Incident reports (Phase 3; Part 6 "Incident recording", Part 7 "Output").
//
// An Incident states what was observed and what it is *correlated with*; it never claims causation.
// Every incident carries symptoms, evidence (metric, observed, baseline, robust z, window), possible
// contributors with a correlation strength, a confidence (0..1 + low/medium/high), a risk level,
// recommended actions and `autoRecoveryAllowed` (false unless the recovery acts only on something
// OpenSweep owns).
//
// IncidentStore: bounded (500). Repeats are merged by `key` (detector + subject): an open incident
// with the same key is updated in place (occurrences++, lastSeen, evidence refreshed); a resolved
// one is re-opened when the repeat comes within `merge_window_ms` of its resolution. Persistence is
// JSONL (`incidents.jsonl`) with a per-line CRC: changed incidents are appended; on load the last
// valid line per id wins; the file is compacted (atomic rewrite) when it exceeds 2 x capacity lines.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "json.hpp"
#include "trace.hpp"

namespace opensweep::incident {

enum class Level { Low, Medium, High };                    // confidence level and risk
enum class Severity { Info, Warning, Error, Critical };
enum class State { Open, Resolved };

const char* to_string(Level l) noexcept;      // "low" | "medium" | "high"
const char* to_string(Severity s) noexcept;   // "info" | "warning" | "error" | "critical"
const char* to_string(State s) noexcept;      // "open" | "resolved"
std::optional<Level> level_from_string(std::string_view s) noexcept;
std::optional<Severity> severity_from_string(std::string_view s) noexcept;
std::optional<State> state_from_string(std::string_view s) noexcept;

struct Evidence {
    std::string metric;     // e.g. "commitPercent", "process.privateBytes", "events.service_failure"
    std::string unit;       // e.g. "%", "bytes", "MB/min", "count"
    double observed = 0.0;
    double baseline = std::numeric_limits<double>::quiet_NaN();   // NaN = no baseline
    double robust_z = std::numeric_limits<double>::quiet_NaN();   // NaN = not applicable
    std::int64_t window_from_ms = 0;
    std::int64_t window_to_ms = 0;
    std::string note;       // short human-readable detail
};

struct Contributor {
    std::string name;            // image / service / driver / app name
    std::string kind;            // "process" | "service" | "driver" | "app" | "device"
    std::uint32_t pid = 0;       // 0 when not a process
    double correlation = 0.0;    // 0..1 strength of the co-movement (not causation)
};

struct Incident {
    std::string id;              // "inc-<firstSeenMs>-<seq>", seq monotonic per store
    std::string key;             // dedupe key: detector + subject
    std::string detector;        // e.g. "memory_growth", "service_crash_loop"
    std::string title;
    std::string summary;         // one or two sentences; uses "correlated with", never "caused by"
    std::vector<std::string> symptoms;
    std::vector<Evidence> evidence;
    std::vector<Contributor> contributors;
    double confidence = 0.0;     // 0..1
    Level confidence_level = Level::Low;
    Level risk = Level::Low;
    Severity severity = Severity::Warning;
    std::vector<std::string> recommended_actions;
    bool auto_recovery_allowed = false;
    State state = State::Open;
    std::int64_t first_seen_ms = 0;
    std::int64_t last_seen_ms = 0;
    std::int64_t resolved_ms = 0;     // 0 while open
    std::uint32_t occurrences = 1;
};

// Confidence model (documented weights):
//   c = 0.15 + 0.40 * strength + 0.25 * duration_term + 0.20 * signals_term
//   strength      : 0..1, how far beyond the threshold the evidence is (detector supplied;
//                   z_strength() maps a robust z: 3 -> 0.0 ... 9 -> 1.0)
//   duration_term : min(1, duration / (2 * required_duration)); 0.5 at exactly the required time
//   signals_term  : min(1, (agreeing_signals) / 3), signals other than the primary one
//   baseline not ready -> c = min(0.6 c, 0.39) (always "low")
//   clamped to [0.05, 0.99]. Level: < 0.4 low, < 0.7 medium, otherwise high.
struct ConfidenceInputs {
    double strength = 0.0;
    double duration_ms = 0.0;
    double required_duration_ms = 0.0;   // 0 -> duration term = 1 (event-driven)
    int agreeing_signals = 0;
    bool baseline_ready = true;
};
double compute_confidence(const ConfidenceInputs& in) noexcept;
Level confidence_level(double c) noexcept;
double z_strength(double z) noexcept;

// Returns false if any user-facing text of the incident claims causation ("caused by",
// "is causing", "because of", "due to"). Used by tests and as a debug guard.
bool wording_ok(const Incident& inc);

json::Value to_json(const Incident& inc);
std::optional<Incident> from_json(const json::Value& v);   // validated; nullopt on garbage

class IncidentStore {
public:
    static constexpr std::size_t kDefaultCapacity = 500;
    static constexpr std::int64_t kDefaultMergeWindowMs = 10 * 60'000;

    explicit IncidentStore(std::size_t capacity = kDefaultCapacity);

    struct Upsert {
        Incident* incident = nullptr;
        bool created = false;     // a new incident was created (false: merged / re-opened)
        bool reopened = false;
    };
    // Opens `candidate` (id/state/timestamps/occurrences are assigned here) or merges it into the
    // newest incident with the same key (see header comment). `now_ms` is the observation time.
    // Re-opening a resolved incident always counts an occurrence; merging into a still-open one
    // counts one only when `count_repeat` is true (event-driven repeats). Level detectors pass false
    // so that refreshing an open incident (e.g. after a service restart) is not counted as a repeat.
    Upsert open_or_merge(Incident candidate, std::int64_t now_ms,
                         std::int64_t merge_window_ms = kDefaultMergeWindowMs, bool count_repeat = true);

    // Updates last_seen of the open incident with this key (no allocation). If `observed` is
    // finite and larger than the first evidence's observed value, that value is raised (peak).
    bool touch(std::string_view key, std::int64_t now_ms,
               double observed = std::numeric_limits<double>::quiet_NaN());
    bool resolve(std::string_view key, std::int64_t now_ms);
    // Adds a contributor to the open incident with this key (no duplicate name + pid; at most
    // `max_contributors`). Used to attach kernel-trace attribution to existing incidents.
    bool add_contributor(std::string_view key, const Contributor& c, std::size_t max_contributors = 8);

    const Incident* find_open(std::string_view key) const;
    const Incident* find_id(std::string_view id) const;
    // Newest activity first (by last_seen); limit 0 = all.
    std::vector<Incident> list(std::size_t limit = 0) const;
    void for_each_open(const std::function<void(const Incident&)>& f) const;
    std::size_t open_count() const noexcept;
    std::size_t size() const noexcept { return items_.size(); }
    std::size_t capacity() const noexcept { return capacity_; }

    bool persist(trace::FileIO& io, const std::string& path);
    trace::LoadReport load(trace::FileIO& io, const std::string& path);
    void clear();

private:
    struct Entry {
        Incident inc;
        bool dirty = true;
    };
    Entry* newest_with_key(std::string_view key);
    void evict_if_full();
    std::string next_id(std::int64_t now_ms);

    std::size_t capacity_;
    std::vector<Entry> items_;   // creation order
    std::uint64_t seq_ = 0;
    std::size_t file_lines_ = 0;
    bool file_known_ = false;
};

}  // namespace opensweep::incident
