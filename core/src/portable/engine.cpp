#include "engine.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <utility>

namespace opensweep::engine {

namespace {

constexpr std::size_t kMaxConfigBytes = 64 * 1024;
constexpr std::size_t kMaxJournalBytes = 64u * 1024u * 1024u;
constexpr std::size_t kMaxHealKeys = 64;
constexpr PolicyKind kAllKinds[] = {PolicyKind::EcoQoS, PolicyKind::MemoryPriority, PolicyKind::PriorityClass,
                                    PolicyKind::CpuSets};

std::int64_t system_now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

bool contains(const std::vector<PolicyKind>& v, PolicyKind k) { return std::find(v.begin(), v.end(), k) != v.end(); }

double finite_or(double v, double def) { return std::isfinite(v) ? v : def; }

control::ControlConfig control_config(const EngineOptions& o, const Settings& s) {
    control::ControlConfig c = o.control;
    c.mode = s.mode;
    c.simulation = s.simulation;
    c.opted_in = s.opted_in;
    return c;
}

guard::GuardConfig guard_config(const EngineOptions& o) {
    guard::GuardConfig g = o.guard;
    g.logical_processors = o.topology.logical_count;
    return g;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
json::Value settings_to_json(const Settings& s) {
    json::Value v = json::Value::object();
    v.set("version", kSettingsVersion);
    v.set("mode", control::to_string(s.mode));
    v.set("simulation", s.simulation);
    json::Value kinds = json::Value::array();
    for (PolicyKind k : s.opted_in) kinds.push_back(telemetry::to_string(k));
    v.set("optIn", std::move(kinds));
    return v;
}

std::optional<Settings> settings_from_json(const json::Value& v, std::string& error) {
    if (!v.is_object()) {
        error = "config is not a JSON object";
        return std::nullopt;
    }
    for (const auto& [key, val] : v.as_object()) {
        if (key != "version" && key != "mode" && key != "simulation" && key != "optIn") {
            error = "unknown key in config";
            return std::nullopt;
        }
        (void)val;
    }
    const json::Value* ver = v.find("version");
    if (!ver || ver->as_int() != kSettingsVersion) {
        error = "unsupported config version";
        return std::nullopt;
    }
    Settings s;
    const json::Value* mode = v.find("mode");
    if (!mode || !mode->is_string() || !control::parse_mode(mode->as_string())) {
        error = "invalid mode";
        return std::nullopt;
    }
    s.mode = *control::parse_mode(mode->as_string());
    if (const json::Value* sim = v.find("simulation")) {
        if (!sim->is_bool()) {
            error = "simulation must be a boolean";
            return std::nullopt;
        }
        s.simulation = sim->as_bool();
    }
    if (const json::Value* opt = v.find("optIn")) {
        if (!opt->is_array() || opt->size() > 4) {
            error = "optIn must be an array of at most 4 policy kinds";
            return std::nullopt;
        }
        for (const auto& k : opt->as_array()) {
            const auto kind = k.is_string() ? control::parse_policy_kind(k.as_string()) : std::nullopt;
            if (!kind || contains(s.opted_in, *kind)) {
                error = "optIn contains an unknown or duplicate policy kind";
                return std::nullopt;
            }
            s.opted_in.push_back(*kind);
        }
    }
    return s;
}

std::optional<std::string> JournalFileAdapter::read(const std::string& path) {
    std::string out;
    if (!io_.read_all(path, out, kMaxJournalBytes)) return std::nullopt;
    return out;
}

std::string join_path(std::string_view dir, std::string_view file) {
    std::string out(dir);
    if (out.empty()) return std::string(file);
    const char sep = out.find('\\') != std::string::npos ? '\\' : '/';
    if (out.back() != '/' && out.back() != '\\') out.push_back(sep);
    out.append(file);
    return out;
}

// ---------------------------------------------------------------------------------------------
// OpenSweep-owned recovery actions are executed by the engine itself; everything else is
// delegated to the platform executor. Runs with the engine mutex held (from tick / approve /
// request_heal), so it uses the *_locked helpers only.
class Engine::OwnedExecutor final : public heal::Executor {
public:
    OwnedExecutor(Engine& e, heal::Executor& platform) : e_(e), platform_(platform) {}

    heal::ExecResult execute(heal::Action a, const std::string& target) override {
        switch (a) {
            case heal::Action::ClearInternalCache:
                e_.clear_caches_locked();
                return {true, "incident captures and derived caches cleared"};
            case heal::Action::RestoreOwnedPolicies: {
                const auto s = e_.control_.rollback_all(e_.now(), "heal_restore_owned_policies");
                return {s.failed == 0, "restored " + std::to_string(s.restored) + ", failed " +
                                           std::to_string(s.failed) + ", expired " + std::to_string(s.expired)};
            }
            default:
                try {
                    return platform_.execute(a, target);
                } catch (const std::exception&) {
                    return {false, "executor exception"};
                }
        }
    }

    bool verify(heal::Action a, const std::string& target) override {
        switch (a) {
            case heal::Action::ClearInternalCache: return e_.captures_.empty();
            case heal::Action::RestoreOwnedPolicies: return e_.control_.active().empty();
            default:
                try {
                    return platform_.verify(a, target);
                } catch (...) {
                    return false;
                }
        }
    }

private:
    Engine& e_;
    heal::Executor& platform_;
};

Engine::LoadedSettings Engine::load_settings(trace::FileIO& io, const std::string& path) {
    LoadedSettings out;
    std::string text;
    if (!io.read_all(path, text, kMaxConfigBytes)) return out;  // missing: defaults, not an error
    out.found = true;
    auto parsed = json::parse(text);
    if (!parsed.ok()) {
        out.valid = false;
        out.error = "config.json is not valid JSON";
        return out;
    }
    auto s = settings_from_json(*parsed.value, out.error);
    if (!s) {
        out.valid = false;
        return out;
    }
    out.settings = *s;
    return out;
}

Engine::Engine(telemetry::Actuator& actuator, heal::Executor& executor, trace::FileIO& io, EngineOptions options)
    : io_(io),
      options_(std::move(options)),
      jio_(io),
      cfg_(load_settings(io, join_path(options_.data_dir, kConfigFile))),
      guard_(guard_config(options_)),
      memory_(options_.memory),
      scheduler_(options_.scheduler),
      journal_(jio_, join_path(options_.data_dir, kJournalFile), options_.journal),
      control_(actuator, journal_, control_config(options_, cfg_.settings)) {
    owned_exec_ = std::make_unique<OwnedExecutor>(*this, executor);
    heal_ = std::make_unique<heal::SweepHeal>(journal_, *owned_exec_, options_.heal);
    mode_.store(control_.mode(), std::memory_order_relaxed);
}

Engine::~Engine() = default;

std::int64_t Engine::now() const { return options_.clock ? options_.clock() : system_now_ms(); }

void Engine::log(std::string_view m) const {
    if (options_.log) options_.log(m);
}

StartReport Engine::start() {
    std::lock_guard<std::mutex> lock(mu_);
    StartReport rep;
    rep.config_found = cfg_.found;
    rep.config_valid = cfg_.valid;
    rep.config_error = cfg_.error;
    if (!cfg_.valid) log("config.json is invalid (" + cfg_.error + "); using fail-safe defaults (monitor)");

    rep.trace = trace_.load(io_, data_path(kTraceFile));
    rep.events = trace_.load_events(io_, data_path(kEventsFile));
    rep.baselines = baselines_.load(io_, data_path(kBaselinesFile));
    rep.incidents = incidents_.load(io_, data_path(kIncidentsFile));
    rep.journal = journal_.load();

    const std::int64_t t = now();
    // Crash recovery: anything the journal says may still be applied is rolled back.
    rep.recovered = control_.recover(t);
    rep.heal_expired = heal_->recover(t);
    last_incident_size_ = incidents_.size();
    last_incident_open_ = incidents_.open_count();
    started_ = true;
    mode_.store(control_.mode(), std::memory_order_relaxed);

    log("engine started: mode=" + std::string(control::to_string(control_.mode())) +
        " trace rows=" + std::to_string(rep.trace.loaded) + " (corrupt " + std::to_string(rep.trace.corrupt) +
        ") events=" + std::to_string(rep.events.loaded) + " baselines=" + std::to_string(rep.baselines.loaded) +
        " incidents=" + std::to_string(rep.incidents.loaded) + " journal=" + std::to_string(rep.journal.loaded) +
        " (corrupt " + std::to_string(rep.journal.corrupt) + ") recovered: restored=" +
        std::to_string(rep.recovered.restored) + " failed=" + std::to_string(rep.recovered.failed) +
        " expired=" + std::to_string(rep.recovered.expired) + " heal_expired=" + std::to_string(rep.heal_expired));
    return rep;
}

std::vector<std::uint32_t> Engine::suspected_leak_pids_locked() const {
    std::vector<std::uint32_t> pids;
    incidents_.for_each_open([&](const incident::Incident& inc) {
        if (inc.detector != "memory_growth" || inc.title.rfind("Suspected", 0) != 0) return;
        for (const auto& c : inc.contributors)
            if (c.kind == "process" && c.pid != 0 && pids.size() < 64) pids.push_back(c.pid);
    });
    return pids;
}

void Engine::capture_incidents_locked(std::int64_t t) {
    for (const auto& id : guard_.last_opened()) {
        if (options_.max_captures == 0) break;
        IncidentCapture c;
        c.incident_id = id;
        c.captured_ms = t;
        c.rows = trace_.window(t - options_.capture_before_ms, t);
        while (captures_.size() >= options_.max_captures) captures_.pop_front();
        captures_.push_back(std::move(c));
    }
}

void Engine::propose_recoveries_locked(std::int64_t t) {
    for (const auto& id : guard_.last_opened()) {
        const incident::Incident* inc = incidents_.find_id(id);
        if (!inc || inc->state != incident::State::Open) continue;
        heal::RecoveryRequest req;
        req.trigger = "incident " + inc->id + " (" + inc->detector + ")";
        req.evidence.push_back(inc->summary.substr(0, 200));
        bool want = false;
        if (inc->detector == "service_crash_loop") {
            for (const auto& c : inc->contributors) {
                if (c.kind == "service" && !c.name.empty()) {
                    req.action = heal::Action::RestartService;
                    req.target = c.name;
                    want = true;
                    break;
                }
            }
        }
        // Explorer restarts / application termination are not proposed by the service: a session-0
        // process cannot do them safely in the user's session (the app offers them instead).
        if (!want) continue;
        const auto r = heal_->request(req, t);  // approval-required: becomes a pending approval
        if (r.status == heal::Status::PendingApproval) log("recovery proposed for approval: " + r.detail);
    }
}

void Engine::clear_caches_locked() { captures_.clear(); }

void Engine::record_config_event(std::int64_t t, std::string message) {
    EventRecord e;
    e.ts_ms = t;
    e.kind = telemetry::EventKind::OpenSweepConfig;
    e.provider = "OpenSweep";
    e.event_id = 1;
    e.level = 4;
    e.subject = "mode";
    if (message.size() > 200) message.resize(200);
    e.message = std::move(message);
    trace_.add_event(e);
    events_dirty_ = true;
    if (!trace_.persist_events(io_, data_path(kEventsFile))) log("events.jsonl could not be written");
    else events_dirty_ = false;
}

void Engine::persist_locked(std::int64_t t, bool force, bool incidents_changed, bool events_changed) {
    auto due = [&](std::int64_t& last, std::int64_t interval) {
        if (force) return true;
        if (last == 0) {
            last = t;
            return false;
        }
        return t - last >= interval || t < last;
    };
    if (due(last_trace_save_, options_.trace_persist_ms)) {
        if (!trace_.save(io_, data_path(kTraceFile))) log("trace.bin could not be written");
        last_trace_save_ = t;
    }
    if (due(last_baseline_save_, options_.baseline_persist_ms)) {
        if (!baselines_.save(io_, data_path(kBaselinesFile))) log("baselines.json could not be written");
        last_baseline_save_ = t;
    }
    const bool inc_due = due(last_incident_save_, options_.incident_persist_ms);
    if (force || incidents_changed || inc_due) {
        if (!incidents_.persist(io_, data_path(kIncidentsFile))) log("incidents.jsonl could not be written");
        last_incident_save_ = t;
    }
    if (force || events_changed || events_dirty_) {
        events_dirty_ = !trace_.persist_events(io_, data_path(kEventsFile));
        if (events_dirty_) log("events.jsonl could not be written");
    }
}

void Engine::tick(const Sample& s, std::span<const EventRecord> events) {
    std::lock_guard<std::mutex> lock(mu_);
    const auto t0 = std::chrono::steady_clock::now();
    const std::int64_t t = s.ts_ms;

    // OBSERVE -> bounded storage
    trace_.append(s);
    trace_.add_events(events);
    // ESTABLISH BASELINE
    baselines_.update(s);
    baselines_.observe_events(events);
    // DETECT (incidents with evidence + confidence)
    guard_.evaluate(s, events, baselines_, incidents_);
    capture_incidents_locked(t);
    propose_recoveries_locked(t);

    // CLASSIFY / RECOMMEND
    memory::MemoryInputs min;
    {
        const baseline::Baseline& hf = baselines_.get(trace::Metric::HardFaultsPerSec);
        min.baseline.ready = hf.ready() && std::isfinite(hf.median());
        min.baseline.hard_faults_median = finite_or(hf.median(), 0.0);
        min.baseline.hard_faults_mad = finite_or(hf.mad(), 0.0);
    }
    min.suspected_leak_pids = suspected_leak_pids_locked();
    const memory::MemoryAssessment ma = memory_.assess(s, min);

    sched::SchedulerInputs sin;
    sin.opensweep_ecoqos_pids = control_.active_pids(PolicyKind::EcoQoS);
    sin.opensweep_cpusets_pids = control_.active_pids(PolicyKind::CpuSets);
    sin.allow_cpu_sets = control_.mode() == Mode::Advanced;
    // No foreground responsiveness measurement is available to a session-0 service (it would need an
    // agent in the user's session); nullopt makes the scheduler require severe queue pressure.
    sin.fg_latency_ms = std::nullopt;
    const sched::SchedulerAssessment sa = scheduler_.assess(s, options_.topology, sin);

    std::vector<policy::Recommendation> recs;
    recs.reserve(ma.recommendations.size() + sa.recommendations.size());
    recs.insert(recs.end(), ma.recommendations.begin(), ma.recommendations.end());
    recs.insert(recs.end(), sa.recommendations.begin(), sa.recommendations.end());

    // DECIDE / APPLY / VERIFY (write-ahead journal inside the control engine)
    const policy::VerificationMetrics vm =
        policy::verification_metrics(s, options_.topology.logical_count, std::nullopt);
    control::TickResult tr = control_.step(t, recs, vm);
    for (const auto& v : tr.verifications) log("policy " + v.tx_id + (v.retained ? " retained: " : " rolled back: ") + v.reason);
    last_decisions_ = std::move(tr.decisions);
    last_no_action_ = tr.no_action_needed;
    last_reason_ = tr.no_action_needed ? tr.reason : (ma.reason.empty() ? sa.reason : sa.reason + "; " + ma.reason);
    mem_level_ = ma.level;
    mem_score_ = ma.score;
    contention_ = sa.contention;

    // HEAL
    heal_->tick(t);
    for (const auto& n : heal_->drain_notifications()) log("heal: " + n.kind + " " + n.detail);

    // PERSIST
    const bool inc_changed = !guard_.last_opened().empty() || incidents_.size() != last_incident_size_ ||
                             incidents_.open_count() != last_incident_open_;
    last_incident_size_ = incidents_.size();
    last_incident_open_ = incidents_.open_count();
    persist_locked(t, false, inc_changed, !events.empty());

    ++ticks_;
    last_tick_ms_ = t;
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
    tick_cost_us_ = us > 0 ? static_cast<std::uint64_t>(us) : 0;
    tick_cost_max_us_ = std::max(tick_cost_max_us_, tick_cost_us_);
}

control::RollbackSummary Engine::shutdown() {
    std::lock_guard<std::mutex> lock(mu_);
    const std::int64_t t = now();
    const control::RollbackSummary s = control_.shutdown(t);
    persist_locked(t, true, true, true);
    log("engine shutdown: restored=" + std::to_string(s.restored) + " failed=" + std::to_string(s.failed) +
        " expired=" + std::to_string(s.expired));
    return s;
}

heal::RequestResult Engine::request_heal(const heal::RecoveryRequest& req) {
    std::lock_guard<std::mutex> lock(mu_);
    const auto key = std::make_pair(req.action, req.target);
    if (std::find(heal_keys_.begin(), heal_keys_.end(), key) == heal_keys_.end()) {
        if (heal_keys_.size() >= kMaxHealKeys) heal_keys_.erase(heal_keys_.begin());
        heal_keys_.push_back(key);
    }
    return heal_->request(req, now());
}

// ---- read side ------------------------------------------------------------------------------
Status Engine::status() const {
    std::lock_guard<std::mutex> lock(mu_);
    Status st;
    st.mode = control_.mode();
    st.simulation = control_.simulating();
    st.simulation_flag = control_.config().simulation;
    st.opted_in = control_.config().opted_in;
    st.running = running_.load(std::memory_order_relaxed);
    st.ticks = ticks_;
    st.last_tick_ms = last_tick_ms_;
    st.tick_cost_us = tick_cost_us_;
    st.tick_cost_max_us = tick_cost_max_us_;
    const Sample* latest = trace_.latest();
    const guard::StabilityScore score = guard_.score(latest ? *latest : Sample{}, incidents_);
    st.stability_score = std::clamp(score.score, 0, 100);
    st.score_reasons = score.reasons;
    st.incidents_open = incidents_.open_count();
    st.policies_active = control_.active().size();
    st.recommendations = last_decisions_.size();
    st.pending_approvals = heal_->pending().size() + control_.pending_consents().size();
    st.breaker_open = heal_->any_breaker_open();
    st.no_action_needed = last_no_action_;
    st.decision_reason = last_reason_;
    st.memory_level = memory::to_string(mem_level_);
    st.memory_score = mem_score_;
    st.cpu_contention = contention_;
    return st;
}

std::optional<Sample> Engine::latest() const {
    std::lock_guard<std::mutex> lock(mu_);
    const Sample* s = trace_.latest();
    if (!s) return std::nullopt;
    return *s;
}

std::vector<trace::Point> Engine::history(trace::Metric m, trace::Range r) const {
    std::lock_guard<std::mutex> lock(mu_);
    return trace_.history(m, r);
}

json::Value Engine::baselines_summary() const {
    std::lock_guard<std::mutex> lock(mu_);
    return baselines_.summary_json();
}

std::vector<incident::Incident> Engine::incidents(std::size_t limit) const {
    std::lock_guard<std::mutex> lock(mu_);
    return incidents_.list(limit);
}

std::vector<EventRecord> Engine::events(std::size_t limit) const {
    std::lock_guard<std::mutex> lock(mu_);
    return trace_.events(limit);
}

std::vector<control::Decision> Engine::recommendations() const {
    std::lock_guard<std::mutex> lock(mu_);
    return last_decisions_;
}

std::vector<control::ActivePolicy> Engine::policies() const {
    std::lock_guard<std::mutex> lock(mu_);
    return control_.active();
}

std::vector<journal::Transaction> Engine::journal(std::size_t limit) const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<journal::Transaction> out;
    const auto recent = journal_.recent(limit);
    out.reserve(recent.size());
    for (const auto* t : recent) out.push_back(*t);
    return out;
}

HealView Engine::heal(std::size_t history_limit) const {
    std::lock_guard<std::mutex> lock(mu_);
    HealView v;
    v.open = heal_->any_breaker_open();
    for (const auto& [a, target] : heal_keys_) {
        const heal::BreakerStatus b = heal_->breaker(a, target);
        v.failures += b.failures;
        if (b.state == heal::BreakerState::Open) v.opened_at = std::max(v.opened_at, b.opened_at);
    }
    v.approvals = heal_->pending();
    v.consents = control_.pending_consents();
    // Recovery history: scan newest first, bounded.
    const auto recent = journal_.recent(journal_.size());
    for (const auto* t : recent) {
        if (v.history.size() >= history_limit) break;
        if (t->kind == "recovery") v.history.push_back(*t);
    }
    return v;
}

std::vector<IncidentCapture> Engine::captures() const {
    std::lock_guard<std::mutex> lock(mu_);
    return {captures_.begin(), captures_.end()};
}

std::size_t Engine::tracked_processes() const {
    std::lock_guard<std::mutex> lock(mu_);
    return guard_.tracked_processes() + scheduler_.tracked();
}

// ---- admin side -----------------------------------------------------------------------------
bool Engine::save_settings_locked(const Settings& s) {
    return io_.write_atomic(data_path(kConfigFile), json::serialize(settings_to_json(s)) + "\n");
}

ModeResult Engine::set_mode(Mode m, std::optional<bool> simulation, std::optional<std::vector<PolicyKind>> opt_in) {
    std::lock_guard<std::mutex> lock(mu_);
    ModeResult r;
    const std::int64_t t = now();
    const Mode before = control_.mode();
    if (opt_in) {
        for (PolicyKind k : kAllKinds) {
            const bool on = contains(*opt_in, k);
            if (on != contains(control_.config().opted_in, k)) r.rolled_back.add(control_.set_opt_in(k, on, t));
        }
    }
    if (simulation) control_.set_simulation(*simulation);
    const control::ModeChange mc = control_.set_mode(m, t);
    r.rolled_back.add(mc.rolled_back);
    r.ok = mc.ok;
    if (!mc.ok) r.error = "mode change refused: the journal cannot record it (fail-safe)";

    Settings next;
    next.mode = control_.mode();
    next.simulation = control_.config().simulation;
    next.opted_in = control_.config().opted_in;
    cfg_.settings = next;
    r.settings = next;
    r.persisted = save_settings_locked(next);
    if (!r.persisted) log("config.json could not be written; the mode will not survive a restart");
    mode_.store(next.mode, std::memory_order_relaxed);
    record_config_event(t, std::string("Mode ") + control::to_string(before) + " -> " +
                               control::to_string(next.mode) + (next.simulation ? " (simulation)" : "") +
                               (mc.ok ? "" : " (refused)"));
    log(std::string("mode set to ") + control::to_string(next.mode) + " (requested " + control::to_string(m) +
        "), rolled back " + std::to_string(r.rolled_back.restored));
    return r;
}

control::RollbackSummary Engine::rollback_all() {
    std::lock_guard<std::mutex> lock(mu_);
    return control_.rollback_all(now(), "admin_rollback_all");
}

bool Engine::disable_policy(const std::string& tx_id) {
    std::lock_guard<std::mutex> lock(mu_);
    return control_.disable_policy(tx_id, now());
}

ApproveResult Engine::approve(const std::string& id) {
    std::lock_guard<std::mutex> lock(mu_);
    ApproveResult r;
    const std::int64_t t = now();
    const auto& pending = heal_->pending();
    if (std::any_of(pending.begin(), pending.end(), [&](const heal::Approval& a) { return a.id == id; })) {
        const auto it = std::find_if(pending.begin(), pending.end(), [&](const heal::Approval& a) { return a.id == id; });
        const auto key = std::make_pair(it->action, it->target);
        if (std::find(heal_keys_.begin(), heal_keys_.end(), key) == heal_keys_.end()) {
            if (heal_keys_.size() >= kMaxHealKeys) heal_keys_.erase(heal_keys_.begin());
            heal_keys_.push_back(key);
        }
        const heal::RequestResult hr = heal_->approve(id, t, /*caller_is_admin=*/true);
        r.tx_id = hr.tx_id;
        r.detail = std::string(heal::to_string(hr.status)) + (hr.detail.empty() ? "" : ": " + hr.detail);
        r.kind = hr.tx_id.empty() ? ApproveResult::Kind::Refused : ApproveResult::Kind::Recovery;
        return r;
    }
    const auto& consents = control_.pending_consents();
    if (std::any_of(consents.begin(), consents.end(), [&](const control::PendingConsent& c) { return c.id == id; })) {
        if (control_.approve(id, "ipc-admin-" + std::to_string(t), t)) {
            const auto recent = journal_.recent(1);
            r.tx_id = recent.empty() ? std::string() : recent.front()->tx_id;
            r.kind = ApproveResult::Kind::Policy;
            r.detail = "consent recorded; applied on a later tick if every gate still passes";
        } else {
            r.kind = ApproveResult::Kind::Refused;
            r.detail = "consent refused (expired, or not in advanced mode)";
        }
        return r;
    }
    r.kind = ApproveResult::Kind::Unknown;
    r.detail = "unknown approval id";
    return r;
}

std::optional<std::string> Engine::write_data_file(std::string_view name, std::string_view data) {
    std::lock_guard<std::mutex> lock(mu_);
    const std::string path = data_path(name);
    if (!io_.write_atomic(path, data)) return std::nullopt;
    return path;
}

// ---------------------------------------------------------------------------------------------
Runner::Runner(Engine& engine, Collect collect, std::chrono::milliseconds interval, std::function<void()> after_tick)
    : engine_(engine), collect_(std::move(collect)), interval_(interval), after_tick_(std::move(after_tick)) {}

Runner::~Runner() { stop(); }

void Runner::start() {
    std::lock_guard<std::mutex> lock(mu_);
    if (thread_.joinable()) return;
    stopping_ = false;
    thread_ = std::thread([this] { run(); });
}

void Runner::stop() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stopping_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void Runner::run() {
    engine_.set_running(true);
    Sample sample;
    std::vector<EventRecord> events;
    auto next = std::chrono::steady_clock::now();
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(mu_);
            if (cv_.wait_until(lock, next, [this] { return stopping_; })) break;
        }
        next += interval_;
        const auto now = std::chrono::steady_clock::now();
        if (next < now) next = now + interval_;  // fell behind (e.g. sleep/resume): do not burst
        events.clear();
        try {
            if (collect_ && collect_(sample, events)) engine_.tick(sample, events);
            if (after_tick_) after_tick_();
        } catch (...) {
            // A failing tick must not kill the loop; the next tick retries.
        }
        if (events.capacity() > 4096) std::vector<EventRecord>().swap(events);
    }
    engine_.set_running(false);
}

}  // namespace opensweep::engine
