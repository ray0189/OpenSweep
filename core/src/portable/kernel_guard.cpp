#include "kernel_guard.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

#include "guard.hpp"
#include "kernel_stats.hpp"
#include "ringbuffer.hpp"
#include "stats.hpp"
#include "trace.hpp"

namespace opensweep::kguard {

using guard::Hysteresis;
using incident::Contributor;
using incident::Incident;
using incident::IncidentStore;
using incident::Level;
using incident::Severity;
using telemetry::KernelDriverSample;
using telemetry::KernelProcessSample;
using telemetry::PolicyKind;
using telemetry::ProcessSample;
using telemetry::Sample;
using telemetry::WorkloadClass;
using trace::Metric;

namespace {

constexpr std::int64_t kMinute = 60'000;
constexpr double kMinuteD = 60'000.0;
constexpr double kMB = 1024.0 * 1024.0;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr std::size_t kDriverMinutes = 60;
constexpr std::size_t kMaxNotes = 8;
constexpr const char* kMicro = "\xC2\xB5s";  // "µs" in UTF-8

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

bool contains(const std::vector<std::uint32_t>& v, std::uint32_t x) { return std::find(v.begin(), v.end(), x) != v.end(); }

struct MinuteRec {
    std::int64_t minute = 0;
    double max_us = 0.0;
    double share = 0.0;  // % of all CPUs spent in this driver's DPCs/ISRs during the minute
};

struct DriverTrack {
    bool used = false;
    std::string name;
    std::string key;
    std::int64_t last_seen = 0;
    double cur_max = 0.0;
    double cur_total = 0.0;
    bool seen_this_minute = false;
    RingBuffer<MinuteRec> minutes{kDriverMinutes};
    Hysteresis h;
    double peak_us = 0.0;  // largest single routine while the incident is open
};

struct ProcTrack {
    bool used = false;
    std::uint32_t pid = 0;
    std::string name;
    std::int64_t last_seen = 0;
    Hysteresis disk;
    Hysteresis paging;
    std::string disk_key;
    std::string paging_key;
    double peak_bps = 0.0;
    double peak_faults = 0.0;
};

}  // namespace

struct KernelGuard::Impl {
    explicit Impl(KernelGuardConfig c) : cfg(std::move(c)) {
        if (cfg.logical_processors == 0) cfg.logical_processors = 1;
        drivers.resize(kMaxTrackedDrivers);
        procs.resize(kMaxTrackedProcs);
    }

    KernelGuardConfig cfg;
    std::vector<DriverTrack> drivers;
    std::vector<ProcTrack> procs;
    std::int64_t minute = -1;
    std::int64_t now = 0;
    KernelAssessment* out = nullptr;

    static void set_confidence(Incident& inc, const incident::ConfidenceInputs& ci) {
        inc.confidence = incident::compute_confidence(ci);
        inc.confidence_level = incident::confidence_level(inc.confidence);
    }

    void open(Incident inc, IncidentStore& store) {
        const auto up = store.open_or_merge(std::move(inc), now, cfg.merge_window_ms, false);
        if (up.incident && (up.created || up.reopened)) out->opened.push_back(up.incident->id);
    }

    void note(std::string n) {
        if (out->notes.size() < kMaxNotes) out->notes.push_back(std::move(n));
    }

    // ---- driver_latency -----------------------------------------------------------------------
    DriverTrack* driver_slot(const std::string& name, IncidentStore& store) {
        DriverTrack* free_slot = nullptr;
        DriverTrack* lru = nullptr;
        for (auto& d : drivers) {
            if (d.used && d.name == name) return &d;
            if (!d.used && !free_slot) free_slot = &d;
            if (d.used && (!lru || d.last_seen < lru->last_seen)) lru = &d;
        }
        DriverTrack* d = free_slot ? free_slot : lru;
        if (!d) return nullptr;
        if (d->used && d->h.is_open()) store.resolve(d->key, now);
        d->used = true;
        d->name = name;
        d->key = "driver_latency|" + name;
        d->last_seen = now;
        d->cur_max = d->cur_total = 0.0;
        d->seen_this_minute = false;
        d->minutes.clear();
        d->h = Hysteresis(0, cfg.driver_resolve_ms, 3 * kMinute);
        d->peak_us = 0.0;
        return d;
    }

