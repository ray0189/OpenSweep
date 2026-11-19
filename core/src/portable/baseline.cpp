#include "baseline.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "stats.hpp"

namespace opensweep::baseline {

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr int kVersion = 1;
constexpr std::size_t kMaxWindow = 100000;
constexpr std::size_t kMaxBaselinesFile = 4u * 1024u * 1024u;

std::int64_t floor_div(std::int64_t a, std::int64_t b) noexcept {
    std::int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}

// Median of `v` (destroys order). v must be non-empty.
double median_of(std::vector<double>& v) {
    const std::size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid), v.end());
    const double upper = v[mid];
    if (v.size() % 2 == 1) return upper;
    const double lower = *std::max_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid));
    return lower + (upper - lower) / 2.0;
}

// Scratch shared by all baselines (single-threaded engine loop). Grows to the largest window once.
std::vector<double>& scratch() {
    static thread_local std::vector<double> s;
    return s;
}

json::Value num_or_null(double d) { return std::isfinite(d) ? json::Value(d) : json::Value(nullptr); }

const char* context_name(Context c) {
    switch (c) {
        case Context::All: return "all";
        case Context::Active: return "active";
        case Context::Idle: return "idle";
    }
    return "all";
}

}  // namespace

// ---------------------------------------------------------------------------------------------
Baseline::Baseline(BaselineConfig cfg)
    : cfg_(cfg), window_(cfg.window == 0 ? 1 : cfg.window), median_(kNaN), mad_(kNaN), fast_(kNaN), slow_(kNaN) {
    if (cfg_.window == 0) cfg_.window = 1;
    if (!(cfg_.fast_alpha > 0.0 && cfg_.fast_alpha <= 1.0)) cfg_.fast_alpha = 0.3;
    if (!(cfg_.slow_alpha > 0.0 && cfg_.slow_alpha <= 1.0)) cfg_.slow_alpha = 0.02;
}

void Baseline::clear() {
    window_.clear();
    samples_ = 0;
    median_ = mad_ = fast_ = slow_ = kNaN;
    ewma_init_ = false;
}

void Baseline::add(double v) {
    if (!std::isfinite(v)) return;
    const auto f = static_cast<float>(v);
    if (!std::isfinite(f)) return;  // out of float range
    window_.push(f);
    ++samples_;
    if (!ewma_init_) {
        fast_ = slow_ = v;
        ewma_init_ = true;
    } else {
        fast_ += cfg_.fast_alpha * (v - fast_);
        slow_ += cfg_.slow_alpha * (v - slow_);
    }
    recompute();
}

void Baseline::recompute() {
    if (window_.empty()) {
        median_ = mad_ = kNaN;
        return;
    }
    auto& s = scratch();
    s.clear();
    s.reserve(window_.capacity());
    for (std::size_t i = 0; i < window_.size(); ++i) s.push_back(static_cast<double>(window_[i]));
    median_ = median_of(s);
    for (double& d : s) d = std::fabs(d - median_);
    mad_ = median_of(s);
}

std::optional<double> Baseline::z(double x, double min_mad) const noexcept {
    if (window_.empty() || !std::isfinite(x) || !std::isfinite(median_)) return std::nullopt;
    double spread = std::isfinite(mad_) ? mad_ : 0.0;
    if (std::isfinite(min_mad)) spread = std::max(spread, min_mad);
    if (spread <= 0.0) spread = 1e-9;
    return stats::robust_z(x, median_, spread);
}

json::Value Baseline::to_json() const {
    json::Value v = json::Value::object();
    v.set("samples", samples_);
    v.set("ewmaFast", num_or_null(fast_));
    v.set("ewmaSlow", num_or_null(slow_));
    json::Array w;
    w.reserve(window_.size());
    for (std::size_t i = 0; i < window_.size(); ++i) {
        // Round to float precision; float -> double is exact.
        w.emplace_back(static_cast<double>(window_[i]));
    }
    v.set("window", json::Value(std::move(w)));
    return v;
}

