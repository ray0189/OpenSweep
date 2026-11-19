#include "guard.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string_view>

#include "ringbuffer.hpp"
#include "stats.hpp"
#include "trace.hpp"

namespace opensweep::guard {

using baseline::BaselineSet;
using baseline::Context;
using incident::Contributor;
using incident::Evidence;
using incident::Incident;
using incident::IncidentStore;
using incident::Level;
using incident::Severity;
using telemetry::EventKind;
using trace::Metric;

// ---------------------------------------------------------------------------------------------
Hysteresis::Edge Hysteresis::update(bool on, bool off, std::int64_t now) noexcept {
    if (last_ >= 0 && (now < last_ || now - last_ > max_gap_)) {
        on_since_ = -1;
        off_since_ = -1;
    }
    last_ = now;
    if (on) {
        off_since_ = -1;
        if (on_since_ < 0) on_since_ = now;
        if (!open_ && now - on_since_ >= open_after_) {
            open_ = true;
            return Edge::Opened;
        }
        return Edge::None;
    }
    on_since_ = -1;
    if (!open_ || !off) {
        off_since_ = -1;
        return Edge::None;
    }
    if (off_since_ < 0) off_since_ = now;
    if (now - off_since_ >= resolve_after_) {
        open_ = false;
        off_since_ = -1;
        return Edge::Resolved;
    }
    return Edge::None;
}

void Hysteresis::force_closed() noexcept {
    open_ = false;
    on_since_ = -1;
    off_since_ = -1;
}

void Hysteresis::reset() noexcept {
    force_closed();
    last_ = -1;
}

// ---------------------------------------------------------------------------------------------
StabilityScore compute_stability_score(const ScoreInputs& in) {
    StabilityScore out;
    int total = 0;
    auto deduct = [&](std::size_t count, int each, int cap, const char* what) {
        if (count == 0) return;
        const long long raw = static_cast<long long>(count) * each;
        const int d = static_cast<int>(std::min<long long>(raw, cap));
        total += d;
        char buf[160];
        std::snprintf(buf, sizeof buf, "%zu %s (-%d)", count, what, d);
        out.reasons.emplace_back(buf);
    };
    deduct(in.open_critical, 25, 50, "open critical incident(s)");
    deduct(in.open_error, 12, 36, "open error incident(s)");
    deduct(in.open_warning, 4, 16, "open warning incident(s)");
    deduct(in.open_info, 1, 4, "open informational incident(s)");
    deduct(in.shutdowns_7d, 8, 24, "unexpected shutdown(s) or bugcheck(s) in 7 days");
    deduct(in.whea_7d, 5, 15, "hardware (WHEA) error(s) in 7 days");
    deduct(in.disk_errors_7d, 3, 12, "disk error(s) in 7 days");
    deduct(in.crashes_24h, 2, 10, "application crash(es) or hang(s) in 24 hours");
    deduct(in.service_failures_24h, 1, 8, "service failure(s) in 24 hours");
    auto pressure = [&](double v, double hi, int hi_pts, double mid, int mid_pts, const char* what) {
        if (!std::isfinite(v)) return;
        const int d = v >= hi ? hi_pts : v >= mid ? mid_pts : 0;
        if (d == 0) return;
        total += d;
        char buf[160];
        std::snprintf(buf, sizeof buf, "%s at %.0f%% (-%d)", what, v, d);
        out.reasons.emplace_back(buf);
    };
    pressure(in.commit_percent, 95.0, 10, 85.0, 5, "commit charge");
    pressure(in.mem_load_percent, 95.0, 5, 90.0, 2, "memory load");
    out.score = std::clamp(100 - total, 0, 100);
    return out;
}

// ---------------------------------------------------------------------------------------------
namespace {

constexpr double kMB = 1024.0 * 1024.0;
constexpr double kMinuteD = 60'000.0;
constexpr double kHourD = 60.0 * kMinuteD;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr std::size_t kMaxCounters = 64;
constexpr std::size_t kCounterDepth = 32;
constexpr std::size_t kKindHistory = 256;
constexpr std::size_t kKindCount = static_cast<std::size_t>(EventKind::Other) + 1;
constexpr std::size_t kMaxContributors = 5;

template <typename... A>
std::string fmt(const char* f, A... a) {
    char buf[512];
    const int n = std::snprintf(buf, sizeof buf, f, a...);
    if (n < 0) return {};
    return std::string(buf, std::min(static_cast<std::size_t>(n), sizeof buf - 1));
}

std::int64_t floor_div(std::int64_t a, std::int64_t b) noexcept {
    std::int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}

double finite_or(double v, double d) { return std::isfinite(v) ? v : d; }

struct ProcPoint {
    std::int64_t minute = 0;
    double priv_mb = 0.0;
    double handles = 0.0;
    double threads = 0.0;
};

struct GrowthTrack {
    bool open = false;
    bool suspected = false;
    std::int64_t clear_since = -1;
    void reset() {
        open = suspected = false;
        clear_since = -1;
    }
};

struct ProcSlot {
    bool used = false;
    std::uint32_t pid = 0;
    std::string name;
    std::int64_t first_seen = 0;
    std::int64_t last_seen = 0;
    bool seen_this_minute = false;
    double cur_priv_mb = 0.0;
    double cur_handles = 0.0;
    double cur_threads = 0.0;
    RingBuffer<ProcPoint> minutes{kProcessMinutes};
    Hysteresis cpu;
    GrowthTrack leak, handles, threads;
    std::string leak_key, cpu_key, handle_key, thread_key;
};

struct SysPoint {
    std::int64_t minute = 0;
    double commit_pct = kNaN;
    double paged = kNaN;
    double nonpaged = kNaN;
    double handles = kNaN;
    double threads = kNaN;
    double latency = kNaN;
};

struct EventCounter {
    std::string key;
    std::array<std::int64_t, kCounterDepth> ts{};
    std::size_t n = 0;
    std::size_t head = 0;  // next write position
    std::int64_t last = 0;
    void add(std::int64_t t) {
        ts[head] = t;
        head = (head + 1) % kCounterDepth;
        n = std::min(n + 1, kCounterDepth);
        last = std::max(last, t);
    }
    std::size_t count_since(std::int64_t from) const {
        std::size_t c = 0;
        for (std::size_t i = 0; i < n; ++i)
            if (ts[i] >= from) ++c;
        return c;
    }
};

struct EventRule {
    EventKind kind;
    const char* detector;
    bool per_subject;
    Severity severity;
    Level risk;
    std::uint32_t count;           // events needed in `window_ms`
    std::int64_t window_ms;
    std::int64_t quiet_ms;         // resolve after this long without a new matching event
    std::uint32_t escalate_count;  // 0 = never
    Severity escalated;
    bool immediate_if_critical;    // a level-1 (critical) event opens regardless of count
};

struct Growth {
    bool ok = false;
    double slope = 0.0;        // unit per minute
    double inc = 0.0;          // increasing fraction
    double first = 0.0;
    double last = 0.0;
    double r = 0.0;            // Pearson r of value vs time
    double drawdown = 0.0;     // largest fall from a running peak (a leak does not give memory back)
    std::int64_t from_minute = 0;
    std::int64_t to_minute = 0;
    std::size_t n = 0;
};

bool is_idle_process(const telemetry::ProcessSample& p) {
    if (p.pid == 0) return true;
    return p.name.empty() || p.name == "Idle" || p.name == "System Idle Process";
}

}  // namespace

// ---------------------------------------------------------------------------------------------
struct Guard::Impl {
    explicit Impl(GuardConfig c)
        : cfg(c),
          sys(kProcessMinutes),
          commit(c.commit_open_ms, c.commit_resolve_ms, c.max_gap_ms),
          handles_sys(c.system_open_ms, c.system_resolve_ms, c.max_gap_ms),
          threads_sys(c.system_open_ms, c.system_resolve_ms, c.max_gap_ms),
          pool_paged(c.system_open_ms, c.system_resolve_ms, c.max_gap_ms),
          pool_nonpaged(c.system_open_ms, c.system_resolve_ms, c.max_gap_ms),
          latency(c.latency_open_ms, c.latency_resolve_ms, c.max_gap_ms),
          dpc(c.dpc_open_ms, c.dpc_resolve_ms, c.max_gap_ms),
          thermal(c.thermal_open_ms, c.thermal_resolve_ms, c.max_gap_ms),
          ph_latency(2.0, 100.0, 5),
          ph_pool(8.0, 256.0, 5) {
        slots.resize(kMaxTrackedProcesses);
        for (auto& sl : slots) sl.cpu = Hysteresis(cfg.runaway_open_ms, cfg.runaway_resolve_ms, cfg.max_gap_ms);
        kind_history.reserve(kKindCount);
        for (std::size_t i = 0; i < kKindCount; ++i) kind_history.emplace_back(kKindHistory);
        counters.reserve(kMaxCounters);
        xs.reserve(kProcessMinutes);
        ys.reserve(kProcessMinutes);
        scratch.reserve(kProcessMinutes * kProcessMinutes / 2);
        rules = {
            {EventKind::ServiceFailure, "service_crash_loop", true, Severity::Error, Level::Medium,
             cfg.service_loop_count, cfg.service_loop_window_ms, cfg.service_loop_window_ms, 0, Severity::Error, false},
            {EventKind::AppCrash, "app_reliability", true, Severity::Warning, Level::Low, cfg.app_repeat_count,
             cfg.app_repeat_window_ms, kDay, 10, Severity::Error, false},
            {EventKind::AppHang, "app_reliability", true, Severity::Warning, Level::Low, cfg.app_repeat_count,
             cfg.app_repeat_window_ms, kDay, 10, Severity::Error, false},
            {EventKind::DiskError, "disk_deterioration", true, Severity::Error, Level::High, cfg.disk_error_count,
             cfg.disk_error_window_ms, 7 * kDay, 10, Severity::Critical, true},
            {EventKind::DriverFailure, "driver_failure", true, Severity::Error, Level::Medium, 1, kDay, kDay, 0,
             Severity::Error, false},
            {EventKind::GpuTimeout, "gpu_timeout", false, Severity::Warning, Level::Medium, 1, kDay, kDay, 3,
             Severity::Error, false},
            {EventKind::Whea, "whea_error", false, Severity::Warning, Level::Medium, 1, kDay, 7 * kDay, 10,
             Severity::Error, false},
            {EventKind::UnexpectedShutdown, "unexpected_shutdown", false, Severity::Error, Level::Medium, 1, kDay,
             kDay, 3, Severity::Critical, false},
            {EventKind::Bugcheck, "bugcheck", false, Severity::Critical, Level::High, 1, 7 * kDay, 7 * kDay, 0,
             Severity::Critical, false},
            {EventKind::ResourceExhaustion, "resource_exhaustion", false, Severity::Error, Level::High, 1, kDay, kDay,
             0, Severity::Error, false},
            {EventKind::Thermal, "thermal_event", false, Severity::Warning, Level::Medium, 1, kDay, kDay, 5,
             Severity::Error, false},
        };
    }