    void observe_drivers(const Sample& s, IncidentStore& store) {
        for (const KernelDriverSample& k : s.kernel_drivers) {
            if (k.name.empty() || k.name == kstats::kUnknownName || k.name == kstats::kOtherName) continue;
            if (!std::isfinite(k.max_us) || !std::isfinite(k.total_us)) continue;
            DriverTrack* d = driver_slot(k.name, store);
            if (!d) continue;
            d->last_seen = now;
            d->seen_this_minute = true;
            d->cur_max = std::max(d->cur_max, k.max_us);
            d->cur_total += std::max(0.0, k.total_us);
        }
    }

    struct DriverVerdict {
        bool on = false;
        std::size_t spikes = 0;
        double worst = 0.0;
        bool share_high = false;
        double share = 0.0;
        double share_median = kNaN;
    };

    DriverVerdict driver_condition(const DriverTrack& d, std::int64_t m) const {
        DriverVerdict v;
        const std::int64_t from = m - static_cast<std::int64_t>(cfg.driver_lookback_minutes) + 1;
        for (std::size_t i = 0; i < d.minutes.size(); ++i) {
            const MinuteRec& r = d.minutes[i];
            if (r.minute < from) continue;
            if (r.max_us >= cfg.driver_spike_us) ++v.spikes;
            v.worst = std::max(v.worst, r.max_us);
        }
        const std::size_t n = d.minutes.size();
        const std::size_t recent = cfg.driver_share_minutes;
        if (n >= cfg.driver_share_ready_minutes + recent && recent > 0) {
            std::vector<double> hist;
            hist.reserve(n - recent);
            for (std::size_t i = 0; i + recent < n; ++i) hist.push_back(d.minutes[i].share);
            const double med = stats::median(hist).value_or(0.0);
            v.share_median = med;
            const double thr = std::max({cfg.driver_share_min_percent, cfg.driver_share_ratio * med,
                                         med + cfg.driver_share_min_excess});
            bool all = true;
            for (std::size_t i = n - recent; i < n; ++i) all = all && d.minutes[i].share >= thr;
            v.share_high = all;
            v.share = d.minutes[n - 1].share;
        } else if (n > 0) {
            v.share = d.minutes[n - 1].share;
        }
        v.on = v.spikes >= cfg.driver_spike_minutes || v.share_high;
        return v;
    }

