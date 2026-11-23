// Scorecard (docs/core/alpha4.md section 3): "before / now" numbers of the machine, computed only
// from data OpenSweep actually measured. Every metric is null when there is not enough data; nothing
// is estimated or invented. Portable: no Windows headers.
//
// Sources and definitions (window = the last 24 h unless stated otherwise):
//   bootMs            median of the `value` (boot duration, ms) of the newest 5 BootPerformance events
//                     (Diagnostics-Performance 100) with a positive value.
//   idleCpuPercent    mean CPU % over the *idle* minutes of the trace's 24 h tier (1-minute rows).
//   idleRamUsedBytes  mean of (memTotal - memAvail) over the same idle minutes.
//                     IDLE: a 1-minute row whose mean CPU is < 15 % and during which no fullscreen
//                     foreground app was reported (the fullscreen minutes are recorded by observe()).
//                     The 24 h tier stores per-minute means, so idleness is decided per minute rather
//                     than per 2 s sample; a minute that mixes busy and idle samples counts as idle
//                     only when its mean is below 15 %.
//   diskLatencyMs     mean of max(avg read latency, avg write latency) over the minutes in which the
//                     disk latency was > 0 (a disk that did no I/O reports 0, which is not a latency).
//   hardFaultsPerSec  mean hard faults/s (\Memory\Page Reads/sec) over all minutes.
//   stutterPerHour    1-minute windows in which any single DPC or ISR ran >= 2000 us, per hour of
//                     kernel-traced time (Sample::kernel worst DPC / ISR, only while kernel tracing
//                     delivered data). Requires >= 30 traced minutes.
//   crashesPerWeek    AppCrash + Bugcheck events in the last 7 days, scaled to 7 days by the covered
//                     time (from max(now - 7 d, first start - 24 h event-log backfill, oldest stored
//                     event when the event store is full)); null when less than 24 h are covered or
//                     the first start is unknown.
// The trace-based metrics need >= 10 qualifying minutes, otherwise they are null.
// hoursMeasured = number of 1-minute rows in the 24 h window / 60 (one decimal).
//
// Persistence (`scorecard.json`, atomic write): { version:1, firstStartMs, baseline:{ capturedAt,
// metrics } | null, minutes:[[minuteIndex, flags]...] } where flags bit 0 = kernel traced, bit 1 =
// stutter, bit 2 = fullscreen; at most 1440 minutes (24 h) are kept.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string>

#include "json.hpp"
#include "telemetry.hpp"
#include "trace.hpp"

namespace opensweep::scorecard {

inline constexpr double kIdleCpuPercent = 15.0;
inline constexpr double kStutterUs = 2000.0;
inline constexpr std::int64_t kWindowMs = 24LL * 3600 * 1000;
inline constexpr std::int64_t kWeekMs = 7LL * 24 * 3600 * 1000;
inline constexpr std::int64_t kAutoCaptureAfterMs = 24LL * 3600 * 1000;
inline constexpr std::int64_t kEventBackfillMs = 24LL * 3600 * 1000;  // events.cpp initial backfill
inline constexpr std::size_t kMinMinutes = 10;
inline constexpr std::size_t kMinKernelMinutes = 30;
inline constexpr std::size_t kBootsForMedian = 5;
inline constexpr std::size_t kMaxMinutes = 1440;
inline constexpr std::int64_t kVersion = 1;

struct Metrics {
    std::optional<double> boot_ms;
    std::optional<double> idle_cpu_percent;
    std::optional<double> idle_ram_used_bytes;
    std::optional<double> disk_latency_ms;
    std::optional<double> hard_faults_per_sec;
    std::optional<double> stutter_per_hour;
    std::optional<double> crashes_per_week;
    bool any() const noexcept {
        return boot_ms || idle_cpu_percent || idle_ram_used_bytes || disk_latency_ms || hard_faults_per_sec ||
               stutter_per_hour || crashes_per_week;
    }
};

struct Baseline {
    std::int64_t captured_at = 0;
    Metrics metrics;
};

// { bootMs, idleCpuPercent, idleRamUsedBytes, diskLatencyMs, hardFaultsPerSec, stutterPerHour,
//   crashesPerWeek } with null for unmeasured values.
json::Value metrics_to_json(const Metrics& m);
// Strict: an object with exactly those keys, each a finite number >= 0 or null.
std::optional<Metrics> metrics_from_json(const json::Value& v);
json::Value baseline_to_json(const Baseline& b);  // { capturedAt, metrics }

class Scorecard {
public:
    // Per tick: records the stutter / fullscreen flags of the sample's wall-clock minute.
    void observe(const telemetry::Sample& s, bool fullscreen);

    // Current metrics at `now` from the trace (24 h tier + events) and the recorded minutes.
    Metrics compute(const trace::Trace& tr, std::span<const telemetry::EventRecord> events, std::int64_t now) const;
    double hours_measured(const trace::Trace& tr, std::int64_t now) const;

    // First start of the service with a scorecard (set once by the engine; persisted).
    std::int64_t first_start() const noexcept { return first_start_; }
    void set_first_start(std::int64_t t) noexcept { first_start_ = t; }
    const std::optional<Baseline>& baseline() const noexcept { return baseline_; }
    void set_baseline(Baseline b) { baseline_ = std::move(b); }
    // True when no baseline exists and 24 h have passed since the first start.
    bool auto_capture_due(std::int64_t now) const noexcept {
        return !baseline_ && first_start_ > 0 && now - first_start_ >= kAutoCaptureAfterMs;
    }

    std::string serialize() const;
    // Restores what is valid; returns false (state untouched) when the text is not a valid file.
    bool deserialize(std::string_view text);
    bool save(trace::FileIO& io, const std::string& path) const;
    trace::LoadReport load(trace::FileIO& io, const std::string& path);

    std::size_t minutes() const noexcept { return minutes_.size(); }

private:
    struct Minute {
        std::int64_t index = 0;  // ts / 60 000
        std::uint8_t flags = 0;
    };
    static constexpr std::uint8_t kKernel = 1, kStutter = 2, kFullscreen = 4;
    bool fullscreen_minute(std::int64_t minute_index) const;

    std::int64_t first_start_ = 0;
    std::optional<Baseline> baseline_;
    std::deque<Minute> minutes_;  // ascending index, only minutes with a flag, <= kMaxMinutes
};

}  // namespace opensweep::scorecard