    GuardConfig cfg;
    std::vector<ProcSlot> slots;
    RingBuffer<SysPoint> sys;
    SysPoint cur_sys;
    double lat_sum = 0.0;
    std::uint32_t lat_n = 0;
    std::int64_t minute = -1;
    std::int64_t now = 0;

    Hysteresis commit, handles_sys, threads_sys, pool_paged, pool_nonpaged, latency, dpc, thermal;
    stats::PageHinkley ph_latency, ph_pool;
    std::int64_t ph_latency_change = -1;
    std::int64_t ph_pool_change = -1;

    std::vector<RingBuffer<std::int64_t>> kind_history;
    std::vector<EventCounter> counters;
    std::vector<EventRule> rules;
    std::array<std::pair<std::int64_t, std::string>, 8> recent_drivers{};
    std::size_t recent_driver_next = 0;

    std::vector<double> xs, ys, scratch;
    std::vector<std::string> opened;
    std::vector<std::string> to_resolve;

    const std::string k_commit = "commit_exhaustion";
    const std::string k_handles = "handle_exhaustion|system";
    const std::string k_threads = "thread_exhaustion|system";
    const std::string k_paged = "kernel_pool|paged";
    const std::string k_nonpaged = "kernel_pool|nonpaged";
    const std::string k_latency = "storage_latency";
    const std::string k_dpc = "dpc_isr";
    const std::string k_thermal = "thermal_throttling";
    const std::string k_boot = "boot_degradation";

    // ----------------------------------------------------------------------------------------
    void open(Incident inc, IncidentStore& store, bool count_repeat, std::int64_t at) {
        const auto up = store.open_or_merge(std::move(inc), at, cfg.merge_window_ms, count_repeat);
        if (up.incident && (up.created || up.reopened)) opened.push_back(up.incident->id);
    }

    template <typename Build>
    void drive(Hysteresis& h, bool on, bool off, const std::string& key, double observed, IncidentStore& store,
               Build&& build) {
        switch (h.update(on, off, now)) {
            case Hysteresis::Edge::Opened:
                open(build(), store, false, now);
                break;
            case Hysteresis::Edge::Resolved:
                store.resolve(key, now);
                break;
            case Hysteresis::Edge::None:
                if (h.is_open() && on && !store.touch(key, now, observed)) open(build(), store, false, now);
                break;
        }
    }

    double core_percent(const telemetry::ProcessSample& p) const {
        const double lp = cfg.logical_processors > 0 ? static_cast<double>(cfg.logical_processors) : 1.0;
        return finite_or(p.cpu_percent, 0.0) * lp;
    }

    Incident base(const char* detector, const std::string& key, Severity sev, Level risk) const {
        Incident inc;
        inc.detector = detector;
        inc.key = key;
        inc.severity = sev;
        inc.risk = risk;
        inc.auto_recovery_allowed = false;  // nothing here acts only on OpenSweep-owned state
        return inc;
    }

    static void set_confidence(Incident& inc, const incident::ConfidenceInputs& ci) {
        inc.confidence = incident::compute_confidence(ci);
        inc.confidence_level = incident::confidence_level(inc.confidence);
    }

    // ----------------------------------------------------------------------------------------
    // Minute bookkeeping.
    void close_minute(std::int64_t m) {
        for (auto& sl : slots) {
            if (!sl.used || !sl.seen_this_minute) continue;
            sl.minutes.push({m, sl.cur_priv_mb, sl.cur_handles, sl.cur_threads});
            sl.seen_this_minute = false;
        }
        SysPoint p = cur_sys;
        p.minute = m;
        p.latency = lat_n ? lat_sum / lat_n : kNaN;
        sys.push(p);
        if (std::isfinite(p.latency) && ph_latency.update(p.latency)) ph_latency_change = now;
        if (std::isfinite(p.nonpaged) && ph_pool.update(p.nonpaged / kMB)) ph_pool_change = now;
        lat_sum = 0.0;
        lat_n = 0;
    }

    // Last `window` minute points of a process field; growth statistics.
    Growth growth(const ProcSlot& sl, double ProcPoint::*field, std::size_t window, std::size_t min_minutes) {
        Growth g;
        const std::size_t n = std::min(window, sl.minutes.size());
        if (n < min_minutes + 1 || n < 3) return g;
        xs.clear();
        ys.clear();
        for (std::size_t i = sl.minutes.size() - n; i < sl.minutes.size(); ++i) {
            const auto& pt = sl.minutes[i];
            xs.push_back(static_cast<double>(pt.minute));
            ys.push_back(pt.*field);
        }
        g.from_minute = sl.minutes[sl.minutes.size() - n].minute;
        g.to_minute = sl.minutes.back().minute;
        if (g.to_minute - g.from_minute < static_cast<std::int64_t>(min_minutes)) return g;
        const auto slope = stats::theil_sen_slope(xs, ys, scratch);
        if (!slope) return g;
        g.ok = true;
        g.slope = *slope;
        g.inc = stats::increasing_fraction(ys);
        g.first = ys.front();
        g.last = ys.back();
        g.r = stats::pearson(xs, ys).value_or(0.0);
        g.n = n;
        double peak = ys.front();
        for (double y : ys) {
            peak = std::max(peak, y);
            g.drawdown = std::max(g.drawdown, peak - y);
        }
        return g;
    }