    Incident driver_incident(const DriverTrack& d, const DriverVerdict& v, const Sample& s,
                             const baseline::BaselineSet& b) const {
        Incident inc;
        inc.detector = "driver_latency";
        inc.key = d.key;
        inc.risk = Level::Low;
        inc.auto_recovery_allowed = false;
        const double worst = std::max(v.worst, d.peak_us);
        inc.severity = worst >= 4.0 * cfg.driver_spike_us ? Severity::Error : Severity::Warning;
        const std::string desc = kstats::driver_description(d.name);
        const std::string label = desc.empty() ? d.name : d.name + " (" + desc + ")";
        inc.title = fmt("Driver %s correlated with latency spikes (max %.0f %s)", d.name.c_str(), worst, kMicro);
        if (v.spikes >= cfg.driver_spike_minutes) {
            inc.summary = fmt("Single DPC/ISR routines of %s ran for %.0f %s or longer in %zu of the last %zu minutes "
                              "(longest %.0f %s). Long DPCs and interrupts are correlated with audio dropouts, input "
                              "lag and stutter.",
                              label.c_str(), cfg.driver_spike_us, kMicro, v.spikes, cfg.driver_lookback_minutes, worst,
                              kMicro);
        } else {
            inc.summary = fmt("%s spent %.2f%% of all processor time in DPCs/ISRs, far above its usual %.2f%%. This is "
                              "correlated with stutter and reduced responsiveness.",
                              label.c_str(), v.share, finite_or(v.share_median, 0.0));
        }
        inc.symptoms = {"Audio crackling, mouse or video stutter, or input lag are possible",
                        "The problem may come and go with device activity (network, storage, USB, graphics)"};
        const std::int64_t from = (minute - static_cast<std::int64_t>(cfg.driver_lookback_minutes) + 1) * kMinute;
        inc.evidence.push_back({"driver.maxDpcIsrUs", "us", worst, cfg.driver_spike_us, kNaN, from, now,
                                fmt("%s: minutes with a routine >= %.0f us: %zu of %zu", d.name.c_str(),
                                    cfg.driver_spike_us, v.spikes, cfg.driver_lookback_minutes)});
        inc.evidence.push_back({"driver.dpcIsrSharePercent", "%", v.share, v.share_median, kNaN, from, now,
                                "share of all processors (latest minute) vs this driver's median"});
        const auto& db = b.get(Metric::DpcPercent);
        inc.evidence.push_back({"dpcPercent", "%", finite_or(s.dpc_percent, 0.0), db.median(),
                                db.z(finite_or(s.dpc_percent, 0.0), 0.25).value_or(kNaN), from, now,
                                "system-wide DPC time (performance counter)"});
        Contributor c;
        c.name = d.name;
        c.kind = "driver";
        c.correlation = std::clamp(static_cast<double>(v.spikes) / static_cast<double>(cfg.driver_lookback_minutes) +
                                       (v.share_high ? 0.5 : 0.0),
                                   0.1, 1.0);
        c.correlation = std::round(c.correlation * 100.0) / 100.0;
        inc.contributors.push_back(c);
        inc.recommended_actions = {
            desc.empty()
                ? fmt("Update %s from the device or PC manufacturer, or roll it back in Device Manager if the spikes "
                      "started after a driver update.",
                      d.name.c_str())
                : fmt("Update the %s %s from the device or PC manufacturer, or roll it back in Device Manager if the "
                      "spikes started after a driver update.",
                      desc.c_str(), d.name.c_str()),
            "Check power settings: use the Balanced or Best performance power mode and, for that device, turn off "
            "aggressive power saving (e.g. link-state power management, USB selective suspend) if available.",
            "OpenSweep never changes or disables drivers automatically."};
        int signals = 0;
        const auto dz = db.z(finite_or(s.dpc_percent, 0.0), 0.25);
        if (db.ready() && dz && *dz >= 3.0) ++signals;
        if (v.spikes >= cfg.driver_spike_minutes && v.share_high) ++signals;
        if (worst >= 2.0 * cfg.driver_spike_us) ++signals;
        const double strength =
            v.spikes >= cfg.driver_spike_minutes
                ? std::clamp((worst / cfg.driver_spike_us - 1.0) / 3.0, 0.0, 1.0) * 0.5 +
                      std::clamp(static_cast<double>(v.spikes) / static_cast<double>(cfg.driver_lookback_minutes), 0.0,
                                 1.0) *
                          0.5
                : std::clamp(v.share / std::max(cfg.driver_share_min_percent * 4.0, 1e-9), 0.0, 1.0);
        set_confidence(inc, {strength, static_cast<double>(std::max<std::size_t>(v.spikes, 1)) * kMinuteD,
                             static_cast<double>(cfg.driver_spike_minutes) * kMinuteD, signals, true});
        return inc;
    }

