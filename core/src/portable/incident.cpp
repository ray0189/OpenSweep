#include "incident.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>

namespace opensweep::incident {

namespace {

constexpr std::size_t kMaxStr = 1024;
constexpr std::size_t kMaxList = 64;
constexpr std::size_t kMaxIncidentsFile = 16u * 1024u * 1024u;

json::Value num_or_null(double d) { return std::isfinite(d) ? json::Value(d) : json::Value(nullptr); }

json::Value str_array(const std::vector<std::string>& v) {
    json::Array a;
    a.reserve(v.size());
    for (const auto& s : v) a.emplace_back(s);
    return json::Value(std::move(a));
}

bool get_str(const json::Value& o, const char* key, std::string& out, bool required) {
    const auto* f = o.find(key);
    if (!f) return !required;
    if (!f->is_string() || f->as_string().size() > kMaxStr) return false;
    out = f->as_string();
    return true;
}

bool get_i64(const json::Value& o, const char* key, std::int64_t& out, bool required) {
    const auto* f = o.find(key);
    if (!f) return !required;
    const auto i = f->as_int();
    if (!i) return false;
    out = *i;
    return true;
}

// Finite number, or null -> NaN.
bool get_num(const json::Value& o, const char* key, double& out, bool required, bool allow_null) {
    const auto* f = o.find(key);
    if (!f) return !required;
    if (f->is_null() && allow_null) {
        out = std::numeric_limits<double>::quiet_NaN();
        return true;
    }
    if (!f->is_number() || !std::isfinite(f->as_double())) return false;
    out = f->as_double();
    return true;
}

bool get_str_array(const json::Value& o, const char* key, std::vector<std::string>& out) {
    const auto* f = o.find(key);
    if (!f) return true;
    if (!f->is_array() || f->as_array().size() > kMaxList) return false;
    out.clear();
    for (const auto& x : f->as_array()) {
        if (!x.is_string() || x.as_string().size() > kMaxStr) return false;
        out.push_back(x.as_string());
    }
    return true;
}

std::string lower(std::string_view s) {
    std::string r(s);
    for (char& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

bool claims_causation(std::string_view text) {
    const std::string t = lower(text);
    for (const char* bad : {"caused by", "is causing", "because of", "due to"}) {
        if (t.find(bad) != std::string::npos) return true;
    }
    return false;
}

Level max_level(Level a, Level b) { return static_cast<int>(a) >= static_cast<int>(b) ? a : b; }
Severity max_sev(Severity a, Severity b) { return static_cast<int>(a) >= static_cast<int>(b) ? a : b; }

}  // namespace

const char* to_string(Level l) noexcept {
    switch (l) {
        case Level::Low: return "low";
        case Level::Medium: return "medium";
        case Level::High: return "high";
    }
    return "low";
}

const char* to_string(Severity s) noexcept {
    switch (s) {
        case Severity::Info: return "info";
        case Severity::Warning: return "warning";
        case Severity::Error: return "error";
        case Severity::Critical: return "critical";
    }
    return "warning";
}

const char* to_string(State s) noexcept { return s == State::Open ? "open" : "resolved"; }

std::optional<Level> level_from_string(std::string_view s) noexcept {
    if (s == "low") return Level::Low;
    if (s == "medium") return Level::Medium;
    if (s == "high") return Level::High;
    return std::nullopt;
}

std::optional<Severity> severity_from_string(std::string_view s) noexcept {
    if (s == "info") return Severity::Info;
    if (s == "warning") return Severity::Warning;
    if (s == "error") return Severity::Error;
    if (s == "critical") return Severity::Critical;
    return std::nullopt;
}

std::optional<State> state_from_string(std::string_view s) noexcept {
    if (s == "open") return State::Open;
    if (s == "resolved") return State::Resolved;
    return std::nullopt;
}

// ---------------------------------------------------------------------------------------------
double z_strength(double z) noexcept {
    if (!std::isfinite(z)) return 0.0;
    return std::clamp((std::fabs(z) - 3.0) / 6.0, 0.0, 1.0);
}

double compute_confidence(const ConfidenceInputs& in) noexcept {
    const double strength = std::isfinite(in.strength) ? std::clamp(in.strength, 0.0, 1.0) : 0.0;
    double dur = 1.0;
    if (in.required_duration_ms > 0.0) {
        dur = std::isfinite(in.duration_ms) ? std::clamp(in.duration_ms / (2.0 * in.required_duration_ms), 0.0, 1.0)
                                            : 0.0;
    }
    const double sig = std::clamp(static_cast<double>(std::max(0, in.agreeing_signals)) / 3.0, 0.0, 1.0);
    double c = 0.15 + 0.40 * strength + 0.25 * dur + 0.20 * sig;
    if (!in.baseline_ready) c = std::min(0.6 * c, 0.39);
    return std::clamp(c, 0.05, 0.99);
}

Level confidence_level(double c) noexcept {
    if (!(c >= 0.4)) return Level::Low;
    if (c < 0.7) return Level::Medium;
    return Level::High;
}

bool wording_ok(const Incident& inc) {
    if (claims_causation(inc.title) || claims_causation(inc.summary)) return false;
    for (const auto& s : inc.symptoms)
        if (claims_causation(s)) return false;
    for (const auto& s : inc.recommended_actions)
        if (claims_causation(s)) return false;
    for (const auto& e : inc.evidence)
        if (claims_causation(e.note)) return false;
    return true;
}

// ---------------------------------------------------------------------------------------------
json::Value to_json(const Incident& inc) {
    json::Value v = json::Value::object();
    v.set("id", inc.id);
    v.set("key", inc.key);
    v.set("detector", inc.detector);
    v.set("title", inc.title);
    v.set("summary", inc.summary);
    v.set("severity", to_string(inc.severity));
    v.set("state", to_string(inc.state));
    v.set("confidence", std::isfinite(inc.confidence) ? std::clamp(inc.confidence, 0.0, 1.0) : 0.0);
    v.set("confidenceLevel", to_string(inc.confidence_level));
    v.set("risk", to_string(inc.risk));
    v.set("autoRecoveryAllowed", inc.auto_recovery_allowed);
    v.set("firstSeen", inc.first_seen_ms);
    v.set("lastSeen", inc.last_seen_ms);
    v.set("resolvedAt", inc.resolved_ms);
    v.set("occurrences", inc.occurrences);
    v.set("symptoms", str_array(inc.symptoms));
    json::Array ev;
    for (const auto& e : inc.evidence) {
        json::Value o = json::Value::object();
        o.set("metric", e.metric);
        o.set("unit", e.unit);
        o.set("observed", num_or_null(e.observed));
        o.set("baseline", num_or_null(e.baseline));
        o.set("robustZ", num_or_null(e.robust_z));
        o.set("windowFrom", e.window_from_ms);
        o.set("windowTo", e.window_to_ms);
        o.set("note", e.note);
        ev.push_back(std::move(o));
    }
    v.set("evidence", json::Value(std::move(ev)));
    json::Array co;
    for (const auto& c : inc.contributors) {
        json::Value o = json::Value::object();
        o.set("name", c.name);
        o.set("kind", c.kind);
        o.set("pid", c.pid);
        o.set("correlation", std::isfinite(c.correlation) ? c.correlation : 0.0);
        co.push_back(std::move(o));
    }
    v.set("contributors", json::Value(std::move(co)));
    v.set("recommendedActions", str_array(inc.recommended_actions));
    return v;
}

std::optional<Incident> from_json(const json::Value& v) {
    if (!v.is_object()) return std::nullopt;
    Incident inc;
    std::string sev, state, level, risk;
    if (!get_str(v, "id", inc.id, true) || inc.id.empty() || !get_str(v, "key", inc.key, false) ||
        !get_str(v, "detector", inc.detector, true) || !get_str(v, "title", inc.title, true) ||
        !get_str(v, "summary", inc.summary, false) || !get_str(v, "severity", sev, true) ||
        !get_str(v, "state", state, true) || !get_str(v, "confidenceLevel", level, true) ||
        !get_str(v, "risk", risk, true)) {
        return std::nullopt;
    }
    const auto s1 = severity_from_string(sev);
    const auto s2 = state_from_string(state);
    const auto s3 = level_from_string(level);
    const auto s4 = level_from_string(risk);
    if (!s1 || !s2 || !s3 || !s4) return std::nullopt;
    inc.severity = *s1;
    inc.state = *s2;
    inc.confidence_level = *s3;
    inc.risk = *s4;
    if (!get_num(v, "confidence", inc.confidence, true, false) || inc.confidence < 0.0 || inc.confidence > 1.0)
        return std::nullopt;
    const auto* ara = v.find("autoRecoveryAllowed");
    if (!ara || !ara->is_bool()) return std::nullopt;
    inc.auto_recovery_allowed = ara->as_bool();
    std::int64_t occ = 1;
    if (!get_i64(v, "firstSeen", inc.first_seen_ms, true) || !get_i64(v, "lastSeen", inc.last_seen_ms, true) ||
        !get_i64(v, "resolvedAt", inc.resolved_ms, false) || !get_i64(v, "occurrences", occ, false) || occ < 1 ||
        occ > 0xFFFFFFFFLL) {
        return std::nullopt;
    }
    inc.occurrences = static_cast<std::uint32_t>(occ);
    if (!get_str_array(v, "symptoms", inc.symptoms) ||
        !get_str_array(v, "recommendedActions", inc.recommended_actions)) {
        return std::nullopt;
    }
    if (const auto* ev = v.find("evidence")) {
        if (!ev->is_array() || ev->as_array().size() > kMaxList) return std::nullopt;
        for (const auto& o : ev->as_array()) {
            if (!o.is_object()) return std::nullopt;
            Evidence e;
            if (!get_str(o, "metric", e.metric, true) || !get_str(o, "unit", e.unit, false) ||
                !get_num(o, "observed", e.observed, true, true) || !get_num(o, "baseline", e.baseline, false, true) ||
                !get_num(o, "robustZ", e.robust_z, false, true) ||
                !get_i64(o, "windowFrom", e.window_from_ms, false) || !get_i64(o, "windowTo", e.window_to_ms, false) ||
                !get_str(o, "note", e.note, false)) {
                return std::nullopt;
            }
            inc.evidence.push_back(std::move(e));
        }
    }
    if (const auto* co = v.find("contributors")) {
        if (!co->is_array() || co->as_array().size() > kMaxList) return std::nullopt;
        for (const auto& o : co->as_array()) {
            if (!o.is_object()) return std::nullopt;
            Contributor c;
            std::int64_t pid = 0;
            if (!get_str(o, "name", c.name, true) || !get_str(o, "kind", c.kind, false) ||
                !get_i64(o, "pid", pid, false) || pid < 0 || pid > 0xFFFFFFFFLL ||
                !get_num(o, "correlation", c.correlation, false, false) || c.correlation < -1.0 ||
                c.correlation > 1.0) {
                return std::nullopt;
            }
            c.pid = static_cast<std::uint32_t>(pid);
            inc.contributors.push_back(std::move(c));
        }
    }
    return inc;
}

// ---------------------------------------------------------------------------------------------
IncidentStore::IncidentStore(std::size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) {
    items_.reserve(capacity_);
}

void IncidentStore::clear() {
    items_.clear();
    seq_ = 0;
    file_known_ = false;
    file_lines_ = 0;
}

std::string IncidentStore::next_id(std::int64_t now_ms) {
    ++seq_;
    return "inc-" + std::to_string(now_ms) + "-" + std::to_string(seq_);
}

IncidentStore::Entry* IncidentStore::newest_with_key(std::string_view key) {
    for (auto it = items_.rbegin(); it != items_.rend(); ++it) {
        if (it->inc.key == key) return &*it;
    }
    return nullptr;
}

void IncidentStore::evict_if_full() {
    while (items_.size() >= capacity_) {
        auto victim = std::find_if(items_.begin(), items_.end(),
                                   [](const Entry& e) { return e.inc.state == State::Resolved; });
        if (victim == items_.end()) victim = items_.begin();
        items_.erase(victim);
    }
}

IncidentStore::Upsert IncidentStore::open_or_merge(Incident c, std::int64_t now_ms, std::int64_t merge_window_ms,
                                                   bool count_repeat) {
    Upsert up;
    if (c.key.empty()) c.key = c.detector;
    if (!std::isfinite(c.confidence)) c.confidence = 0.0;
    c.confidence = std::clamp(c.confidence, 0.0, 1.0);
    c.confidence_level = confidence_level(c.confidence);
    if (Entry* e = newest_with_key(c.key)) {
        Incident& inc = e->inc;
        const bool mergeable =
            inc.state == State::Open || (now_ms >= inc.resolved_ms && now_ms - inc.resolved_ms <= merge_window_ms);
        if (mergeable) {
            up.reopened = inc.state == State::Resolved;
            inc.state = State::Open;
            inc.resolved_ms = 0;
            inc.last_seen_ms = std::max(inc.last_seen_ms, now_ms);
            if (up.reopened || count_repeat) ++inc.occurrences;
            inc.title = std::move(c.title);
            inc.summary = std::move(c.summary);
            inc.symptoms = std::move(c.symptoms);
            inc.evidence = std::move(c.evidence);
            inc.contributors = std::move(c.contributors);
            inc.recommended_actions = std::move(c.recommended_actions);
            inc.confidence = std::max(inc.confidence, c.confidence);
            inc.confidence_level = confidence_level(inc.confidence);
            inc.severity = max_sev(inc.severity, c.severity);
            inc.risk = max_level(inc.risk, c.risk);
            inc.auto_recovery_allowed = inc.auto_recovery_allowed && c.auto_recovery_allowed;
            e->dirty = true;
            up.incident = &inc;
            return up;
        }
    }
    evict_if_full();
    c.id = next_id(now_ms);
    c.state = State::Open;
    c.first_seen_ms = now_ms;
    c.last_seen_ms = now_ms;
    c.resolved_ms = 0;
    c.occurrences = 1;
    items_.push_back(Entry{std::move(c), true});
    up.incident = &items_.back().inc;
    up.created = true;
    return up;
}

bool IncidentStore::touch(std::string_view key, std::int64_t now_ms, double observed) {
    Entry* e = newest_with_key(key);
    if (!e || e->inc.state != State::Open) return false;
    if (now_ms > e->inc.last_seen_ms) e->inc.last_seen_ms = now_ms;
    if (std::isfinite(observed) && !e->inc.evidence.empty() && observed > e->inc.evidence.front().observed) {
        e->inc.evidence.front().observed = observed;
    }
    e->dirty = true;
    return true;
}

bool IncidentStore::resolve(std::string_view key, std::int64_t now_ms) {
    Entry* e = newest_with_key(key);
    if (!e || e->inc.state != State::Open) return false;
    e->inc.state = State::Resolved;
    e->inc.resolved_ms = std::max(now_ms, e->inc.last_seen_ms);
    e->dirty = true;
    return true;
}

const Incident* IncidentStore::find_open(std::string_view key) const {
    for (auto it = items_.rbegin(); it != items_.rend(); ++it) {
        if (it->inc.key == key) return it->inc.state == State::Open ? &it->inc : nullptr;
    }
    return nullptr;
}

const Incident* IncidentStore::find_id(std::string_view id) const {
    for (const auto& e : items_)
        if (e.inc.id == id) return &e.inc;
    return nullptr;
}

std::vector<Incident> IncidentStore::list(std::size_t limit) const {
    std::vector<const Incident*> ptrs;
    ptrs.reserve(items_.size());
    for (auto it = items_.rbegin(); it != items_.rend(); ++it) ptrs.push_back(&it->inc);
    std::stable_sort(ptrs.begin(), ptrs.end(),
                     [](const Incident* a, const Incident* b) { return a->last_seen_ms > b->last_seen_ms; });
    const std::size_t n = limit == 0 ? ptrs.size() : std::min(limit, ptrs.size());
    std::vector<Incident> out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) out.push_back(*ptrs[i]);
    return out;
}

void IncidentStore::for_each_open(const std::function<void(const Incident&)>& f) const {
    for (const auto& e : items_)
        if (e.inc.state == State::Open) f(e.inc);
}

std::size_t IncidentStore::open_count() const noexcept {
    std::size_t n = 0;
    for (const auto& e : items_)
        if (e.inc.state == State::Open) ++n;
    return n;
}

bool IncidentStore::persist(trace::FileIO& io, const std::string& path) {
    std::size_t dirty = 0;
    for (const auto& e : items_)
        if (e.dirty) ++dirty;
    if (!file_known_ || file_lines_ + dirty > 2 * capacity_) {
        std::string all;
        for (const auto& e : items_) all += trace::jsonl_seal(to_json(e.inc));
        if (!io.write_atomic(path, all)) return false;
        for (auto& e : items_) e.dirty = false;
        file_known_ = true;
        file_lines_ = items_.size();
        return true;
    }
    if (dirty == 0) return true;
    std::string lines;
    for (const auto& e : items_)
        if (e.dirty) lines += trace::jsonl_seal(to_json(e.inc));
    if (!io.append(path, lines)) return false;
    for (auto& e : items_) e.dirty = false;
    file_lines_ += dirty;
    return true;
}

trace::LoadReport IncidentStore::load(trace::FileIO& io, const std::string& path) {
    trace::LoadReport rep;
    std::string text;
    if (!io.read_all(path, text, kMaxIncidentsFile)) {
        file_known_ = false;
        return rep;
    }
    rep.found = true;
    items_.clear();
    std::size_t lines = 0;
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string_view line(text.data() + pos, nl - pos);
        pos = nl + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
        if (line.empty()) continue;
        ++lines;
        auto obj = trace::jsonl_open(line);
        std::optional<Incident> inc;
        if (obj) inc = from_json(*obj);
        if (!inc) {
            ++rep.corrupt;
            continue;
        }
        // Last line per id wins; keep the position of the latest version.
        auto same = std::find_if(items_.begin(), items_.end(), [&](const Entry& e) { return e.inc.id == inc->id; });
        if (same != items_.end()) items_.erase(same);
        // Restore the sequence counter from "inc-<ts>-<seq>".
        const auto dash = inc->id.rfind('-');
        if (dash != std::string::npos) {
            std::uint64_t s = 0;
            const char* b = inc->id.data() + dash + 1;
            const char* end = inc->id.data() + inc->id.size();
            if (std::from_chars(b, end, s).ec == std::errc() && s > seq_) seq_ = s;
        }
        if (inc->key.empty()) inc->key = inc->detector;
        items_.push_back(Entry{std::move(*inc), false});
        if (items_.size() > capacity_) items_.erase(items_.begin());
    }
    rep.loaded = items_.size();
    file_known_ = true;
    file_lines_ = lines;
    return rep;
}

}  // namespace opensweep::incident