    // Processes whose `pf` moved together with the system series `sf` over the last `minutes`
    // minutes (Pearson r >= 0.6 and growth > 0), largest growth first.
    std::vector<Contributor> correlate(double SysPoint::*sf, double ProcPoint::*pf, std::size_t minutes) {
        struct Cand {
            const ProcSlot* sl;
            double r;
            double delta;
        };
        std::vector<Cand> cands;
        const std::size_t n = std::min(minutes, sys.size());
        if (n < 3) return {};
        const std::size_t s0 = sys.size() - n;
        for (const auto& sl : slots) {
            if (!sl.used || sl.minutes.size() < 3) continue;
            xs.clear();
            ys.clear();
            std::size_t j = 0;
            for (std::size_t i = s0; i < sys.size(); ++i) {
                const auto& sp = sys[i];
                while (j < sl.minutes.size() && sl.minutes[j].minute < sp.minute) ++j;
                if (j < sl.minutes.size() && sl.minutes[j].minute == sp.minute) {
                    xs.push_back(sp.*sf);
                    ys.push_back(sl.minutes[j].*pf);
                }
            }
            if (ys.size() < 3) continue;
            const auto r = stats::pearson(xs, ys);
            const double delta = ys.back() - ys.front();
            if (r && *r >= 0.6 && delta > 0.0) cands.push_back({&sl, *r, delta});
        }
        std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.delta > b.delta; });
        std::vector<Contributor> out;
        for (std::size_t i = 0; i < cands.size() && i < kMaxContributors; ++i) {
            Contributor c;
            c.name = cands[i].sl->name;
            c.kind = "process";
            c.pid = cands[i].sl->pid;
            c.correlation = std::round(cands[i].r * 100.0) / 100.0;
            out.push_back(std::move(c));
        }
        return out;
    }

    std::string contributor_list(const std::vector<Contributor>& cs) const {
        std::string s;
        for (std::size_t i = 0; i < cs.size() && i < 3; ++i) {
            if (i) s += ", ";
            s += cs[i].name;
        }
        return s;
    }

    // ----------------------------------------------------------------------------------------
    // Process tracking.
    ProcSlot* slot_for(const telemetry::ProcessSample& p, IncidentStore& store) {
        ProcSlot* free_slot = nullptr;
        ProcSlot* lru = nullptr;
        for (auto& sl : slots) {
            if (sl.used && sl.pid == p.pid && sl.name == p.name) return &sl;
            if (!sl.used && !free_slot) free_slot = &sl;
            if (sl.used && (!lru || sl.last_seen < lru->last_seen)) lru = &sl;
        }
        ProcSlot* sl = free_slot ? free_slot : lru;
        if (!sl) return nullptr;
        if (sl->used) release(*sl, store);
        sl->used = true;
        sl->pid = p.pid;
        sl->name = p.name;
        sl->first_seen = now;
        sl->last_seen = now;
        sl->seen_this_minute = false;
        sl->minutes.clear();
        sl->cpu.reset();
        sl->leak.reset();
        sl->handles.reset();
        sl->threads.reset();
        const std::string suffix = "|" + p.name + "|" + std::to_string(p.pid);
        sl->leak_key = "memory_growth" + suffix;
        sl->cpu_key = "cpu_runaway" + suffix;
        sl->handle_key = "handle_growth" + suffix;
        sl->thread_key = "thread_growth" + suffix;
        return sl;
    }

    void release(ProcSlot& sl, IncidentStore& store) {
        if (sl.leak.open) store.resolve(sl.leak_key, now);
        if (sl.cpu.is_open()) store.resolve(sl.cpu_key, now);
        if (sl.handles.open) store.resolve(sl.handle_key, now);
        if (sl.threads.open) store.resolve(sl.thread_key, now);
        sl.used = false;
        sl.minutes.clear();
        sl.cpu.reset();
        sl.leak.reset();
        sl.handles.reset();
        sl.threads.reset();
    }

    void update_processes(const Sample& s, const baseline::BaselineSet& b, IncidentStore& store) {
        for (const auto& p : s.top) {
            if (is_idle_process(p)) continue;
            ProcSlot* sl = slot_for(p, store);
            if (!sl) continue;
            sl->last_seen = now;
            sl->seen_this_minute = true;
            sl->cur_priv_mb = static_cast<double>(p.private_bytes) / kMB;
            sl->cur_handles = static_cast<double>(p.handles);
            sl->cur_threads = static_cast<double>(p.threads);
            cpu_runaway(*sl, p, s, b, store);
        }
    }

    void cpu_runaway(ProcSlot& sl, const telemetry::ProcessSample& p, const Sample& s,
                     const baseline::BaselineSet& b, IncidentStore& store) {
        const bool fg = p.foreground || (s.foreground_pid != 0 && p.pid == s.foreground_pid);
        const double core = core_percent(p);
        const bool on = !fg && core >= cfg.runaway_core_percent;
        const bool off = fg || core < cfg.runaway_resolve_core_percent;
        drive(sl.cpu, on, off, sl.cpu_key, core, store, [&] {
            Incident inc = base("cpu_runaway", sl.cpu_key, Severity::Warning, Level::Low);
            const double minutes = static_cast<double>(now - sl.cpu.on_since()) / kMinuteD;
            inc.title = fmt("Sustained high CPU in background process %s", p.name.c_str());
            inc.summary = fmt(
                "%s used about %.0f%% of one logical processor for %.0f min while not in the foreground. "
                "System CPU was %.0f%%, which is correlated with this process's activity.",
                p.name.c_str(), core, minutes, s.cpu_percent);
            inc.symptoms = {"A background process kept at least one processor core busy",
                            "Fans, battery drain or reduced responsiveness may be noticeable"};
            const auto& cb = b.get(Metric::CpuPercent);
            const auto z = cb.z(s.cpu_percent, 2.0);
            inc.evidence.push_back({"process.cpuCorePercent", "%", core, kNaN, kNaN, sl.cpu.on_since(), now,
                                    fmt("pid %u, sustained >= %.0f%% of a core", p.pid, cfg.runaway_core_percent)});
            inc.evidence.push_back({"cpuPercent", "%", s.cpu_percent, cb.median(), z.value_or(kNaN),
                                    sl.cpu.on_since(), now, "system-wide CPU in the same window"});
            Contributor c{p.name, "process", p.pid,
                          s.cpu_percent > 0 ? std::clamp(p.cpu_percent / s.cpu_percent, 0.0, 1.0) : 1.0};
            inc.contributors.push_back(c);
            inc.recommended_actions = {
                fmt("If %s is not expected to be busy, close or restart it.", p.name.c_str()),
                "Check whether it is updating, indexing or scanning; such work is often expected and ends by itself.",
                "OpenSweep does not terminate processes automatically."};
            int signals = 0;
            if (z && *z >= 2.0) ++signals;
            if (cfg.logical_processors > 0 && s.cpu_queue_length >= cfg.logical_processors) ++signals;
            set_confidence(inc, {std::clamp(core / 200.0, 0.0, 1.0), static_cast<double>(now - sl.cpu.on_since()),
                                 static_cast<double>(cfg.runaway_open_ms), signals, cb.ready()});
            return inc;
        });
    }

    // Shared logic of the per-process growth detectors (memory, handles, threads).
    template <typename Build>
    void growth_detector(GrowthTrack& t, const Growth& g, bool cond, double resolve_slope, std::int64_t resolve_ms,
                         const std::string& key, IncidentStore& store, Build&& build) {
        if (cond) {
            t.clear_since = -1;
            if (!t.open) {
                t.open = true;
                open(build(), store, false, now);
            } else if (!store.touch(key, now, g.slope)) {
                open(build(), store, false, now);
            }
            return;
        }
        if (!t.open) return;
        if (g.ok && g.slope < resolve_slope) {
            if (t.clear_since < 0) t.clear_since = now;
            if (now - t.clear_since >= resolve_ms) {
                store.resolve(key, now);
                t.reset();
            }
        } else {
            t.clear_since = -1;
            store.touch(key, now);
        }
    }

    void minute_process_detectors(const Sample& s, const baseline::BaselineSet& b, IncidentStore& store) {
        const auto& commit_b = b.get(Metric::CommitPercent);
        const double commit_pct = trace::metric_value(s, Metric::CommitPercent);
        const Growth sys_commit = system_growth(&SysPoint::commit_pct, cfg.leak_window_minutes);
        for (auto& sl : slots) {
            if (!sl.used) continue;
            if (now - sl.last_seen > cfg.process_forget_ms) {
                release(sl, store);
                continue;
            }
            // ---- memory_growth
            {
                const Growth g = growth(sl, &ProcPoint::priv_mb, cfg.leak_window_minutes, cfg.leak_min_minutes);
                const bool cond = g.ok && g.slope >= cfg.leak_min_slope_mb_per_min &&
                                  g.last - g.first >= cfg.leak_min_growth_mb && g.inc >= cfg.leak_min_increasing &&
                                  g.drawdown <= cfg.growth_max_drawdown * (g.last - g.first);
                const bool suspected = cond && g.inc >= cfg.leak_suspect_increasing &&
                                       g.to_minute - g.from_minute >= static_cast<std::int64_t>(cfg.leak_suspect_minutes);
                auto build = [&] {
                    const Severity sev = commit_pct >= cfg.commit_high_percent ? Severity::Error : Severity::Warning;
                    const Level risk = commit_pct >= cfg.commit_high_percent ? Level::High
                                       : commit_pct >= cfg.commit_warn_percent ? Level::Medium
                                                                               : Level::Low;
                    Incident inc = base("memory_growth", sl.leak_key, sev, risk);
                    const double mins = static_cast<double>(g.to_minute - g.from_minute);
                    inc.title = suspected ? fmt("Suspected memory leak in %s", sl.name.c_str())
                                          : fmt("Sustained memory growth in %s", sl.name.c_str());
                    const bool commit_up = sys_commit.ok && sys_commit.slope > 0.0;
                    inc.summary = fmt(
                        "Private memory of %s grew from %.0f MB to %.0f MB over %.0f min (about %.1f MB/min; %.0f%% "
                        "of minutes increased).%s",
                        sl.name.c_str(), g.first, g.last, mins, g.slope, g.inc * 100.0,
                        commit_up ? " System commit charge rose in the same window and is correlated with this growth."
                                  : "");
                    inc.symptoms = {"Steadily increasing private memory of one process",
                                    "Less memory available for other applications over time"};
                    inc.evidence.push_back({"process.privateBytesSlope", "MB/min", g.slope, kNaN, kNaN,
                                            g.from_minute * kMinute, now,
                                            fmt("Theil-Sen slope over %zu minute samples", g.n)});
                    inc.evidence.push_back({"process.privateBytes", "MB", g.last, g.first, kNaN, g.from_minute * kMinute,
                                            now, "current value vs value at window start"});
                    inc.evidence.push_back({"commitPercent", "%", commit_pct, commit_b.median(),
                                            commit_b.z(commit_pct, 1.0).value_or(kNaN), g.from_minute * kMinute, now,
                                            "system commit charge"});
                    inc.contributors.push_back({sl.name, "process", sl.pid, std::clamp(g.r, 0.0, 1.0)});
                    inc.recommended_actions = {
                        fmt("Save your work in %s and restart it when convenient; this releases its memory.",
                            sl.name.c_str()),
                        fmt("Check for an update to %s; continuous growth can indicate a leak fixed in newer versions.",
                            sl.name.c_str()),
                        "OpenSweep does not restart applications automatically."};
                    int signals = 0;
                    if (commit_up) ++signals;
                    if (g.inc >= cfg.leak_suspect_increasing) ++signals;
                    if (g.r >= 0.9) ++signals;
                    const double strength =
                        0.5 * std::clamp((g.slope / cfg.leak_min_slope_mb_per_min - 1.0) / 4.0, 0.0, 1.0) +
                        0.5 * std::clamp((g.inc - cfg.leak_min_increasing) / (1.0 - cfg.leak_min_increasing), 0.0, 1.0);
                    set_confidence(inc, {strength, mins * kMinuteD,
                                         static_cast<double>(cfg.leak_min_minutes) * kMinuteD, signals,
                                         commit_b.ready()});
                    return inc;
                };
                const bool upgrade = sl.leak.open && suspected && !sl.leak.suspected;
                growth_detector(sl.leak, g, cond, cfg.leak_resolve_slope_mb_per_min, cfg.leak_resolve_ms, sl.leak_key,
                                store, build);
                if (upgrade) open(build(), store, false, now);  // refresh title/classification
                if (sl.leak.open && suspected) sl.leak.suspected = true;
            }
            // ---- handle_growth / thread_growth
            growth_kind(sl, sl.handles, &ProcPoint::handles, cfg.handle_min_count, cfg.handle_min_slope_per_min,
                        "handle_growth", "handles", sl.handle_key, store);
            growth_kind(sl, sl.threads, &ProcPoint::threads, cfg.thread_min_count, cfg.thread_min_slope_per_min,
                        "thread_growth", "threads", sl.thread_key, store);
        }
    }

    void growth_kind(ProcSlot& sl, GrowthTrack& t, double ProcPoint::*field, double min_count, double min_slope,
                     const char* detector, const char* what, const std::string& key, IncidentStore& store) {
        const Growth g = growth(sl, field, cfg.leak_window_minutes, cfg.growth_min_minutes);
        const bool cond = g.ok && g.last >= min_count && g.slope >= min_slope && g.inc >= cfg.leak_min_increasing &&
                          g.drawdown <= cfg.growth_max_drawdown * std::max(0.0, g.last - g.first);
        growth_detector(t, g, cond, min_slope / 4.0, cfg.growth_resolve_ms, key, store, [&] {
            Incident inc = base(detector, key, Severity::Warning, Level::Medium);
            const double mins = static_cast<double>(g.to_minute - g.from_minute);
            inc.title = fmt("Growing number of %s in %s", what, sl.name.c_str());
            inc.summary = fmt("%s held %.0f %s and gained about %.0f per minute over %.0f min (%.0f%% of minutes "
                              "increased). Continuous growth like this is correlated with resource leaks.",
                              sl.name.c_str(), g.last, what, g.slope, mins, g.inc * 100.0);
            inc.symptoms = {fmt("Steadily increasing %s count of one process", what)};
            inc.evidence.push_back({fmt("process.%s", what), "count", g.last, g.first, kNaN, g.from_minute * kMinute,
                                    now, fmt("Theil-Sen slope %.1f per minute", g.slope)});
            inc.contributors.push_back({sl.name, "process", sl.pid, std::clamp(g.r, 0.0, 1.0)});
            inc.recommended_actions = {fmt("Restart %s when convenient.", sl.name.c_str()),
                                       fmt("Check for an update to %s.", sl.name.c_str())};
            int signals = g.r >= 0.9 ? 1 : 0;
            if (g.inc >= cfg.leak_suspect_increasing) ++signals;
            set_confidence(inc, {std::clamp((g.slope / min_slope - 1.0) / 4.0, 0.0, 1.0), mins * kMinuteD,
                                 static_cast<double>(cfg.growth_min_minutes) * kMinuteD, signals, true});
            return inc;
        });
    }

    Growth system_growth(double SysPoint::*field, std::size_t window) {
        Growth g;
        const std::size_t n = std::min(window, sys.size());
        if (n < 3) return g;
        xs.clear();
        ys.clear();
        for (std::size_t i = sys.size() - n; i < sys.size(); ++i) {
            if (!std::isfinite(sys[i].*field)) continue;
            xs.push_back(static_cast<double>(sys[i].minute));
            ys.push_back(sys[i].*field);
        }
        if (ys.size() < 3) return g;
        const auto slope = stats::theil_sen_slope(xs, ys, scratch);
        if (!slope) return g;
        g.ok = true;
        g.slope = *slope;
        g.inc = stats::increasing_fraction(ys);
        g.first = ys.front();
        g.last = ys.back();
        g.n = ys.size();
        g.from_minute = static_cast<std::int64_t>(xs.front());
        g.to_minute = static_cast<std::int64_t>(xs.back());
        return g;
    }

    // ----------------------------------------------------------------------------------------
    // System-level detectors (every sample).
    void commit_exhaustion(const Sample& s, const BaselineSet& b, IncidentStore& store) {
        const double pct = trace::metric_value(s, Metric::CommitPercent);
        if (!std::isfinite(pct)) return;
        const Growth g = system_growth(&SysPoint::commit_pct, 10);
        // Extrapolate only a steady climb: a one-off step (an application starting) also produces a
        // positive Theil-Sen slope over a short window but is not a trend.
        const bool steady = g.ok && g.n >= 5 && g.inc >= cfg.commit_min_increasing;
        const auto tte = steady ? stats::time_to_threshold(pct, g.slope, 100.0) : std::nullopt;
        const bool on = pct >= cfg.commit_high_percent ||
                        (pct >= cfg.commit_warn_percent && tte && *tte <= cfg.commit_tte_minutes);
        const bool off = pct < cfg.commit_resolve_percent;
        drive(commit, on, off, k_commit, pct, store, [&] {
            const bool severe = pct >= 95.0 || (tte && *tte <= 15.0);
            Incident inc = base("commit_exhaustion", k_commit, severe ? Severity::Error : Severity::Warning, Level::High);
            inc.title = "Commit charge close to the limit";
            auto contributors = correlate(&SysPoint::commit_pct, &ProcPoint::priv_mb, 10);
            std::string tte_text = tte ? fmt(" At the recent rate the limit would be reached in about %.0f min.", *tte)
                                       : std::string();
            inc.summary = fmt("Committed memory is at %.1f%% of the commit limit.%s", pct, tte_text.c_str());
            if (!contributors.empty())
                inc.summary += " The increase is correlated with memory growth of " + contributor_list(contributors) + ".";
            inc.symptoms = {"Applications may fail to allocate memory or close unexpectedly",
                            "Windows may warn about low memory"};
            const auto& cb = b.get(Metric::CommitPercent);
            inc.evidence.push_back({"commitPercent", "%", pct, cb.median(), cb.z(pct, 1.0).value_or(kNaN),
                                    commit.on_since() >= 0 ? commit.on_since() : now, now,
                                    g.ok ? fmt("10-min Theil-Sen slope %.2f %%/min", g.slope) : std::string("slope n/a")});
            if (tte) inc.evidence.push_back({"commitTimeToLimit", "min", *tte, kNaN, kNaN, now - 10 * kMinute, now,
                                             "linear extrapolation"});
            inc.contributors = std::move(contributors);
            inc.recommended_actions = {"Save your work and close applications you are not using.",
                                       "Restart applications whose memory keeps growing.",
                                       "Keep the page file Windows-managed (or larger) so the commit limit can grow."};
            int signals = 0;
            if (s.mem_load_percent >= 90) ++signals;
            const auto hf = b.get(Metric::HardFaultsPerSec).z(s.hard_faults_per_sec, 5.0);
            if (b.get(Metric::HardFaultsPerSec).ready() && hf && *hf >= 3.0) ++signals;
            if (!inc.contributors.empty()) ++signals;
            const double strength = std::clamp((pct - cfg.commit_warn_percent) / (100.0 - cfg.commit_warn_percent), 0.0, 1.0);
            set_confidence(inc, {strength, static_cast<double>(now - commit.on_since()),
                                 static_cast<double>(cfg.commit_open_ms), signals, cb.ready()});
            return inc;
        });
    }

    void system_count(const Sample& s, const BaselineSet& b, IncidentStore& store, Metric m, Hysteresis& h,
                      const std::string& key, const char* detector, const char* what, double ProcPoint::*pf,
                      double SysPoint::*sf) {
        const double v = trace::metric_value(s, m);
        const auto& bl = b.get(m);
        const double med = bl.median();
        const auto z = bl.z(v, std::max(1.0, 0.01 * finite_or(med, 0.0)));
        const bool on = bl.ready() && z && *z >= cfg.system_z && std::isfinite(med) && v >= cfg.system_ratio * med;
        const bool off = !bl.ready() || !z || *z < cfg.system_z / 2.0 || v < 1.2 * med;
        drive(h, on, off, key, v, store, [&] {
            Incident inc = base(detector, key, Severity::Warning, Level::Medium);
            auto contributors = correlate(sf, pf, 15);
            inc.title = fmt("System %s far above this machine's baseline", what);
            inc.summary = fmt("The system holds %.0f %s, %.1fx its usual median of %.0f.", v, what, v / med, med);
            if (!contributors.empty())
                inc.summary += " The increase is correlated with growth in " + contributor_list(contributors) + ".";
            inc.symptoms = {fmt("Unusually high number of %s system-wide", what),
                            "Exhaustion can make new windows, processes or connections fail"};
            inc.evidence.push_back({info_name(m), "count", v, med, z.value_or(kNaN), h.on_since(), now,
                                    "robust z against the 24 h baseline"});
            inc.contributors = std::move(contributors);
            inc.recommended_actions = {"Restart the application whose count keeps growing, if one is listed.",
                                       "If no application stands out, a restart of Windows releases kernel objects."};
            const int signals = inc.contributors.empty() ? 0 : 1;
            set_confidence(inc, {incident::z_strength(z.value_or(0.0)), static_cast<double>(now - h.on_since()),
                                 static_cast<double>(cfg.system_open_ms), signals, bl.ready()});
            return inc;
        });
    }

    static std::string info_name(Metric m) { return trace::info(m).name; }

    void kernel_pool(const Sample& s, const BaselineSet& b, IncidentStore& store, bool paged) {
        const Metric m = paged ? Metric::KernelPaged : Metric::KernelNonpaged;
        Hysteresis& h = paged ? pool_paged : pool_nonpaged;
        const std::string& key = paged ? k_paged : k_nonpaged;
        const double v = trace::metric_value(s, m);
        if (!(v > 0.0)) return;
        const auto& bl = b.get(m);
        const double med = bl.median();
        const auto z = bl.z(v, std::max(16.0 * kMB, 0.01 * finite_or(med, 0.0)));
        const double abs_limit = paged ? cfg.pool_abs_paged_bytes : cfg.pool_abs_nonpaged_bytes;
        const bool rel = bl.ready() && z && *z >= cfg.pool_z && std::isfinite(med) &&
                         v - med >= std::max(cfg.pool_min_excess_bytes, cfg.pool_min_excess_ratio * med);
        const bool on = rel || v >= abs_limit;
        const bool off = !on && (!std::isfinite(med) || v < med + 0.5 * std::max(cfg.pool_min_excess_bytes,
                                                                                   cfg.pool_min_excess_ratio * med)) &&
                         v < 0.9 * abs_limit;
        drive(h, on, off, key, v / kMB, store, [&] {
            const char* which = paged ? "paged" : "non-paged";
            Incident inc = base("kernel_pool", key, Severity::Warning, Level::Medium);
            auto contributors = correlate(&SysPoint::handles, &ProcPoint::handles, 15);
            inc.title = fmt("Kernel %s pool usage unusually high", which);
            inc.summary = fmt("Kernel %s pool is %.0f MB%s.", which, v / kMB,
                              std::isfinite(med) ? fmt(" (usual median %.0f MB)", med / kMB).c_str() : "");
            if (!contributors.empty())
                inc.summary += " Handle counts of " + contributor_list(contributors) +
                               " rose in the same window (correlated; handles consume pool memory).";
            inc.symptoms = {"Kernel memory is growing; drivers or many open handles can exhaust it"};
            inc.evidence.push_back({info_name(m), "MB", v / kMB, finite_or(med, kNaN) / kMB, z.value_or(kNaN),
                                    h.on_since(), now,
                                    "pool tags are not visible to user-mode counters; use poolmon or WPR to attribute"});
            inc.contributors = std::move(contributors);
            inc.recommended_actions = {"Update chipset, network, storage and security-software drivers.",
                                       "If the value keeps rising, capture a pool trace with Windows Performance Recorder.",
                                       "Restarting Windows releases kernel pool memory."};
            int signals = 0;
            if (ph_pool_change >= 0 && now - ph_pool_change <= 30 * kMinute) ++signals;
            if (!inc.contributors.empty()) ++signals;
            const double strength = rel ? incident::z_strength(z.value_or(0.0)) : 0.5;
            set_confidence(inc, {strength, static_cast<double>(now - h.on_since()),
                                 static_cast<double>(cfg.system_open_ms), signals, bl.ready()});
            return inc;
        });
    }

    void storage_latency(const Sample& s, const BaselineSet& b, IncidentStore& store) {
        const double rd = finite_or(s.disk_read_latency_ms, 0.0);
        const double wr = finite_or(s.disk_write_latency_ms, 0.0);
        const bool read_worse = rd >= wr;
        const double v = read_worse ? rd : wr;
        const Metric m = read_worse ? Metric::DiskReadLatencyMs : Metric::DiskWriteLatencyMs;
        lat_sum += v;
        ++lat_n;
        const auto& bl = b.get(m);
        const auto z = bl.z(v, cfg.latency_min_mad_ms);
        const bool on = v >= cfg.latency_floor_ms && (!bl.ready() || (z && *z >= cfg.latency_z));
        const bool off = v < cfg.latency_floor_ms;
        drive(latency, on, off, k_latency, v, store, [&] {
            Incident inc = base("storage_latency", k_latency,
                                v >= cfg.latency_error_ms ? Severity::Error : Severity::Warning, Level::Low);
            inc.title = "Storage latency spike";
            // Contributors: processes with hard faults (page reads from disk) in this sample.
            double total_hf = 0.0;
            for (const auto& p : s.top) total_hf += static_cast<double>(p.hard_faults_per_sec);
            std::vector<Contributor> cs;
            if (total_hf > 0.0) {
                for (const auto& p : s.top) {
                    const double share = static_cast<double>(p.hard_faults_per_sec) / total_hf;
                    if (share >= 0.1 && !is_idle_process(p)) cs.push_back({p.name, "process", p.pid, share});
                }
                std::sort(cs.begin(), cs.end(),
                          [](const Contributor& a, const Contributor& c) { return a.correlation > c.correlation; });
                if (cs.size() > kMaxContributors) cs.resize(kMaxContributors);
                for (auto& c : cs) c.correlation = std::round(c.correlation * 100.0) / 100.0;
            }
            inc.summary = fmt("Average %s latency reached %.0f ms for %.0f s (usual median %.1f ms).",
                              read_worse ? "read" : "write", v,
                              static_cast<double>(now - latency.on_since()) / 1000.0, finite_or(bl.median(), 0.0));
            if (!cs.empty())
                inc.summary += " Hard page faults of " + contributor_list(cs) + " are correlated with this period.";
            inc.symptoms = {"Applications may pause or load slowly", "File operations take longer than usual"};
            inc.evidence.push_back({info_name(m), "ms", v, bl.median(), z.value_or(kNaN), latency.on_since(), now,
                                    fmt("absolute floor %.0f ms", cfg.latency_floor_ms)});
            inc.evidence.push_back({"diskQueueLength", "count", s.disk_queue_length, b.get(Metric::DiskQueueLength).median(),
                                    kNaN, latency.on_since(), now, "outstanding I/O"});
            inc.contributors = std::move(cs);
            inc.recommended_actions = {"Check which applications read or write heavily (Resource Monitor, Disk tab).",
                                       "Avoid running large copies or scans together with interactive work.",
                                       "If this repeats, check the drive's health (SMART) and free space."};
            int signals = 0;
            if (s.disk_queue_length >= 2.0) ++signals;
            const auto& hb = b.get(Metric::HardFaultsPerSec);
            const auto hz = hb.z(s.hard_faults_per_sec, 5.0);
            if (hb.ready() && hz && *hz >= 3.0) ++signals;
            if (ph_latency_change >= 0 && now - ph_latency_change <= 30 * kMinute) ++signals;
            const double strength = bl.ready() ? incident::z_strength(z.value_or(0.0))
                                               : std::clamp(v / (4.0 * cfg.latency_floor_ms), 0.0, 1.0);
            set_confidence(inc, {strength, static_cast<double>(now - latency.on_since()),
                                 static_cast<double>(cfg.latency_open_ms), signals, bl.ready()});
            return inc;
        });
    }

    void dpc_isr(const Sample& s, const BaselineSet& b, IncidentStore& store) {
        const double d = finite_or(s.dpc_percent, 0.0);
        const double i = finite_or(s.interrupt_percent, 0.0);
        const auto& db = b.get(Metric::DpcPercent);
        const auto& ib = b.get(Metric::InterruptPercent);
        const auto dz = db.z(d, cfg.dpc_min_mad);
        const auto iz = ib.z(i, cfg.dpc_min_mad);
        const bool d_on = d >= cfg.dpc_floor_percent && (!db.ready() || (dz && *dz >= cfg.dpc_z));
        const bool i_on = i >= cfg.dpc_floor_percent && (!ib.ready() || (iz && *iz >= cfg.dpc_z));
        const bool on = d_on || i_on;
        const bool off = d < cfg.dpc_floor_percent && i < cfg.dpc_floor_percent;
        drive(dpc, on, off, k_dpc, std::max(d, i), store, [&] {
            Incident inc = base("dpc_isr", k_dpc, Severity::Warning, Level::Low);
            inc.title = "High DPC / interrupt time";
            inc.summary = fmt("Processors spent %.1f%% in DPCs and %.1f%% in interrupts (usual medians %.1f%% / %.1f%%). "
                              "This is correlated with audio dropouts, input lag and stutter.",
                              d, i, finite_or(db.median(), 0.0), finite_or(ib.median(), 0.0));
            inc.symptoms = {"Audio crackling, mouse stutter or frame drops are possible"};
            inc.evidence.push_back({"dpcPercent", "%", d, db.median(), dz.value_or(kNaN), dpc.on_since(), now,
                                    "driver deferred procedure calls"});
            inc.evidence.push_back({"interruptPercent", "%", i, ib.median(), iz.value_or(kNaN), dpc.on_since(), now,
                                    "hardware interrupt service routines"});
            for (const auto& [ts, name] : recent_drivers) {
                if (!name.empty() && now - ts <= kHour) inc.contributors.push_back({name, "driver", 0, 0.3});
            }
            if (!inc.contributors.empty())
                inc.summary += " Recent driver failures (" + contributor_list(inc.contributors) +
                               ") were logged in the same hour.";
            inc.recommended_actions = {"Update network, storage, audio, GPU and chipset drivers.",
                                       "Use a DPC latency tool or Windows Performance Recorder to see which driver is busy."};
            const int signals = inc.contributors.empty() ? 0 : 1;
            const bool ready = d_on ? db.ready() : ib.ready();
            const double strength =
                ready ? incident::z_strength(std::max(dz.value_or(0.0), iz.value_or(0.0)))
                      : std::clamp(std::max(d, i) / (4.0 * cfg.dpc_floor_percent), 0.0, 1.0);
            set_confidence(inc, {strength, static_cast<double>(now - dpc.on_since()),
                                 static_cast<double>(cfg.dpc_open_ms), signals, ready});
            return inc;
        });
    }

    void thermal_throttling(const Sample& s, const BaselineSet& b, IncidentStore& store) {
        const double f = s.cpu_freq_percent;
        if (!std::isfinite(f) || f <= 0.0) return;
        const auto& fb = b.get(Metric::CpuFreqPercent, Context::Active);
        const double ref = fb.ready() && std::isfinite(fb.median()) ? fb.median() : 100.0;
        const bool busy = s.cpu_percent >= cfg.thermal_busy_cpu_percent;
        const bool low = fb.ready() ? f <= cfg.thermal_freq_ratio * ref : f <= cfg.thermal_abs_freq_percent;
        const bool on = busy && low;
        const bool off = !busy || f >= 0.9 * ref;
        drive(thermal, on, off, k_thermal, f, store, [&] {
            Incident inc = base("thermal_throttling", k_thermal, Severity::Warning, Level::Medium);
            inc.title = "Processor running below its usual speed under load";
            inc.summary = fmt("While the CPU was %.0f%% busy, processor performance stayed at %.0f%% (usual %.0f%% when "
                              "busy). This is correlated with thermal or power limits; a power-saving plan or battery "
                              "mode can show the same pattern.",
                              s.cpu_percent, f, ref);
            inc.symptoms = {"Slower performance under sustained load", "Possibly high temperatures or loud fans"};
            inc.evidence.push_back({"cpuFreqPercent", "%", f, fb.median(), fb.z(f, 2.0).value_or(kNaN),
                                    thermal.on_since(), now, "busy-time (active) baseline"});
            inc.evidence.push_back({"cpuPercent", "%", s.cpu_percent, kNaN, kNaN, thermal.on_since(), now, "load"});
            inc.recommended_actions = {"Make sure vents are free and clean dust from fans and heatsinks.",
                                       "Check the power plan / power mode and whether the device runs on battery.",
                                       "Check firmware (BIOS/UEFI) updates from the device manufacturer."};
            int signals = 0;
            if (kind_history[static_cast<std::size_t>(EventKind::Thermal)].size() > 0) {
                const auto& kh = kind_history[static_cast<std::size_t>(EventKind::Thermal)];
                if (now - kh.back() <= kDay) ++signals;
            }
            const double strength = std::clamp((ref - f) / std::max(ref, 1.0) * 2.0, 0.0, 1.0);
            set_confidence(inc, {strength, static_cast<double>(now - thermal.on_since()),
                                 static_cast<double>(cfg.thermal_open_ms), signals, fb.ready()});
            return inc;
        });
    }

    // ----------------------------------------------------------------------------------------
    // Event-driven detectors.
    EventCounter& counter(const std::string& key) {
        for (auto& c : counters)
            if (c.key == key) return c;
        if (counters.size() >= kMaxCounters) {
            auto lru = std::min_element(counters.begin(), counters.end(),
                                        [](const EventCounter& a, const EventCounter& b) { return a.last < b.last; });
            *lru = EventCounter{};
            lru->key = key;
            return *lru;
        }
        counters.push_back(EventCounter{});
        counters.back().key = key;
        return counters.back();
    }

    void on_event(const EventRecord& e, const BaselineSet& b, IncidentStore& store) {
        const auto ki = static_cast<std::size_t>(e.kind);
        if (ki < kind_history.size()) kind_history[ki].push(e.ts_ms);
        if (e.kind == EventKind::DriverFailure && !e.subject.empty()) {
            recent_drivers[recent_driver_next] = {e.ts_ms, e.subject};
            recent_driver_next = (recent_driver_next + 1) % recent_drivers.size();
        }
        if (e.kind == EventKind::BootPerformance) {
            boot(e, b, store);
            return;
        }
        for (const auto& r : rules) {
            if (r.kind != e.kind) continue;
            const std::string subject = r.per_subject ? (e.subject.empty() ? std::string("unknown") : e.subject) : "";
            const std::string key = r.per_subject ? std::string(r.detector) + "|" + subject : std::string(r.detector);
            EventCounter& c = counter(key);
            c.add(e.ts_ms);
            const std::size_t n = c.count_since(e.ts_ms - r.window_ms);
            const bool critical = r.immediate_if_critical && e.level == 1;
            if (n < r.count && !critical) continue;
            Severity sev = r.severity;
            Level risk = r.risk;
            if (r.kind == EventKind::Whea && e.level <= 2) {
                sev = Severity::Critical;  // uncorrected / fatal hardware error
                risk = Level::High;
            }
            if (r.escalate_count > 0 && n >= r.escalate_count && static_cast<int>(r.escalated) > static_cast<int>(sev))
                sev = r.escalated;
            open(event_incident(r, e, key, subject, n, sev, risk), store, true, e.ts_ms);
        }
    }

    Incident event_incident(const EventRule& r, const EventRecord& e, const std::string& key, const std::string& subject,
                            std::size_t n, Severity sev, Level risk) const {
        Incident inc = base(r.detector, key, sev, risk);
        const double hours = static_cast<double>(r.window_ms) / kHourD;
        const char* subj = subject.c_str();
        const std::string d = r.detector;
        if (d == "service_crash_loop") {
            inc.title = fmt("Service %s keeps failing", subj);
            inc.summary = fmt("The service %s stopped unexpectedly %zu times within %.0f h.", subj, n, hours);
            inc.symptoms = {"A background feature may be unavailable or restart repeatedly"};
            inc.recommended_actions = {"Check the System event log entry for the service's error code.",
                                       fmt("Update or repair the application that installs %s.", subj),
                                       "OpenSweep does not restart third-party services automatically."};
        } else if (d == "app_reliability") {
            inc.title = fmt("%s crashes or hangs repeatedly", subj);
            inc.summary = fmt("%s crashed or stopped responding %zu times within %.0f h.", subj, n, hours);
            inc.symptoms = {"The application closes unexpectedly or freezes"};
            inc.recommended_actions = {fmt("Update %s, or repair/reinstall it.", subj),
                                       "Check the Reliability Monitor for the faulting module."};
        } else if (d == "disk_deterioration") {
            inc.title = fmt("Disk errors reported for %s", subj);
            inc.summary = fmt("%zu disk error event(s) for %s within %.0f h (latest: event %u from %s). Repeated disk "
                              "errors are correlated with failing storage hardware or cabling.",
                              n, subj, hours, e.event_id, e.provider.c_str());
            inc.symptoms = {"Slow or failing file operations", "Risk of data loss"};
            inc.recommended_actions = {"Back up important data now.",
                                       "Check the drive's health (SMART) with the manufacturer's tool.",
                                       "Run 'chkdsk /scan' on the affected volume."};
        } else if (d == "driver_failure") {
            inc.title = fmt("Driver or device failure: %s", subj);
            inc.summary = fmt("Windows reported %zu failure(s) loading or running %s within %.0f h.", n, subj, hours);
            inc.symptoms = {"A device or feature may not work"};
            inc.recommended_actions = {fmt("Update or reinstall the driver for %s from the device manufacturer.", subj),
                                       "Check Device Manager for devices with a warning sign."};
        } else if (d == "gpu_timeout") {
            inc.title = "Graphics driver stopped responding and recovered";
            inc.summary = fmt("%zu display driver timeout(s) (TDR) within %.0f h.", n, hours);
            inc.symptoms = {"Screen flickers or goes black briefly", "Games or video may crash"};
            inc.recommended_actions = {"Update the graphics driver from the GPU vendor.",
                                       "Check GPU temperatures and remove overclocks."};
        } else if (d == "whea_error") {
            const bool fatal = e.level <= 2;
            inc.title = fatal ? "Uncorrected hardware error reported" : "Corrected hardware errors reported";
            inc.summary = fmt("%zu WHEA hardware error event(s) within %.0f h%s.", n, hours,
                              fatal ? "; at least one was not corrected" : "");
            inc.symptoms = {"Possible instability, crashes or data corruption"};
            inc.recommended_actions = {"Remove overclocking / undervolting and check firmware (BIOS/UEFI) updates.",
                                       "Test memory (Windows Memory Diagnostic) and check temperatures.",
                                       "Back up important data."};
        } else if (d == "unexpected_shutdown") {
            inc.title = "Unexpected shutdown";
            inc.summary = fmt("Windows recorded %zu unexpected shutdown(s) within %.0f h.", n, hours);
            inc.symptoms = {"The computer restarted or turned off without a normal shutdown"};
            inc.recommended_actions = {"Check power supply, battery and cables.",
                                       "Look for a bugcheck (blue screen) or thermal event in the same period."};
        } else if (d == "bugcheck") {
            inc.title = "System crash (bugcheck)";
            inc.summary = fmt("Windows recorded %zu bugcheck(s) within %.0f h (latest: %s).", n, hours,
                              e.message.empty() ? "no details" : e.message.c_str());
            inc.symptoms = {"Blue screen and automatic restart"};
            inc.recommended_actions = {"Update drivers, especially graphics, storage and network.",
                                       "Test memory and check for hardware (WHEA) errors in the same period."};
        } else if (d == "resource_exhaustion") {
            inc.title = "Windows reported low virtual memory";
            inc.summary = fmt("The resource-exhaustion detector fired %zu time(s) within %.0f h.", n, hours);
            inc.symptoms = {"Applications may fail to allocate memory"};
            inc.recommended_actions = {"Close applications you do not use; restart those with growing memory.",
                                       "Keep the page file Windows-managed."};
        } else {  // thermal_event
            inc.title = "Thermal event reported";
            inc.summary = fmt("Windows reported %zu thermal event(s) within %.0f h.", n, hours);
            inc.symptoms = {"The system may slow down or shut down to protect itself"};
            inc.recommended_actions = {"Make sure vents are free and clean dust from fans.",
                                       "Check the power mode and firmware updates."};
        }
        inc.evidence.push_back({std::string("events.") + telemetry::to_string(e.kind), "count", static_cast<double>(n),
                                kNaN, kNaN, e.ts_ms - r.window_ms, e.ts_ms,
                                fmt("latest: event %u from %s (level %u)", e.event_id, e.provider.c_str(),
                                    static_cast<unsigned>(e.level))});
        if (r.per_subject && subject != "unknown") {
            const char* kind = d == "service_crash_loop" ? "service"
                               : d == "app_reliability"  ? "app"
                               : d == "driver_failure"   ? "driver"
                                                         : "device";
            inc.contributors.push_back({subject, kind, 0, 1.0});
        }
        // Event evidence is direct (logged by Windows); confidence grows with repeats.
        const double strength = std::clamp(static_cast<double>(n) / (2.0 * std::max<std::uint32_t>(r.count, 1)), 0.0, 1.0);
        set_confidence(inc, {strength, 0.0, 0.0, n > r.count ? 1 : 0, true});
        return inc;
    }

    void boot(const EventRecord& e, const BaselineSet& b, IncidentStore& store) {
        const auto& bb = b.boot();
        const double v = e.value;
        if (!std::isfinite(v) || v <= 0.0 || !bb.ready()) return;
        const double med = bb.median();
        const auto z = bb.z(v, cfg.boot_min_mad_ms);
        const bool cond = v >= cfg.boot_ratio * med && v - med >= cfg.boot_min_excess_ms && z && *z >= cfg.boot_z;
        if (!cond) {
            if (v <= 1.2 * med) store.resolve(k_boot, e.ts_ms);
            return;
        }
        Incident inc = base("boot_degradation", k_boot, Severity::Warning, Level::Low);
        inc.title = "Startup slower than usual";
        inc.summary = fmt("The last boot took %.0f s; the usual median on this machine is %.0f s.", v / 1000.0,
                          med / 1000.0);
        inc.symptoms = {"Windows takes longer to become usable after power-on"};
        inc.evidence.push_back({"bootDurationMs", "ms", v, med, z.value_or(kNaN), e.ts_ms, e.ts_ms,
                                fmt("baseline of %llu boots", static_cast<unsigned long long>(bb.samples()))});
        inc.recommended_actions = {"Review startup apps (Task Manager, Startup apps) added recently.",
                                   "Check whether Windows Update installed updates during this boot."};
        set_confidence(inc, {incident::z_strength(*z), 0.0, 0.0, 0, true});
        open(std::move(inc), store, true, e.ts_ms);
    }

    // ----------------------------------------------------------------------------------------
    const EventRule* rule_for(std::string_view detector) const {
        for (const auto& r : rules)
            if (detector == r.detector) return &r;
        return nullptr;
    }

    void sweep(IncidentStore& store) {
        to_resolve.clear();
        store.for_each_open([&](const Incident& inc) {
            std::int64_t quiet = cfg.stale_resolve_ms;
            if (const EventRule* r = rule_for(inc.detector)) quiet = r->quiet_ms;
            else if (inc.detector == "boot_degradation") quiet = 7 * kDay;
            if (now - inc.last_seen_ms >= quiet) to_resolve.push_back(inc.key);
        });
        for (const auto& k : to_resolve) store.resolve(k, now);
    }
};