    void close_minute(std::int64_t m, const Sample& s, const baseline::BaselineSet& b, IncidentStore& store) {
        const double denom = 60.0 * 1e6 * cfg.logical_processors;
        const std::int64_t at = (m + 1) * kMinute;
        for (auto& d : drivers) {
            if (!d.used) continue;
            if (!d.seen_this_minute && now - d.last_seen > cfg.driver_forget_ms && !d.h.is_open()) {
                d.used = false;
                d.minutes.clear();
                continue;
            }
            d.minutes.push({m, d.seen_this_minute ? d.cur_max : 0.0,
                            d.seen_this_minute ? d.cur_total / denom * 100.0 : 0.0});
            d.cur_max = d.cur_total = 0.0;
            d.seen_this_minute = false;
            const DriverVerdict v = driver_condition(d, m);
            const std::int64_t saved_now = now;
            now = std::max(now, at);
            switch (d.h.update(v.on, !v.on, at)) {
                case Hysteresis::Edge::Opened:
                    d.peak_us = v.worst;
                    open(driver_incident(d, v, s, b), store);
                    break;
                case Hysteresis::Edge::Resolved:
                    store.resolve(d.key, now);
                    d.peak_us = 0.0;
                    break;
                case Hysteresis::Edge::None:
                    if (d.h.is_open() && v.on) {
                        const bool worse = v.worst > d.peak_us;
                        d.peak_us = std::max(d.peak_us, v.worst);
                        // Refresh the text when the peak grew (the title carries the max).
                        if (worse || !store.touch(d.key, now, v.worst)) open(driver_incident(d, v, s, b), store);
                    }
                    break;
            }
            now = saved_now;
        }
    }

    // ---- per-process (disk_hog, paging_hog) -------------------------------------------------
    ProcTrack* proc_slot(const KernelProcessSample& k, IncidentStore& store, bool create) {
        ProcTrack* free_slot = nullptr;
        ProcTrack* lru = nullptr;
        for (auto& p : procs) {
            if (p.used && p.pid == k.pid && p.name == k.name) return &p;
            if (!p.used && !free_slot) free_slot = &p;
            if (p.used && (!lru || p.last_seen < lru->last_seen)) lru = &p;
        }
        if (!create) return nullptr;
        ProcTrack* p = free_slot ? free_slot : lru;
        if (!p) return nullptr;
        if (p->used) release(*p, store);
        p->used = true;
        p->pid = k.pid;
        p->name = k.name;
        p->last_seen = now;
        p->disk = Hysteresis(cfg.disk_hog_open_ms, cfg.disk_hog_resolve_ms, cfg.max_gap_ms);
        p->paging = Hysteresis(cfg.paging_open_ms, cfg.paging_resolve_ms, cfg.max_gap_ms);
        const std::string suffix = "|" + k.name + "|" + std::to_string(k.pid);
        p->disk_key = "disk_hog" + suffix;
        p->paging_key = "paging_hog" + suffix;
        p->peak_bps = p->peak_faults = 0.0;
        return p;
    }

    void release(ProcTrack& p, IncidentStore& store) {
        if (p.disk.is_open()) store.resolve(p.disk_key, now);
        if (p.paging.is_open()) store.resolve(p.paging_key, now);
        p.used = false;
        p.disk.reset();
        p.paging.reset();
    }

    static std::uint32_t foreground_pid(const Sample& s) {
        if (s.foreground_pid != 0) return s.foreground_pid;
        for (const auto& p : s.top)
            if (p.foreground) return p.pid;
        return 0;
    }

    static const ProcessSample* find_top(const Sample& s, std::uint32_t pid) {
        for (const auto& p : s.top)
            if (p.pid == pid) return &p;
        return nullptr;
    }

