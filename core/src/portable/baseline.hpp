// Machine-specific baselines (Phase 2, Part 10 "ESTABLISH BASELINE").
//
// For every trace metric the engine feeds one Sample every 2 s. Samples are averaged per wall-clock
// minute; each closed minute value is added to:
//   * the metric's overall baseline, and
//   * exactly one context baseline: "active" when the majority of the minute's samples had
//     cpu_percent >= kActiveCpuPercent (10 %), otherwise "idle".
// The foreground hint is deliberately not used for the context: session 0 only knows it while the
// UI is running, which would make the split depend on whether the dashboard is open.
//
// A Baseline keeps, in fixed memory: a rolling window of the last `window` minute values (float),
// the median and MAD of that window (recomputed when a value is added, i.e. once per minute), a fast
// and a slow EWMA, and a total sample count. It is `ready()` once `ready_after` values have been
// added (default 120 = two hours of minutes for the overall baseline).
//
// Boot durations (BootPerformance events, ms) have their own small baseline (last 20 boots, ready
// after 3) fed through observe_events().
//
// Persistence: JSON (`baselines.json`), validated on load: wrong version, wrong types, non-finite
// numbers or oversize windows reject that baseline (counted); unknown metrics are ignored.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "json.hpp"
#include "ringbuffer.hpp"
#include "telemetry.hpp"
#include "trace.hpp"

namespace opensweep::baseline {

using trace::Metric;
using trace::kMetricCount;

inline constexpr double kActiveCpuPercent = 10.0;

struct BaselineConfig {
    std::size_t window = 1440;       // values kept (1 per minute -> 24 h)
    std::size_t ready_after = 120;   // values needed before ready()
    double fast_alpha = 0.3;         // EWMA per value (~3 min memory at 1/min)
    double slow_alpha = 0.02;        // ~50 min memory
};

class Baseline {
public:
    explicit Baseline(BaselineConfig cfg = {});

    // Adds one aggregated value. Non-finite values are ignored.
    void add(double v);
    bool ready() const noexcept { return samples_ >= cfg_.ready_after; }
    std::uint64_t samples() const noexcept { return samples_; }
    std::size_t window_size() const noexcept { return window_.size(); }
    // Median / MAD of the window; NaN when empty.
    double median() const noexcept { return median_; }
    double mad() const noexcept { return mad_; }
    double ewma_fast() const noexcept { return fast_; }
    double ewma_slow() const noexcept { return slow_; }
    const BaselineConfig& config() const noexcept { return cfg_; }

    // Robust z of x against the window: 0.6745 (x - median) / max(MAD, min_mad). nullopt when the
    // window is empty or x is not finite. Readiness is separate (callers check ready()).
    std::optional<double> z(double x, double min_mad) const noexcept;

    json::Value to_json() const;
    // Validates and restores; returns false (leaving *this unchanged) on any invalid content.
    bool from_json(const json::Value& v);
    void clear();

private:
    void recompute();

    BaselineConfig cfg_;
    RingBuffer<float> window_;
    std::uint64_t samples_ = 0;
    double median_;
    double mad_;
    double fast_;
    double slow_;
    bool ewma_init_ = false;
};

enum class Context { All, Active, Idle };

struct BaselineSetConfig {
    BaselineConfig overall{};                      // 1440 values, ready after 120
    BaselineConfig context{720, 60, 0.3, 0.02};    // active / idle: 720 values, ready after 60
    BaselineConfig boot{20, 3, 0.5, 0.2};          // boot durations
};

class BaselineSet {
public:
    explicit BaselineSet(BaselineSetConfig cfg = {});

    // Feed every sample (2 s). Minute values are committed when a later minute starts.
    void update(const telemetry::Sample& s);
    // Feeds BootPerformance durations (value, ms) into the boot baseline.
    void observe_events(std::span<const telemetry::EventRecord> events);

    const Baseline& get(Metric m, Context c = Context::All) const noexcept;
    Baseline& get_mut(Metric m, Context c = Context::All) noexcept;
    const Baseline& boot() const noexcept { return boot_; }
    Baseline& boot_mut() noexcept { return boot_; }

    // IPC `baselines` shape: { metrics:{ name:{ median, mad, ewma, samples, ready } } }.
    json::Value summary_json() const;

    // Full state (windows included) for baselines.json.
    json::Value to_json() const;
    // Returns the number of rejected entries (0 = all good); a top-level problem rejects everything
    // and returns >= 1 while leaving the set empty.
    std::size_t from_json(const json::Value& v);
    bool save(trace::FileIO& io, const std::string& path) const;
    trace::LoadReport load(trace::FileIO& io, const std::string& path);
    void clear();

private:
    void commit_minute();

    struct PerMetric {
        Baseline all;
        Baseline active;
        Baseline idle;
    };
    BaselineSetConfig cfg_;
    std::vector<PerMetric> m_;  // kMetricCount entries
    Baseline boot_;
    // Open minute accumulator.
    std::int64_t minute_ = -1;
    std::array<double, kMetricCount> sum_{};
    std::array<std::uint32_t, kMetricCount> n_{};
    std::uint32_t active_samples_ = 0;
    std::uint32_t total_samples_ = 0;
};

}  // namespace opensweep::baseline