// ---------------------------------------------------------------------------------------------
Guard::Guard(GuardConfig cfg) : impl_(std::make_unique<Impl>(cfg)) {}
Guard::~Guard() = default;

const GuardConfig& Guard::config() const noexcept { return impl_->cfg; }
const std::vector<std::string>& Guard::last_opened() const noexcept { return impl_->opened; }

std::size_t Guard::tracked_processes() const noexcept {
    std::size_t n = 0;
    for (const auto& sl : impl_->slots)
        if (sl.used) ++n;
    return n;
}

void Guard::evaluate(const Sample& s, std::span<const EventRecord> new_events, const BaselineSet& baselines,
                     IncidentStore& store) {
    Impl& im = *impl_;
    im.opened.clear();
    im.now = s.ts_ms;
    const std::int64_t mb = floor_div(s.ts_ms, kMinute);
    const bool tick = im.minute >= 0 && mb != im.minute;
    if (tick) im.close_minute(im.minute);
    im.minute = mb;

    im.cur_sys.commit_pct = trace::metric_value(s, Metric::CommitPercent);
    im.cur_sys.paged = static_cast<double>(s.kernel_paged);
    im.cur_sys.nonpaged = static_cast<double>(s.kernel_nonpaged);
    im.cur_sys.handles = static_cast<double>(s.handles);
    im.cur_sys.threads = static_cast<double>(s.threads);

    im.update_processes(s, baselines, store);
    if (tick) {
        im.minute_process_detectors(s, baselines, store);
        im.sweep(store);
    }
    im.commit_exhaustion(s, baselines, store);
    im.system_count(s, baselines, store, Metric::Handles, im.handles_sys, im.k_handles, "handle_exhaustion", "handles",
                    &ProcPoint::handles, &SysPoint::handles);
    im.system_count(s, baselines, store, Metric::Threads, im.threads_sys, im.k_threads, "thread_exhaustion", "threads",
                    &ProcPoint::threads, &SysPoint::threads);
    im.kernel_pool(s, baselines, store, true);
    im.kernel_pool(s, baselines, store, false);
    im.storage_latency(s, baselines, store);
    im.dpc_isr(s, baselines, store);
    im.thermal_throttling(s, baselines, store);
    for (const auto& e : new_events) im.on_event(e, baselines, store);
}