    // Verified-background check shared by both hog detectors; nullptr + note when not eligible.
    const ProcessSample* verified_background(const Sample& s, const ProcTrack& t, ProcessSample& copy,
                                             WorkloadClass& cls, const char* detector) {
        const ProcessSample* p = find_top(s, t.pid);
        if (!p) {
            note(fmt("%s: %s (pid %u) not a candidate: not in the sampled process list, cannot be verified", detector,
                     t.name.c_str(), t.pid));
            return nullptr;
        }
        copy = *p;
        const std::uint32_t fg = foreground_pid(s);
        if (fg != 0 && copy.pid == fg) copy.foreground = true;
        cls = sched::classify(copy, sched::HistoryView{}, cfg.lists);
        if (auto why = sched::protection_reason(copy, cls, cfg.lists)) {
            note(fmt("%s: %s (pid %u) not a candidate: %s", detector, t.name.c_str(), t.pid, why->c_str()));
            return nullptr;
        }
        if (cls != WorkloadClass::Background) {
            note(fmt("%s: %s (pid %u) not a candidate: workload %s is not verified background", detector,
                     t.name.c_str(), t.pid, telemetry::to_string(cls)));
            return nullptr;
        }
        return p;
    }

    policy::Recommendation candidate(const ProcessSample& p, WorkloadClass cls, policy::Area area, PolicyKind kind,
                                     std::int64_t value, int risk, const Sample& s) const {
        policy::Recommendation r;
        r.area = area;
        r.target.pid = p.pid;
        r.target.name = p.name;
        r.target.process_start_ms = p.start_ms;
        r.workload = cls;
        r.kind = kind;
        r.value = value;
        r.risk = risk;
        r.beneficiary_pid = foreground_pid(s);
        r.id = std::string(policy::to_string(area)) + ":" + telemetry::to_string(kind) + ":" + std::to_string(p.pid);
        return r;
    }

