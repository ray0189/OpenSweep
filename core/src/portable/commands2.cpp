#include "commands2.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <optional>
#include <string_view>

namespace opensweep::commands2 {

using protocol::Capability;
using protocol::ClientContext;
using protocol::CommandError;
using protocol::ErrorCode;
using protocol::Request;

namespace {

[[noreturn]] void bad_args(const std::string& m) { throw CommandError(ErrorCode::BadArgs, m); }

// Strict argument check: only the listed keys may be present.
void allow_only(const Request& req, std::initializer_list<std::string_view> keys) {
    for (const auto& [k, v] : req.args.as_object()) {
        (void)v;
        if (std::find(keys.begin(), keys.end(), std::string_view(k)) == keys.end())
            bad_args("unknown argument '" + (k.size() <= 32 ? k : k.substr(0, 32)) + "'");
    }
}

std::size_t limit_arg(const Request& req, std::size_t def, std::size_t max) {
    const json::Value* v = req.args.find("limit");
    if (!v) return def;
    const auto i = v->as_int();
    if (!i || *i < 1 || static_cast<std::uint64_t>(*i) > max)
        bad_args("limit must be an integer between 1 and " + std::to_string(max));
    return static_cast<std::size_t>(*i);
}

bool id_char_ok(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' ||
           c == ':' || c == '-';
}

// Transaction / approval ids: a short token [A-Za-z0-9_.:-]{1,64} or a positive integer.
std::string id_arg(const Request& req, const char* key) {
    const json::Value* v = req.args.find(key);
    if (!v) bad_args(std::string(key) + " is required");
    if (const auto i = v->as_int(); i && *i > 0) return std::to_string(*i);
    if (!v->is_string()) bad_args(std::string(key) + " must be a string or a positive integer");
    const std::string& s = v->as_string();
    if (s.empty() || s.size() > 64 || !std::all_of(s.begin(), s.end(), id_char_ok))
        bad_args(std::string(key) + " has an invalid format");
    return s;
}

const char* risk_word(int risk) { return risk <= 2 ? "low" : (risk == 3 ? "medium" : "high"); }

json::Value target_json(const telemetry::PolicyTarget& t) {
    json::Value o = json::Value::object();
    o.set("pid", t.pid);
    o.set("name", t.name);
    o.set("startMs", t.process_start_ms);
    return o;
}

json::Value strings(const std::vector<std::string>& v) {
    json::Value a = json::Value::array();
    for (const auto& s : v) a.push_back(s);
    return a;
}

double num(double v) { return std::isfinite(v) ? v : 0.0; }

// Appends serialised items until the budget is reached (responses must stay below 1 MiB).
template <typename T, typename F>
json::Value fit_list(const std::vector<T>& items, F&& to_json, std::size_t budget = kListBudgetBytes) {
    json::Value arr = json::Value::array();
    std::size_t used = 0;
    for (const auto& it : items) {
        json::Value v = to_json(it);
        used += json::serialize(v).size() + 1;
        if (used > budget) break;
        arr.push_back(std::move(v));
    }
    return arr;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
json::Value process_to_json(const telemetry::ProcessSample& p) {
    json::Value o = json::Value::object();
    o.set("pid", p.pid);
    o.set("parentPid", p.parent_pid);
    o.set("name", p.name);
    o.set("cpuPercent", num(p.cpu_percent));
    o.set("privateBytes", p.private_bytes);
    o.set("workingSet", p.working_set);
    o.set("hardFaultsPerSec", p.hard_faults_per_sec);
    o.set("pageFaultsPerSec", num(p.page_faults_per_sec));
    o.set("handles", p.handles);
    o.set("threads", p.threads);
    o.set("sessionId", p.session_id);
    o.set("priorityClass", p.priority_class);
    o.set("foreground", p.foreground);
    o.set("isSystemCritical", p.is_system_critical);
    o.set("isSecuritySoftware", p.is_security_software);
    o.set("isService", p.is_service);
    o.set("hasAudio", p.has_audio);
    o.set("signedByMicrosoft", p.signed_by_microsoft);
    o.set("startMs", p.start_ms);
    return o;
}

json::Value sample_to_json(const telemetry::Sample& s) {
    json::Value o = json::Value::object();
    o.set("tsMs", s.ts_ms);
    o.set("cpuPercent", num(s.cpu_percent));
    o.set("cpuQueueLength", num(s.cpu_queue_length));
    o.set("contextSwitchesPerSec", num(s.context_switches_per_sec));
    o.set("dpcPercent", num(s.dpc_percent));
    o.set("interruptPercent", num(s.interrupt_percent));
    o.set("memTotal", s.mem_total);
    o.set("memAvail", s.mem_avail);
    o.set("memLoadPercent", s.mem_load_percent);
    o.set("commitTotal", s.commit_total);
    o.set("commitLimit", s.commit_limit);
    o.set("systemCache", s.system_cache);
    o.set("kernelPaged", s.kernel_paged);
    o.set("kernelNonpaged", s.kernel_nonpaged);
    o.set("compressedBytes", s.compressed_bytes);
    o.set("hardFaultsPerSec", num(s.hard_faults_per_sec));
    o.set("pageFaultsPerSec", num(s.page_faults_per_sec));
    o.set("diskReadLatencyMs", num(s.disk_read_latency_ms));
    o.set("diskWriteLatencyMs", num(s.disk_write_latency_ms));
    o.set("diskQueueLength", num(s.disk_queue_length));
    o.set("diskBytesPerSec", num(s.disk_bytes_per_sec));
    o.set("cpuFreqPercent", num(s.cpu_freq_percent));
    o.set("processes", s.processes);
    o.set("threads", s.threads);
    o.set("handles", s.handles);
    o.set("foregroundPid", s.foreground_pid);
    json::Value top = json::Value::array();
    for (const auto& p : s.top) top.push_back(process_to_json(p));
    o.set("top", std::move(top));
    return o;
}

json::Value event_to_json(const telemetry::EventRecord& e) {
    json::Value o = json::Value::object();
    o.set("tsMs", e.ts_ms);
    o.set("kind", telemetry::to_string(e.kind));
    o.set("provider", e.provider);
    o.set("eventId", e.event_id);
    o.set("level", static_cast<std::uint32_t>(std::clamp<std::uint8_t>(e.level, 1, 4)));
    o.set("subject", e.subject);
    o.set("message", e.message.size() <= 200 ? e.message : e.message.substr(0, 200));
    o.set("value", num(e.value));
    return o;
}

json::Value status_to_json(const engine::Status& st) {
    json::Value o = json::Value::object();
    o.set("mode", control::to_string(st.mode));
    o.set("simulation", st.simulation);
    o.set("simulationFlag", st.simulation_flag);
    json::Value opt = json::Value::array();
    for (auto k : st.opted_in) opt.push_back(telemetry::to_string(k));
    o.set("optIn", std::move(opt));
    json::Value eng = json::Value::object();
    eng.set("running", st.running);
    eng.set("ticks", st.ticks);
    eng.set("lastTickMs", st.last_tick_ms);
    eng.set("tickCostUs", st.tick_cost_us);
    eng.set("tickCostMaxUs", st.tick_cost_max_us);
    o.set("engine", std::move(eng));
    o.set("stabilityScore", st.stability_score);
    o.set("scoreReasons", strings(st.score_reasons));
    json::Value counts = json::Value::object();
    counts.set("incidentsOpen", st.incidents_open);
    counts.set("policiesActive", st.policies_active);
    counts.set("recommendations", st.recommendations);
    counts.set("pendingApprovals", st.pending_approvals);
    o.set("counts", std::move(counts));
    json::Value heal = json::Value::object();
    heal.set("breakerOpen", st.breaker_open);
    o.set("heal", std::move(heal));
    json::Value decision = json::Value::object();
    decision.set("noActionNeeded", st.no_action_needed);
    decision.set("reason", st.decision_reason);
    o.set("decision", std::move(decision));
    json::Value mem = json::Value::object();
    mem.set("level", st.memory_level);
    mem.set("score", num(st.memory_score));
    o.set("memory", std::move(mem));
    json::Value cpu = json::Value::object();
    cpu.set("contention", st.cpu_contention);
    o.set("cpu", std::move(cpu));
    return o;
}

json::Value decision_to_json(const control::Decision& d) {
    const auto& r = d.rec;
    json::Value o = json::Value::object();
    o.set("id", r.id);
    o.set("area", policy::to_string(r.area));
    o.set("target", target_json(r.target));
    o.set("workload", telemetry::to_string(r.workload));
    o.set("action", telemetry::to_string(r.kind));
    o.set("value", r.value);
    o.set("reason", r.reason);
    o.set("evidence", strings(r.evidence));
    o.set("expectedEffect", r.expected_effect);
    o.set("risk", r.risk);
    o.set("cost", num(d.cost));
    o.set("wouldApply", d.would_apply);
    if (!d.blocked_by.empty()) o.set("blockedBy", d.blocked_by);
    o.set("outcome", control::to_string(d.outcome));
    if (!d.tx_id.empty()) o.set("txId", d.tx_id);
    o.set("beneficiaryPid", r.beneficiary_pid);
    return o;
}

json::Value policy_to_json(const control::ActivePolicy& p) {
    json::Value o = json::Value::object();
    o.set("txId", p.tx_id);
    o.set("kind", telemetry::to_string(p.rec.kind));
    o.set("area", policy::to_string(p.rec.area));
    o.set("target", target_json(p.rec.target));
    o.set("value", p.rec.value);
    o.set("original", p.original);
    o.set("appliedAt", p.applied_at);
    o.set("verifyUntil", p.verify_until);
    o.set("retainUntil", p.retain_until);
    o.set("state", p.verifying ? "verifying" : "retained");
    o.set("reason", p.rec.reason);
    return o;
}

json::Value approval_to_json(const heal::Approval& a) {
    const heal::CatalogueEntry* e = heal::find(a.action);
    json::Value o = json::Value::object();
    o.set("id", a.id);
    o.set("kind", "recovery");
    o.set("action", e ? std::string(e->id) : std::string("unknown"));
    o.set("description", e ? std::string(e->description) : std::string());
    o.set("target", a.target);
    o.set("trigger", a.trigger);
    o.set("evidence", strings(a.evidence));
    o.set("risk", heal::to_string(a.risk));
    o.set("auth", heal::to_string(a.auth));
    o.set("createdMs", a.created_ms);
    o.set("expiresMs", a.expires_ms);
    return o;
}

json::Value consent_to_json(const control::PendingConsent& c) {
    json::Value o = json::Value::object();
    o.set("id", c.id);
    o.set("kind", "policy");
    o.set("action", telemetry::to_string(c.rec.kind));
    o.set("description", c.rec.expected_effect);
    o.set("target", c.rec.target.name);
    o.set("targetPid", c.rec.target.pid);
    o.set("value", c.rec.value);
    o.set("trigger", c.rec.reason);
    o.set("evidence", strings(c.rec.evidence));
    o.set("risk", risk_word(c.rec.risk));
    o.set("auth", "admin");
    o.set("createdMs", c.created_ms);
    o.set("expiresMs", c.expires_ms);
    return o;
}

json::Value heal_to_json(const engine::HealView& h) {
    json::Value o = json::Value::object();
    json::Value b = json::Value::object();
    b.set("open", h.open);
    b.set("failures", h.failures);
    b.set("openedAt", h.opened_at);
    o.set("breaker", std::move(b));
    json::Value pending = json::Value::array();
    for (const auto& a : h.approvals) pending.push_back(approval_to_json(a));
    for (const auto& c : h.consents) pending.push_back(consent_to_json(c));
    o.set("pending", std::move(pending));
    o.set("history", fit_list(h.history, [](const journal::Transaction& t) { return journal::to_json(t); },
                              kListBudgetBytes / 2));
    return o;
}

json::Value baselines_to_json(const json::Value& summary) {
    json::Value metrics = json::Value::object();
    if (const json::Value* m = summary.find("metrics"); m && m->is_object()) {
        for (const auto& [name, b] : m->as_object()) {
            const json::Value* med = b.find("median");
            const json::Value* mad = b.find("mad");
            const json::Value* ewma = b.find("ewma");
            // A metric without any committed minute yet has no median: it is omitted, not reported as 0.
            if (!med || !med->is_number() || !mad || !mad->is_number() || !ewma || !ewma->is_number()) continue;
            metrics.set(name, b);
        }
    }
    json::Value o = json::Value::object();
    o.set("metrics", std::move(metrics));
    return o;
}

json::Value topology_to_json(const commands::TopologyInfo& t, const std::vector<CpuSetInfo>& sets) {
    json::Value o = commands::build_topology(t);
    json::Value a = json::Value::array();
    for (const auto& c : sets) {
        json::Value s = json::Value::object();
        s.set("id", c.id);
        s.set("group", c.group);
        s.set("core", c.core);
        s.set("efficiencyClass", c.efficiency_class);
        s.set("numa", c.numa);
        a.push_back(std::move(s));
    }
    o.set("cpuSets", std::move(a));
    return o;
}

json::Value build_diagnostics(engine::Engine& engine, const Platform& platform) {
    json::Value d = json::Value::object();
    d.set("format", "opensweep-core-diagnostics");
    d.set("formatVersion", 1);
    d.set("generatedAt", commands::format_iso8601_utc(commands::now_unix_ms()));
    json::Value svc = json::Value::object();
    svc.set("service", commands::kServiceName);
    svc.set("version", commands::kVersion);
    svc.set("protocol", protocol::kProtocolVersion);
    if (platform.service) svc.set("startedAt", commands::format_iso8601_utc(platform.service->started_at_ms));
    d.set("service", std::move(svc));
    d.set("status", status_to_json(engine.status()));
    if (auto s = engine.latest()) d.set("sample", sample_to_json(*s));
    d.set("baselines", baselines_to_json(engine.baselines_summary()));
    const auto incs = engine.incidents(kMaxIncidents);
    d.set("incidents", fit_list(incs, [](const incident::Incident& i) { return incident::to_json(i); }, 4u << 20));
    const auto evs = engine.events(500);
    d.set("events", fit_list(evs, [](const telemetry::EventRecord& e) { return event_to_json(e); }, 2u << 20));
    const auto decs = engine.recommendations();
    d.set("recommendations", fit_list(decs, [](const control::Decision& x) { return decision_to_json(x); }));
    const auto pols = engine.policies();
    d.set("policies", fit_list(pols, [](const control::ActivePolicy& p) { return policy_to_json(p); }));
    const auto jr = engine.journal(kMaxJournal);
    d.set("journal", fit_list(jr, [](const journal::Transaction& t) { return journal::to_json(t); }, 4u << 20));
    d.set("heal", heal_to_json(engine.heal(kHealHistory)));
    json::Value caps = json::Value::array();
    for (const auto& c : engine.captures()) {
        json::Value cj = json::Value::object();
        cj.set("incidentId", c.incident_id);
        cj.set("capturedMs", c.captured_ms);
        json::Value names = json::Value::array();
        for (const auto& mi : trace::metrics()) names.push_back(mi.name);
        cj.set("metrics", std::move(names));
        json::Value rows = json::Value::array();
        for (const auto& r : c.rows) {
            json::Value row = json::Value::array();
            row.push_back(r.ts_ms);
            for (float v : r.v) row.push_back(std::isfinite(v) ? json::Value(static_cast<double>(v)) : json::Value());
            rows.push_back(std::move(row));
        }
        cj.set("rows", std::move(rows));
        caps.push_back(std::move(cj));
    }
    d.set("incidentWindows", std::move(caps));
    if (platform.provider) {
        commands::TopologyInfo t;
        if (platform.provider->topology(t)) d.set("topology", topology_to_json(t, platform.cpu_sets));
    }
    if (platform.diagnostics_extra) d.set("platform", platform.diagnostics_extra());
    return d;
}

commands::ModeProvider engine_mode_provider(const engine::Engine& engine) {
    return [&engine]() { return engine.mode_name(); };
}

// ---------------------------------------------------------------------------------------------
void register_engine_commands(protocol::Dispatcher& d, engine::Engine& engine, const Platform& platform) {
    engine::Engine* e = &engine;
    const Platform* pf = &platform;

    // ---- read ---------------------------------------------------------------------------------
    d.add("status", Capability::Read, [e](const Request& req, const ClientContext&) {
        allow_only(req, {});
        return status_to_json(e->status());
    });
    d.add("sample", Capability::Read, [e](const Request& req, const ClientContext&) {
        allow_only(req, {});
        auto s = e->latest();
        if (!s) throw CommandError(ErrorCode::Internal, "no sample has been collected yet");
        return sample_to_json(*s);
    });
    d.add("history", Capability::Read, [e](const Request& req, const ClientContext&) {
        allow_only(req, {"metric", "range"});
        const json::Value* m = req.args.find("metric");
        const json::Value* r = req.args.find("range");
        if (!m || !m->is_string()) bad_args("metric is required (string)");
        if (!r || !r->is_string()) bad_args("range is required: \"30m\" | \"24h\" | \"14d\"");
        const auto metric = trace::metric_from_name(m->as_string());
        if (!metric) bad_args("unknown metric");
        const auto range = trace::range_from_string(r->as_string());
        if (!range) bad_args("range must be \"30m\", \"24h\" or \"14d\"");
        const auto points = e->history(*metric, *range);
        json::Value pts = json::Value::array();
        for (const auto& p : points) {
            if (!std::isfinite(p.value)) continue;
            json::Value pair = json::Value::array();
            pair.push_back(p.ts_ms);
            pair.push_back(p.value);
            pts.push_back(std::move(pair));
        }
        json::Value o = json::Value::object();
        o.set("metric", trace::info(*metric).name);
        o.set("unit", trace::info(*metric).unit);
        o.set("range", trace::to_string(*range));
        o.set("points", std::move(pts));
        return o;
    });
    d.add("baselines", Capability::Read, [e](const Request& req, const ClientContext&) {
        allow_only(req, {});
        return baselines_to_json(e->baselines_summary());
    });
    d.add("incidents", Capability::Read, [e](const Request& req, const ClientContext&) {
        allow_only(req, {"limit"});
        const auto items = e->incidents(limit_arg(req, kDefaultIncidents, kMaxIncidents));
        json::Value o = json::Value::object();
        o.set("incidents", fit_list(items, [](const incident::Incident& i) { return incident::to_json(i); }));
        return o;
    });
    d.add("events", Capability::Read, [e](const Request& req, const ClientContext&) {
        allow_only(req, {"limit"});
        const auto items = e->events(limit_arg(req, kDefaultEvents, kMaxEvents));
        json::Value o = json::Value::object();
        o.set("events", fit_list(items, [](const telemetry::EventRecord& x) { return event_to_json(x); }));
        return o;
    });
    d.add("recommendations", Capability::Read, [e](const Request& req, const ClientContext&) {
        allow_only(req, {});
        const auto items = e->recommendations();
        json::Value o = json::Value::object();
        o.set("items", fit_list(items, [](const control::Decision& x) { return decision_to_json(x); }));
        return o;
    });
    d.add("policies", Capability::Read, [e](const Request& req, const ClientContext&) {
        allow_only(req, {});
        const auto items = e->policies();
        json::Value o = json::Value::object();
        o.set("active", fit_list(items, [](const control::ActivePolicy& p) { return policy_to_json(p); }));
        return o;
    });
    d.add("journal", Capability::Read, [e](const Request& req, const ClientContext&) {
        allow_only(req, {"limit"});
        const auto items = e->journal(limit_arg(req, kDefaultJournal, kMaxJournal));
        json::Value o = json::Value::object();
        o.set("entries", fit_list(items, [](const journal::Transaction& t) { return journal::to_json(t); }));
        return o;
    });
    d.add("heal", Capability::Read, [e](const Request& req, const ClientContext&) {
        allow_only(req, {});
        return heal_to_json(e->heal(kHealHistory));
    });
    d.add("topology", Capability::Read, [pf](const Request& req, const ClientContext&) {
        allow_only(req, {});
        commands::TopologyInfo t;
        if (!pf->provider || !pf->provider->topology(t)) throw CommandError(ErrorCode::Internal, "topology unavailable");
        return topology_to_json(t, pf->cpu_sets);
    });
    d.add("reportForeground", Capability::Read, [pf](const Request& req, const ClientContext& ctx) {
        allow_only(req, {"pid"});
        const json::Value* v = req.args.find("pid");
        const auto pid = v ? v->as_int() : std::nullopt;
        if (!pid || *pid <= 4 || *pid > 0xFFFFFFFFLL) bad_args("pid must be an integer process id > 4");
        if (ctx.session_id == protocol::kUnknownSession)
            throw CommandError(ErrorCode::Forbidden, "the caller's session could not be determined");
        if (!pf->report_foreground) throw CommandError(ErrorCode::Internal, "foreground hints are not supported");
        switch (pf->report_foreground(static_cast<std::uint32_t>(*pid), ctx.session_id)) {
            case ForegroundVerdict::Accepted:
            case ForegroundVerdict::Ignored: return json::Value::object();
            case ForegroundVerdict::NoSuchProcess: bad_args("no such process");
            case ForegroundVerdict::WrongSession: bad_args("the process is not in the caller's session");
            case ForegroundVerdict::Unavailable: break;
        }
        throw CommandError(ErrorCode::Internal, "foreground hint could not be checked");
    });

    // ---- admin --------------------------------------------------------------------------------
    d.add("setMode", Capability::Admin, [e](const Request& req, const ClientContext&) {
        allow_only(req, {"mode", "simulation", "optIn"});
        const json::Value* m = req.args.find("mode");
        if (!m || !m->is_string()) bad_args("mode is required: \"monitor\" | \"balanced\" | \"advanced\"");
        const auto mode = control::parse_mode(m->as_string());
        if (!mode) bad_args("mode must be \"monitor\", \"balanced\" or \"advanced\"");
        std::optional<bool> sim;
        if (const json::Value* s = req.args.find("simulation")) {
            if (!s->is_bool()) bad_args("simulation must be a boolean");
            sim = s->as_bool();
        }
        std::optional<std::vector<telemetry::PolicyKind>> opt;
        if (const json::Value* o = req.args.find("optIn")) {
            if (!o->is_array() || o->size() > 4) bad_args("optIn must be an array of policy kinds");
            opt.emplace();
            for (const auto& k : o->as_array()) {
                const auto kind = k.is_string() ? control::parse_policy_kind(k.as_string()) : std::nullopt;
                if (!kind || std::find(opt->begin(), opt->end(), *kind) != opt->end())
                    bad_args("optIn: unknown or duplicate policy kind (ecoqos | memory_priority | priority_class | cpu_sets)");
                opt->push_back(*kind);
            }
        }
        const engine::ModeResult r = e->set_mode(*mode, sim, std::move(opt));
        if (!r.ok) throw CommandError(ErrorCode::Internal, r.error);
        json::Value out = json::Value::object();
        out.set("mode", control::to_string(r.settings.mode));
        out.set("simulation", r.settings.mode == control::Mode::Monitor || r.settings.simulation);
        out.set("simulationFlag", r.settings.simulation);
        json::Value kinds = json::Value::array();
        for (auto k : r.settings.opted_in) kinds.push_back(telemetry::to_string(k));
        out.set("optIn", std::move(kinds));
        out.set("persisted", r.persisted);
        json::Value rb = json::Value::object();
        rb.set("restored", r.rolled_back.restored);
        rb.set("failed", r.rolled_back.failed);
        rb.set("expired", r.rolled_back.expired);
        out.set("rolledBack", std::move(rb));
        return out;
    });
    d.add("rollbackAll", Capability::Admin, [e](const Request& req, const ClientContext&) {
        allow_only(req, {});
        const auto s = e->rollback_all();
        json::Value o = json::Value::object();
        o.set("restored", s.restored);
        o.set("failed", s.failed);
        o.set("expired", s.expired);
        return o;
    });
    d.add("disablePolicy", Capability::Admin, [e](const Request& req, const ClientContext&) {
        allow_only(req, {"txId"});
        const std::string tx = id_arg(req, "txId");
        json::Value o = json::Value::object();
        o.set("restored", e->disable_policy(tx));
        return o;
    });
    d.add("approve", Capability::Admin, [e](const Request& req, const ClientContext&) {
        allow_only(req, {"id"});
        const std::string id = id_arg(req, "id");
        const engine::ApproveResult r = e->approve(id);
        if (r.kind == engine::ApproveResult::Kind::Unknown) bad_args("unknown approval id");
        if (r.kind == engine::ApproveResult::Kind::Refused) bad_args("approval refused: " + r.detail);
        json::Value o = json::Value::object();
        o.set("txId", r.tx_id);
        o.set("kind", r.kind == engine::ApproveResult::Kind::Recovery ? "recovery" : "policy");
        o.set("detail", r.detail);
        return o;
    });
    d.add("exportDiagnostics", Capability::Admin, [e, pf](const Request& req, const ClientContext&) {
        allow_only(req, {});
        const std::string body = json::serialize(build_diagnostics(*e, *pf));
        const auto path = e->write_data_file(engine::kDiagnosticsFile, body);
        if (!path) throw CommandError(ErrorCode::Internal, "the diagnostics bundle could not be written");
        json::Value o = json::Value::object();
        o.set("path", *path);
        o.set("bytes", body.size());
        return o;
    });
}

}  // namespace opensweep::commands2