bool Baseline::from_json(const json::Value& v) {
    if (!v.is_object()) return false;
    const auto* samples = v.find("samples");
    const auto* window = v.find("window");
    const auto* fast = v.find("ewmaFast");
    const auto* slow = v.find("ewmaSlow");
    if (!samples || !window || !fast || !slow) return false;
    const auto n = samples->as_int();
    if (!n || *n < 0 || !window->is_array()) return false;
    const auto& arr = window->as_array();
    if (arr.size() > kMaxWindow || arr.size() > static_cast<std::uint64_t>(*n)) return false;
    auto finite_or_null = [](const json::Value& x, double& out) {
        if (x.is_null()) {
            out = kNaN;
            return true;
        }
        if (!x.is_number() || !std::isfinite(x.as_double())) return false;
        out = x.as_double();
        return true;
    };
    double f = kNaN, s = kNaN;
    if (!finite_or_null(*fast, f) || !finite_or_null(*slow, s)) return false;
    if (std::isfinite(f) != std::isfinite(s)) return false;
    if (*n > 0 && !std::isfinite(f)) return false;
    for (const auto& x : arr) {
        if (!x.is_number()) return false;
        const double d = x.as_double();
        if (!std::isfinite(d) || !std::isfinite(static_cast<float>(d))) return false;
    }
    // Valid: commit.
    window_.clear();
    const std::size_t skip = arr.size() > window_.capacity() ? arr.size() - window_.capacity() : 0;
    for (std::size_t i = skip; i < arr.size(); ++i) window_.push(static_cast<float>(arr[i].as_double()));
    samples_ = static_cast<std::uint64_t>(*n);
    fast_ = f;
    slow_ = s;
    ewma_init_ = std::isfinite(f);
    recompute();
    return true;
}

// ---------------------------------------------------------------------------------------------
BaselineSet::BaselineSet(BaselineSetConfig cfg) : cfg_(cfg), boot_(cfg.boot) {
    m_.reserve(kMetricCount);
    for (std::size_t i = 0; i < kMetricCount; ++i) {
        m_.push_back(PerMetric{Baseline(cfg_.overall), Baseline(cfg_.context), Baseline(cfg_.context)});
    }
}

void BaselineSet::clear() {
    for (auto& pm : m_) {
        pm.all.clear();
        pm.active.clear();
        pm.idle.clear();
    }
    boot_.clear();
    minute_ = -1;
    sum_.fill(0.0);
    n_.fill(0);
    active_samples_ = total_samples_ = 0;
}

const Baseline& BaselineSet::get(Metric m, Context c) const noexcept {
    const auto& pm = m_[std::min(trace::index(m), kMetricCount - 1)];
    return c == Context::Active ? pm.active : c == Context::Idle ? pm.idle : pm.all;
}

Baseline& BaselineSet::get_mut(Metric m, Context c) noexcept {
    auto& pm = m_[std::min(trace::index(m), kMetricCount - 1)];
    return c == Context::Active ? pm.active : c == Context::Idle ? pm.idle : pm.all;
}

void BaselineSet::commit_minute() {
    if (minute_ >= 0 && total_samples_ > 0) {
        const bool active = active_samples_ * 2 > total_samples_;
        for (std::size_t i = 0; i < kMetricCount; ++i) {
            if (n_[i] == 0) continue;
            const double mean = sum_[i] / n_[i];
            m_[i].all.add(mean);
            (active ? m_[i].active : m_[i].idle).add(mean);
        }
    }
    sum_.fill(0.0);
    n_.fill(0);
    active_samples_ = total_samples_ = 0;
}

void BaselineSet::update(const telemetry::Sample& s) {
    const std::int64_t mb = floor_div(s.ts_ms, trace::kTier1BucketMs);
    if (minute_ >= 0 && mb != minute_) commit_minute();
    minute_ = mb;
    ++total_samples_;
    if (s.cpu_percent >= kActiveCpuPercent) ++active_samples_;
    for (std::size_t i = 0; i < kMetricCount; ++i) {
        const double v = trace::metric_value(s, static_cast<Metric>(i));
        if (!std::isfinite(v)) continue;
        sum_[i] += v;
        ++n_[i];
    }
}