    void disk_hog(const Sample& s, const baseline::BaselineSet& b, IncidentStore& store, const KernelInputs& in,
                  std::vector<bool>& present) {
        const double rd = finite_or(s.disk_read_latency_ms, 0.0);
        const double wr = finite_or(s.disk_write_latency_ms, 0.0);
        const bool read_worse = rd >= wr;
        const double lat = std::max(rd, wr);
        const auto& lb = b.get(read_worse ? Metric::DiskReadLatencyMs : Metric::DiskWriteLatencyMs);
        const auto lz = lb.z(lat, cfg.disk_latency_min_mad_ms);
        const bool lat_high = lb.ready() ? (lz && *lz >= cfg.disk_latency_z && lat >= cfg.disk_latency_min_ms)
                                         : lat >= cfg.disk_latency_floor_ms;
        const std::uint32_t fg = foreground_pid(s);
        const double total = std::max(finite_or(s.kernel.disk_bytes_per_sec, 0.0), 1.0);

        for (const KernelProcessSample& k : s.kernel_processes) {
            if (k.pid == 0 || k.pid == kstats::kOtherPid) continue;
            const double rate = finite_or(k.read_bps, 0.0) + finite_or(k.write_bps, 0.0);
            ProcTrack* t = proc_slot(k, store, rate >= cfg.disk_hog_bps || k.hard_faults_per_sec >= cfg.paging_min_faults);
            if (!t) continue;
            present[static_cast<std::size_t>(t - procs.data())] = true;
            t->last_seen = now;
            const bool is_fg = fg != 0 && k.pid == fg;
            const bool on = rate >= cfg.disk_hog_bps && !is_fg && lat_high;
            const bool off = rate < 0.5 * cfg.disk_hog_bps || !lat_high || is_fg;
            auto build = [&] {
                Incident inc;
                inc.detector = "disk_hog";
                inc.key = t->disk_key;
                inc.severity = Severity::Warning;
                inc.risk = Level::Low;
                inc.auto_recovery_allowed = false;
                const double mins = static_cast<double>(now - t->disk.on_since()) / 60'000.0;
                inc.title = fmt("Heavy disk activity by %s correlated with slow storage", k.name.c_str());
                inc.summary = fmt("%s (not in the foreground) read %.1f MB/s and wrote %.1f MB/s for %.0f min while "
                                  "disk latency was %.0f ms (usual median %.1f ms). Its I/O is correlated with the "
                                  "slow storage in this period.",
                                  k.name.c_str(), k.read_bps / kMB, k.write_bps / kMB, mins, lat,
                                  finite_or(lb.median(), 0.0));
                inc.symptoms = {"Applications and file operations may load or save slowly",
                                "The disk activity light / Task Manager disk graph stays busy"};
                inc.evidence.push_back({"process.diskBytesPerSec", "MB/s", rate / kMB, cfg.disk_hog_bps / kMB, kNaN,
                                        t->disk.on_since(), now,
                                        fmt("pid %u, mean I/O latency %.1f ms, max %.1f ms", k.pid, k.mean_latency_ms,
                                            k.max_latency_ms)});
                inc.evidence.push_back({read_worse ? "diskReadLatencyMs" : "diskWriteLatencyMs", "ms", lat,
                                        lb.median(), lz.value_or(kNaN), t->disk.on_since(), now,
                                        lb.ready() ? "robust z against this machine's baseline"
                                                   : "baseline not ready: absolute floor"});
                Contributor c{k.name, "process", k.pid, std::round(std::clamp(rate / total, 0.0, 1.0) * 100.0) / 100.0};
                inc.contributors.push_back(c);
                inc.recommended_actions = {
                    fmt("If %s is a backup, sync, update or scan, schedule it for when you are not working.",
                        k.name.c_str()),
                    "In Balanced mode OpenSweep can lower its priority (EcoQoS / below-normal) while this lasts, if it "
                    "is a verified background process; the change is journaled and rolled back automatically.",
                    "If storage is often slow, check free space and the drive's health."};
                int signals = 0;
                if (s.disk_queue_length >= 2.0) ++signals;
                if (k.mean_latency_ms >= cfg.disk_latency_floor_ms) ++signals;
                if (rate / total >= 0.5) ++signals;
                const double strength = 0.5 * std::clamp(rate / (4.0 * cfg.disk_hog_bps), 0.0, 1.0) +
                                        0.5 * (lb.ready() ? incident::z_strength(lz.value_or(0.0))
                                                          : std::clamp(lat / (4.0 * cfg.disk_latency_floor_ms), 0.0, 1.0));
                set_confidence(inc, {strength, static_cast<double>(now - t->disk.on_since()),
                                     static_cast<double>(cfg.disk_hog_open_ms), signals, lb.ready()});
                return inc;
            };
            switch (t->disk.update(on, off, now)) {
                case Hysteresis::Edge::Opened: open(build(), store); break;
                case Hysteresis::Edge::Resolved: store.resolve(t->disk_key, now); break;
                case Hysteresis::Edge::None:
                    if (t->disk.is_open() && on && !store.touch(t->disk_key, now, rate / kMB)) open(build(), store);
                    break;
            }
            if (!t->disk.is_open() || !on) continue;
            ProcessSample copy;
            WorkloadClass cls = WorkloadClass::Unknown;
            const ProcessSample* p = verified_background(s, *t, copy, cls, "disk_hog");
            if (!p) continue;
            policy::Recommendation r;
            if (!contains(in.ecoqos_active, p->pid)) {
                r = candidate(*p, cls, policy::Area::Disk, PolicyKind::EcoQoS, 1, 1, s);
                r.expected_effect = "the background process runs at efficient clocks and yields to foreground work";
            } else if (p->priority_class == policy::kPriorityNormal) {
                r = candidate(*p, cls, policy::Area::Disk, PolicyKind::PriorityClass, policy::kPriorityBelowNormal, 2, s);
                r.expected_effect = "foreground threads (and their I/O) win ties against this background process";
            } else {
                continue;  // escalation exhausted
            }
            r.reason = "sustained heavy disk I/O by " + p->name + " while disk latency is above baseline (disk_hog)";
            r.evidence.push_back(fmt("%s issued %.1f MB/s of disk I/O (>= %.0f MB/s)", p->name.c_str(), rate / kMB,
                                     cfg.disk_hog_bps / kMB));
            r.evidence.push_back(fmt("disk latency %.1f ms (median %.1f ms)", lat, finite_or(lb.median(), 0.0)));
            out->recommendations.push_back(std::move(r));
        }
    }