ScoreInputs Guard::score_inputs(const Sample& latest, const IncidentStore& store) const {
    const Impl& im = *impl_;
    ScoreInputs in;
    store.for_each_open([&](const Incident& inc) {
        switch (inc.severity) {
            case Severity::Critical: ++in.open_critical; break;
            case Severity::Error: ++in.open_error; break;
            case Severity::Warning: ++in.open_warning; break;
            case Severity::Info: ++in.open_info; break;
        }
    });
    const std::int64_t now = latest.ts_ms;
    auto count = [&](EventKind k, std::int64_t window) {
        const auto& h = im.kind_history[static_cast<std::size_t>(k)];
        std::size_t c = 0;
        for (std::size_t i = 0; i < h.size(); ++i)
            if (h[i] >= now - window && h[i] <= now) ++c;
        return c;
    };
    in.crashes_24h = count(EventKind::AppCrash, kDay) + count(EventKind::AppHang, kDay);
    in.service_failures_24h = count(EventKind::ServiceFailure, kDay);
    in.shutdowns_7d = count(EventKind::UnexpectedShutdown, 7 * kDay) + count(EventKind::Bugcheck, 7 * kDay);
    in.whea_7d = count(EventKind::Whea, 7 * kDay);
    in.disk_errors_7d = count(EventKind::DiskError, 7 * kDay);
    in.commit_percent = trace::metric_value(latest, Metric::CommitPercent);
    in.mem_load_percent = static_cast<double>(latest.mem_load_percent);
    return in;
}

}  // namespace opensweep::guard
