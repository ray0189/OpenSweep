#include "scorecard.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <vector>

namespace opensweep::scorecard {

namespace {

constexpr const char* kKeys[] = {"bootMs",           "idleCpuPercent", "idleRamUsedBytes", "diskLatencyMs",
                                 "hardFaultsPerSec", "stutterPerHour", "crashesPerWeek"};
constexpr std::size_t kMaxFileBytes = 256 * 1024;

std::optional<double>* field(Metrics& m, std::size_t i) {
    switch (i) {
        case 0: return &m.boot_ms;
        case 1: return &m.idle_cpu_percent;
        case 2: return &m.idle_ram_used_bytes;
        case 3: return &m.disk_latency_ms;
        case 4: return &m.hard_faults_per_sec;
        case 5: return &m.stutter_per_hour;
        default: return &m.crashes_per_week;
    }
}

const std::optional<double>* field(const Metrics& m, std::size_t i) { return field(const_cast<Metrics&>(m), i); }

json::Value opt_num(const std::optional<double>& v) {
    if (!v || !std::isfinite(*v)) return json::Value();
    return json::Value(*v);
}

bool finite(float v) { return std::isfinite(v); }

// Weighted mean helper.
struct Mean {
    double sum = 0.0;
    double weight = 0.0;
    std::size_t rows = 0;
    void add(double v, double w) {
        sum += v * w;
        weight += w;
        ++rows;
    }
    std::optional<double> get(std::size_t min_rows) const {
        if (rows < min_rows || weight <= 0.0) return std::nullopt;
        return sum / weight;
    }
};

}  // namespace

json::Value metrics_to_json(const Metrics& m) {
    json::Value o = json::Value::object();
    for (std::size_t i = 0; i < std::size(kKeys); ++i) o.set(kKeys[i], opt_num(*field(m, i)));
    return o;
}

std::optional<Metrics> metrics_from_json(const json::Value& v) {
    if (!v.is_object() || v.size() != std::size(kKeys)) return std::nullopt;
    Metrics m;
    for (std::size_t i = 0; i < std::size(kKeys); ++i) {
        const json::Value* x = v.find(kKeys[i]);
        if (!x) return std::nullopt;
        if (x->is_null()) continue;
        if (!x->is_number() || !std::isfinite(x->as_double()) || x->as_double() < 0.0) return std::nullopt;
        *field(m, i) = x->as_double();
    }
    return m;
}

json::Value baseline_to_json(const Baseline& b) {
    json::Value o = json::Value::object();
    o.set("capturedAt", b.captured_at);
    o.set("metrics", metrics_to_json(b.metrics));
    return o;
}

// ---------------------------------------------------------------------------------------------
void Scorecard::observe(const telemetry::Sample& s, bool fullscreen) {
    std::uint8_t flags = 0;
    if (s.kernel_valid && s.kernel.running) {
        flags |= kKernel;
        const double worst = std::max(s.kernel.worst_dpc_us, s.kernel.worst_isr_us);
        if (std::isfinite(worst) && worst >= kStutterUs) flags |= kStutter;
    }
    if (fullscreen) flags |= kFullscreen;
    if (flags == 0 || s.ts_ms <= 0) return;
    const std::int64_t idx = s.ts_ms / trace::kTier1BucketMs;
    if (!minutes_.empty() && minutes_.back().index == idx) {
        minutes_.back().flags |= flags;
    } else if (minutes_.empty() || minutes_.back().index < idx) {
        minutes_.push_back({idx, flags});
    } else {
        // Clock moved backwards: merge into an existing minute if present, otherwise drop.
        auto it = std::find_if(minutes_.begin(), minutes_.end(), [&](const Minute& m) { return m.index == idx; });
        if (it != minutes_.end()) it->flags |= flags;
    }
    const std::int64_t oldest = idx - static_cast<std::int64_t>(kMaxMinutes);
    while (!minutes_.empty() && (minutes_.front().index <= oldest || minutes_.size() > kMaxMinutes))
        minutes_.pop_front();
}

bool Scorecard::fullscreen_minute(std::int64_t minute_index) const {
    auto it = std::lower_bound(minutes_.begin(), minutes_.end(), minute_index,
                               [](const Minute& m, std::int64_t i) { return m.index < i; });
    return it != minutes_.end() && it->index == minute_index && (it->flags & kFullscreen);
}

Metrics Scorecard::compute(const trace::Trace& tr, std::span<const telemetry::EventRecord> events,
                           std::int64_t now) const {
    using trace::Metric;
    Metrics m;
    const std::int64_t from = now - kWindowMs;

    // ---- trace 24 h tier (1-minute rows) --------------------------------------------------------
    Mean idle_cpu, idle_ram, disk, faults;
    const auto& t1 = tr.tier1();
    for (std::size_t i = 0; i < t1.size(); ++i) {
        const trace::AggRow& r = t1[i];
        if (r.ts_ms <= from || r.ts_ms > now || r.count == 0) continue;
        const double w = static_cast<double>(r.count);
        const float cpu = r.mean[trace::index(Metric::CpuPercent)];
        const float total = r.mean[trace::index(Metric::MemTotal)];
        const float avail = r.mean[trace::index(Metric::MemAvail)];
        const float rl = r.mean[trace::index(Metric::DiskReadLatencyMs)];
        const float wl = r.mean[trace::index(Metric::DiskWriteLatencyMs)];
        const float hf = r.mean[trace::index(Metric::HardFaultsPerSec)];
        const bool idle = finite(cpu) && cpu < kIdleCpuPercent && !fullscreen_minute(r.ts_ms / trace::kTier1BucketMs);
        if (idle) {
            idle_cpu.add(cpu, w);
            if (finite(total) && finite(avail) && total > 0.0f && total >= avail)
                idle_ram.add(static_cast<double>(total) - static_cast<double>(avail), w);
        }
        const double lat = std::max(finite(rl) ? static_cast<double>(rl) : 0.0, finite(wl) ? static_cast<double>(wl) : 0.0);
        if (lat > 0.0) disk.add(lat, w);
        if (finite(hf) && hf >= 0.0f) faults.add(hf, w);
    }
    m.idle_cpu_percent = idle_cpu.get(kMinMinutes);
    m.idle_ram_used_bytes = idle_ram.get(kMinMinutes);
    if (m.idle_ram_used_bytes) m.idle_ram_used_bytes = std::round(*m.idle_ram_used_bytes);
    m.disk_latency_ms = disk.get(kMinMinutes);
    m.hard_faults_per_sec = faults.get(kMinMinutes);

    // ---- stutter (kernel-traced minutes) -------------------------------------------------------
    {
        const std::int64_t from_idx = from / trace::kTier1BucketMs;
        const std::int64_t now_idx = now / trace::kTier1BucketMs;
        std::size_t traced = 0, stutter = 0;
        for (const Minute& mi : minutes_) {
            if (mi.index <= from_idx || mi.index > now_idx || !(mi.flags & kKernel)) continue;
            ++traced;
            if (mi.flags & kStutter) ++stutter;
        }
        if (traced >= kMinKernelMinutes)
            m.stutter_per_hour = static_cast<double>(stutter) * 60.0 / static_cast<double>(traced);
    }

    // ---- events ---------------------------------------------------------------------------------
    {
        std::vector<const telemetry::EventRecord*> boots;
        for (const auto& e : events)
            if (e.kind == telemetry::EventKind::BootPerformance && std::isfinite(e.value) && e.value > 0.0 &&
                e.ts_ms <= now)
                boots.push_back(&e);
        std::sort(boots.begin(), boots.end(), [](const auto* a, const auto* b) { return a->ts_ms > b->ts_ms; });
        if (boots.size() > kBootsForMedian) boots.resize(kBootsForMedian);
        if (!boots.empty()) {
            std::vector<double> v;
            for (const auto* b : boots) v.push_back(b->value);
            std::sort(v.begin(), v.end());
            const std::size_t n = v.size();
            m.boot_ms = n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
        }
    }
    if (first_start_ > 0) {  // without a known first start the covered time is unknown
        std::int64_t cover_from = std::max(now - kWeekMs, first_start_ - kEventBackfillMs);
        if (events.size() >= trace::kMaxEvents) {
            std::int64_t oldest = now;
            for (const auto& e : events) oldest = std::min(oldest, e.ts_ms);
            cover_from = std::max(cover_from, oldest);
        }
        const std::int64_t covered = now - cover_from;
        if (covered >= kEventBackfillMs) {
            std::size_t crashes = 0;
            for (const auto& e : events)
                if ((e.kind == telemetry::EventKind::AppCrash || e.kind == telemetry::EventKind::Bugcheck) &&
                    e.ts_ms > cover_from && e.ts_ms <= now)
                    ++crashes;
            m.crashes_per_week =
                static_cast<double>(crashes) * static_cast<double>(kWeekMs) / static_cast<double>(covered);
        }
    }
    return m;
}

double Scorecard::hours_measured(const trace::Trace& tr, std::int64_t now) const {
    const std::int64_t from = now - kWindowMs;
    std::size_t rows = 0;
    const auto& t1 = tr.tier1();
    for (std::size_t i = 0; i < t1.size(); ++i)
        if (t1[i].ts_ms > from && t1[i].ts_ms <= now && t1[i].count > 0) ++rows;
    return std::round(static_cast<double>(rows) / 6.0) / 10.0;  // rows / 60, one decimal
}

// ---- persistence ------------------------------------------------------------------------------
std::string Scorecard::serialize() const {
    json::Value o = json::Value::object();
    o.set("version", kVersion);
    o.set("firstStartMs", first_start_);
    o.set("baseline", baseline_ ? baseline_to_json(*baseline_) : json::Value());
    json::Value mins = json::Value::array();
    for (const Minute& mi : minutes_) {
        json::Value pair = json::Value::array();
        pair.push_back(mi.index);
        pair.push_back(static_cast<std::uint32_t>(mi.flags));
        mins.push_back(std::move(pair));
    }
    o.set("minutes", std::move(mins));
    return json::serialize(o) + "\n";
}

bool Scorecard::deserialize(std::string_view text) {
    auto parsed = json::parse(text);
    if (!parsed.ok()) return false;
    const json::Value root = std::move(*parsed.value);  // owned copy: no reference into a temporary
    if (!root.is_object()) return false;
    const json::Value* ver = root.find("version");
    if (!ver || ver->as_int() != kVersion) return false;
    const json::Value* fs = root.find("firstStartMs");
    const auto first = fs ? fs->as_int() : std::nullopt;
    if (!first || *first < 0) return false;

    std::optional<Baseline> base;
    if (const json::Value* b = root.find("baseline"); b && !b->is_null()) {
        const json::Value* at = b->find("capturedAt");
        const json::Value* mj = b->find("metrics");
        const auto at_i = at ? at->as_int() : std::nullopt;
        auto mm = mj ? metrics_from_json(*mj) : std::nullopt;
        if (!at_i || *at_i <= 0 || !mm) return false;
        base = Baseline{*at_i, *mm};
    }
    std::deque<Minute> mins;
    if (const json::Value* ma = root.find("minutes"); ma && ma->is_array()) {
        for (const json::Value& e : ma->as_array()) {
            if (!e.is_array() || e.size() != 2) continue;  // corrupt entry: skipped
            const auto idx = e.as_array()[0].as_int();
            const auto fl = e.as_array()[1].as_int();
            if (!idx || !fl || *fl <= 0 || *fl > 7) continue;
            if (!mins.empty() && *idx <= mins.back().index) continue;  // must be ascending
            mins.push_back({*idx, static_cast<std::uint8_t>(*fl)});
        }
        while (mins.size() > kMaxMinutes) mins.pop_front();
    }
    first_start_ = *first;
    baseline_ = std::move(base);
    minutes_ = std::move(mins);
    return true;
}

bool Scorecard::save(trace::FileIO& io, const std::string& path) const { return io.write_atomic(path, serialize()); }

trace::LoadReport Scorecard::load(trace::FileIO& io, const std::string& path) {
    trace::LoadReport rep;
    std::string text;
    if (!io.read_all(path, text, kMaxFileBytes)) return rep;
    rep.found = true;
    if (deserialize(text)) {
        rep.loaded = 1;
    } else {
        rep.corrupt = 1;
    }
    return rep;
}

}  // namespace opensweep::scorecard