    void paging_hog(const Sample& s, IncidentStore& store, const KernelInputs& in, std::vector<bool>& present) {
        double total = finite_or(s.kernel.hard_faults_per_sec, 0.0);
        if (total <= 0.0)
            for (const auto& k : s.kernel_processes) total += finite_or(k.hard_faults_per_sec, 0.0);
        const bool pressure = in.memory_level >= memory::PressureLevel::Elevated;
        for (const KernelProcessSample& k : s.kernel_processes) {
            if (k.pid == 0 || k.pid == kstats::kOtherPid) continue;
            const double f = finite_or(k.hard_faults_per_sec, 0.0);
            ProcTrack* t = proc_slot(k, store, false);
            if (!t) continue;  // created by disk_hog() when the process had enough faults
            present[static_cast<std::size_t>(t - procs.data())] = true;
            const double share = total > 0.0 ? f / total : 0.0;
            const bool on = pressure && f >= cfg.paging_min_faults && share >= cfg.paging_min_share;
            const bool off = !pressure || share < cfg.paging_resolve_share || f < 0.5 * cfg.paging_min_faults;
            auto build = [&] {
                Incident inc;
                inc.detector = "paging_hog";
                inc.key = t->paging_key;
                inc.severity = in.memory_level >= memory::PressureLevel::Significant ? Severity::Warning : Severity::Info;
                inc.risk = Level::Low;
                inc.auto_recovery_allowed = false;
                inc.title = fmt("%s correlated with most hard page faults during memory pressure", k.name.c_str());
                inc.summary = fmt("While memory pressure was %s, %.0f%% of the hard page faults (%.0f/s) were "
                                  "attributed to %s. Reading its memory back from disk is correlated with the "
                                  "slowdown in this period.",
                                  memory::to_string(in.memory_level), share * 100.0, f, k.name.c_str());
                inc.symptoms = {"Switching to or between applications can pause", "Disk activity while memory is low"};
                inc.evidence.push_back({"process.hardFaultsPerSec", "1/s", f, kNaN, kNaN, t->paging.on_since(), now,
                                        fmt("pid %u, %.0f%% of all hard faults", k.pid, share * 100.0)});
                inc.evidence.push_back({"hardFaultsPerSec", "1/s", total, kNaN, kNaN, t->paging.on_since(), now,
                                        "all hard faults seen by kernel tracing"});
                inc.contributors.push_back({k.name, "process", k.pid, std::round(share * 100.0) / 100.0});
                inc.recommended_actions = {
                    fmt("Close %s or other large applications you are not using to free memory.", k.name.c_str()),
                    "In Balanced mode OpenSweep can lower the memory priority of a verified background process while "
                    "the pressure lasts; the change is journaled and rolled back automatically.",
                    "If this happens regularly, review startup apps or consider more RAM."};
                const int signals = in.memory_level >= memory::PressureLevel::Significant ? 1 : 0;
                set_confidence(inc, {std::clamp((share - cfg.paging_min_share) / (1.0 - cfg.paging_min_share), 0.0, 1.0),
                                     static_cast<double>(now - t->paging.on_since()),
                                     static_cast<double>(cfg.paging_open_ms), signals, true});
                return inc;
            };
            switch (t->paging.update(on, off, now)) {
                case Hysteresis::Edge::Opened: open(build(), store); break;
                case Hysteresis::Edge::Resolved: store.resolve(t->paging_key, now); break;
                case Hysteresis::Edge::None:
                    if (t->paging.is_open() && on && !store.touch(t->paging_key, now, f)) open(build(), store);
                    break;
            }
            if (!t->paging.is_open() || !on) continue;
            // Contributor in the memory incidents that are open right now.
            std::vector<std::string> keys;
            store.for_each_open([&](const Incident& inc) {
                if (keys.size() < 8 && (inc.detector == "commit_exhaustion" || inc.detector == "memory_growth" ||
                                        inc.detector == "storage_latency"))
                    keys.push_back(inc.key);
            });
            for (const auto& key : keys)
                store.add_contributor(key, {k.name, "process", k.pid, std::round(share * 100.0) / 100.0});

            ProcessSample copy;
            WorkloadClass cls = WorkloadClass::Unknown;
            const ProcessSample* p = verified_background(s, *t, copy, cls, "paging_hog");
            if (!p) continue;
            policy::Recommendation r;
            if (!contains(in.memory_priority_active, p->pid)) {
                r = candidate(*p, cls, policy::Area::Memory, PolicyKind::MemoryPriority, policy::kMemoryPriorityLow, 1, s);
                r.expected_effect = "the memory manager trims and repurposes this process's pages before the foreground's";
            } else if (!contains(in.ecoqos_active, p->pid)) {
                r = candidate(*p, cls, policy::Area::Memory, PolicyKind::EcoQoS, 1, 1, s);
                r.expected_effect = "background allocation/CPU rate is reduced while pressure lasts";
            } else {
                continue;
            }
            r.reason = "memory pressure " + std::string(memory::to_string(in.memory_level)) + "; " + p->name +
                       " accounts for most hard page faults (paging_hog)";
            r.evidence.push_back(fmt("%s: %.0f hard faults/s, %.0f%% of all", p->name.c_str(), f, share * 100.0));
            out->recommendations.push_back(std::move(r));
        }
    }
};

// ---------------------------------------------------------------------------------------------
KernelGuard::KernelGuard(KernelGuardConfig cfg) : impl_(std::make_unique<Impl>(std::move(cfg))) {}
KernelGuard::~KernelGuard() = default;

const KernelGuardConfig& KernelGuard::config() const noexcept { return impl_->cfg; }

std::size_t KernelGuard::tracked_drivers() const noexcept {
    return static_cast<std::size_t>(std::count_if(impl_->drivers.begin(), impl_->drivers.end(),
                                                  [](const DriverTrack& d) { return d.used; }));
}

std::size_t KernelGuard::tracked_processes() const noexcept {
    return static_cast<std::size_t>(
        std::count_if(impl_->procs.begin(), impl_->procs.end(), [](const ProcTrack& p) { return p.used; }));
}

KernelAssessment KernelGuard::evaluate(const Sample& s, const baseline::BaselineSet& b, IncidentStore& store,
                                       const KernelInputs& in) {
    Impl& im = *impl_;
    KernelAssessment out;
    if (!s.kernel_valid) return out;
    im.out = &out;
    im.now = s.ts_ms;
    const std::int64_t m = floor_div(s.ts_ms, kMinute);
    if (im.minute >= 0 && m != im.minute) im.close_minute(im.minute, s, b, store);
    im.minute = m;
    im.observe_drivers(s, store);

    std::vector<bool> present(im.procs.size(), false);
    im.disk_hog(s, b, store, in, present);
    im.paging_hog(s, store, in, present);
    // Tracked processes without activity in this sample: their conditions are off.
    for (std::size_t i = 0; i < im.procs.size(); ++i) {
        ProcTrack& t = im.procs[i];
        if (!t.used || present[i]) continue;
        if (t.disk.update(false, true, im.now) == Hysteresis::Edge::Resolved) store.resolve(t.disk_key, im.now);
        if (t.paging.update(false, true, im.now) == Hysteresis::Edge::Resolved) store.resolve(t.paging_key, im.now);
        if (!t.disk.is_open() && !t.paging.is_open() && im.now - t.last_seen > im.cfg.process_forget_ms)
            im.release(t, store);
    }
    im.out = nullptr;
    return out;
}

}  // namespace opensweep::kguard