void BaselineSet::observe_events(std::span<const telemetry::EventRecord> events) {
    for (const auto& e : events) {
        if (e.kind == telemetry::EventKind::BootPerformance && std::isfinite(e.value) && e.value > 0.0) {
            boot_.add(e.value);
        }
    }
}

json::Value BaselineSet::summary_json() const {
    json::Value metrics = json::Value::object();
    for (std::size_t i = 0; i < kMetricCount; ++i) {
        const auto& b = m_[i].all;
        json::Value e = json::Value::object();
        e.set("median", num_or_null(b.median()));
        e.set("mad", num_or_null(b.mad()));
        e.set("ewma", num_or_null(b.ewma_slow()));
        e.set("samples", b.samples());
        e.set("ready", b.ready());
        metrics.set(trace::metrics()[i].name, std::move(e));
    }
    json::Value boot = json::Value::object();
    boot.set("median", num_or_null(boot_.median()));
    boot.set("mad", num_or_null(boot_.mad()));
    boot.set("ewma", num_or_null(boot_.ewma_slow()));
    boot.set("samples", boot_.samples());
    boot.set("ready", boot_.ready());
    metrics.set("bootDurationMs", std::move(boot));
    json::Value out = json::Value::object();
    out.set("metrics", std::move(metrics));
    return out;
}

json::Value BaselineSet::to_json() const {
    json::Value out = json::Value::object();
    out.set("version", kVersion);
    json::Value metrics = json::Value::object();
    for (std::size_t i = 0; i < kMetricCount; ++i) {
        json::Value e = json::Value::object();
        e.set(context_name(Context::All), m_[i].all.to_json());
        e.set(context_name(Context::Active), m_[i].active.to_json());
        e.set(context_name(Context::Idle), m_[i].idle.to_json());
        metrics.set(trace::metrics()[i].name, std::move(e));
    }
    out.set("metrics", std::move(metrics));
    out.set("boot", boot_.to_json());
    return out;
}

std::size_t BaselineSet::from_json(const json::Value& v) {
    clear();
    const auto* ver = v.find("version");
    const auto* metrics = v.find("metrics");
    if (!v.is_object() || !ver || ver->as_int() != std::optional<std::int64_t>(kVersion) || !metrics ||
        !metrics->is_object()) {
        return 1;
    }
    std::size_t rejected = 0;
    for (const auto& [name, entry] : metrics->as_object()) {
        const auto m = trace::metric_from_name(name);
        if (!m) continue;  // metric removed in this version
        if (!entry.is_object()) {
            ++rejected;
            continue;
        }
        for (Context c : {Context::All, Context::Active, Context::Idle}) {
            const auto* b = entry.find(context_name(c));
            if (!b) continue;
            if (!get_mut(*m, c).from_json(*b)) ++rejected;
        }
    }
    if (const auto* b = v.find("boot")) {
        if (!boot_.from_json(*b)) ++rejected;
    }
    return rejected;
}

bool BaselineSet::save(trace::FileIO& io, const std::string& path) const {
    return io.write_atomic(path, json::serialize(to_json()));
}

trace::LoadReport BaselineSet::load(trace::FileIO& io, const std::string& path) {
    trace::LoadReport rep;
    std::string text;
    if (!io.read_all(path, text, kMaxBaselinesFile)) return rep;
    rep.found = true;
    json::ParseOptions opts;
    opts.max_size = kMaxBaselinesFile;
    const auto r = json::parse(text, opts);
    if (!r.ok()) {
        clear();
        rep.corrupt = 1;
        return rep;
    }
    rep.corrupt = from_json(*r.value);
    for (std::size_t i = 0; i < kMetricCount; ++i) {
        if (m_[i].all.samples() > 0) ++rep.loaded;
    }
    return rep;
}

}  // namespace opensweep::baseline
