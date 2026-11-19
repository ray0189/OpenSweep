#include "control.hpp"

#include <algorithm>
#include <cstdio>
#include <exception>

namespace opensweep::control {

using journal::TxState;
using telemetry::WorkloadClass;

namespace {

std::string fmt(double v, int decimals = 2) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
    return buf;
}

std::string hysteresis_key(const policy::Recommendation& r) {
    return target_key(r.target) + "|" + telemetry::to_string(r.kind);
}

bool same_target(const telemetry::PolicyTarget& a, const telemetry::PolicyTarget& b) {
    return a.pid == b.pid && a.process_start_ms == b.process_start_ms && a.name == b.name;
}

template <typename F>
telemetry::ApplyResult guarded(F&& f) {
    try {
        return f();
    } catch (const std::exception& e) {
        telemetry::ApplyResult r;
        r.error = std::string("actuator exception: ") + e.what();
        return r;
    } catch (...) {
        telemetry::ApplyResult r;
        r.error = "actuator exception";
        return r;
    }
}

template <typename F>
bool guarded_bool(F&& f) {
    try {
        return f();
    } catch (...) {
        return false;
    }
}

}  // namespace

std::string target_key(const telemetry::PolicyTarget& t) {
    return std::to_string(t.pid) + ":" + std::to_string(t.process_start_ms) + ":" + t.name;
}

const char* to_string(Mode m) noexcept {
    switch (m) {
        case Mode::Monitor: return "monitor";
        case Mode::Balanced: return "balanced";
        case Mode::Advanced: return "advanced";
    }
    return "monitor";
}

std::optional<Mode> parse_mode(std::string_view s) noexcept {
    if (s == "monitor") return Mode::Monitor;
    if (s == "balanced") return Mode::Balanced;
    if (s == "advanced") return Mode::Advanced;
    return std::nullopt;
}

std::optional<PolicyKind> parse_policy_kind(std::string_view s) noexcept {
    for (PolicyKind k : {PolicyKind::EcoQoS, PolicyKind::MemoryPriority, PolicyKind::PriorityClass, PolicyKind::CpuSets})
        if (s == telemetry::to_string(k)) return k;
    return std::nullopt;
}

const char* to_string(Outcome o) noexcept {
    switch (o) {
        case Outcome::Simulated: return "simulated";
        case Outcome::Applied: return "applied";
        case Outcome::Blocked: return "blocked";
        case Outcome::PendingConsent: return "pending_consent";
        case Outcome::Failed: return "failed";
    }
    return "blocked";
}

double intervention_cost(const policy::Recommendation& r) noexcept {
    double impact = 2.0;
    double reversibility = 1.0;  // 1 = trivially and fully reversible
    switch (r.kind) {
        case PolicyKind::MemoryPriority: impact = 1.0; break;
        case PolicyKind::EcoQoS: impact = 2.0; break;
        case PolicyKind::PriorityClass: impact = 2.0; break;
        case PolicyKind::CpuSets:
            impact = 3.0;
            reversibility = 1.5;
            break;
    }
    const double risk = static_cast<double>(std::clamp(r.risk, 1, 5));
    return risk * impact * reversibility;
}

std::int64_t default_value(PolicyKind k) noexcept {
    switch (k) {
        case PolicyKind::EcoQoS: return -1;  // let the system manage
        case PolicyKind::MemoryPriority: return policy::kMemoryPriorityNormal;
        case PolicyKind::PriorityClass: return policy::kPriorityNormal;
        case PolicyKind::CpuSets: return 0;  // clear the default CPU set assignment
    }
    return 0;
}

void ControlEngine::Ledger::prune(std::int64_t now, std::int64_t window) {
    while (!applies.empty() && applies.front() <= now - window) applies.pop_front();
    for (auto it = cooldown_until.begin(); it != cooldown_until.end();) {
        if (it->second <= now)
            it = cooldown_until.erase(it);
        else
            ++it;
    }
}

ControlEngine::ControlEngine(telemetry::Actuator& act, journal::Journal& journal, ControlConfig cfg)
    : act_(act), journal_(journal), cfg_(std::move(cfg)) {
    if (cfg_.hysteresis_ticks < 1) cfg_.hysteresis_ticks = 1;
    if (cfg_.rate_limit_actions < 0) cfg_.rate_limit_actions = 0;
}

std::vector<std::uint32_t> ControlEngine::active_pids(PolicyKind k) const {
    std::vector<std::uint32_t> out;
    for (const auto& ap : active_)
        if (ap.rec.kind == k) out.push_back(ap.rec.target.pid);
    return out;
}

bool ControlEngine::allow_listed(Mode m, const policy::Recommendation& r) const {
    if (m == Mode::Monitor) return false;
    switch (r.kind) {
        case PolicyKind::EcoQoS: return r.value == 1;
        case PolicyKind::MemoryPriority:
            return r.value >= policy::kMemoryPriorityLow && r.value < policy::kMemoryPriorityNormal;
        case PolicyKind::PriorityClass: return r.value == static_cast<std::int64_t>(policy::kPriorityBelowNormal);
        case PolicyKind::CpuSets: return m == Mode::Advanced && r.value > 0;
    }
    return false;
}

bool ControlEngine::opted_in(PolicyKind k) const {
    return std::find(cfg_.opted_in.begin(), cfg_.opted_in.end(), k) != cfg_.opted_in.end();
}

std::optional<std::string> ControlEngine::gate(const policy::Recommendation& r, std::int64_t now, Mode effective,
                                               double cost, bool conflicted, const Ledger& ledger,
                                               bool simulated) const {
    if (r.target.pid == 0 || r.target.pid == 4 || r.target.name.empty()) return "invalid_target";
    if (r.workload != WorkloadClass::Background) return "workload_not_background";
    if (!allow_listed(effective, r)) return "not_allow_listed";
    // In monitor mode opt-ins are ignored so the user can see what opting in would do.
    if (cfg_.mode != Mode::Monitor && !opted_in(r.kind)) return "not_opted_in";
    if (conflicted) return "conflict";
    if (!simulated)
        for (const auto& ap : active_)
            if (ap.rec.kind == r.kind && same_target(ap.rec.target, r.target)) return "already_active";
    if (auto it = persist_.find(hysteresis_key(r)); it == persist_.end() || it->second < cfg_.hysteresis_ticks)
        return "hysteresis";
    if (auto it = ledger.cooldown_until.find(target_key(r.target)); it != ledger.cooldown_until.end() && it->second > now)
        return "cooldown";
    const double max_cost = effective == Mode::Advanced ? cfg_.max_cost_advanced : cfg_.max_cost_balanced;
    if (cost > max_cost) return "cost_too_high";
    if (!simulated && active_.size() >= cfg_.max_active) return "max_active_policies";
    if (ledger.applies.size() >= static_cast<std::size_t>(cfg_.rate_limit_actions)) return "rate_limit";
    return std::nullopt;
}

TickResult ControlEngine::step(std::int64_t now, std::span<const policy::Recommendation> recs,
                               const policy::VerificationMetrics& current) {
    TickResult out;
    verify_active(now, current, out);

    // Expire consents and pending consent requests.
    for (auto it = consents_.begin(); it != consents_.end();) {
        if (it->second.expires_ms <= now)
            it = consents_.erase(it);
        else
            ++it;
    }
    std::erase_if(pending_, [&](const PendingConsent& p) { return p.expires_ms <= now; });
    real_.prune(now, cfg_.rate_limit_window_ms);
    sim_.prune(now, cfg_.rate_limit_window_ms);

    // De-duplicate identical (target, kind, value) recommendations from different policies.
    std::vector<policy::Recommendation> uniq;
    uniq.reserve(recs.size());
    for (const auto& r : recs) {
        auto dup = std::find_if(uniq.begin(), uniq.end(), [&](const policy::Recommendation& u) {
            return u.kind == r.kind && u.value == r.value && same_target(u.target, r.target);
        });
        if (dup != uniq.end()) {
            if (dup->area != r.area && dup->evidence.size() < 16)
                dup->evidence.push_back(std::string("also recommended by ") + policy::to_string(r.area) + " policy");
            continue;
        }
        uniq.push_back(r);
    }

    // Hysteresis: count consecutive ticks each (target, kind) is recommended; forget absent ones.
    {
        std::unordered_map<std::string, int> next;
        for (const auto& r : uniq) {
            const std::string k = hysteresis_key(r);
            if (next.count(k)) continue;
            auto it = persist_.find(k);
            next[k] = (it == persist_.end() ? 0 : it->second) + 1;
        }
        persist_.swap(next);
    }

    if (uniq.empty()) {
        out.no_action_needed = true;
        out.reason = "no action needed: no sustained, attributed contention or memory pressure";
        return out;
    }

    // Conflicts: same target/kind with different values; a target that another recommendation
    // (or an active policy) is trying to help; a different value than an already active policy.
    std::vector<bool> conflicted(uniq.size(), false);
    for (std::size_t i = 0; i < uniq.size(); ++i) {
        for (std::size_t j = 0; j < uniq.size(); ++j) {
            if (i == j) continue;
            if (uniq[i].kind == uniq[j].kind && same_target(uniq[i].target, uniq[j].target) &&
                uniq[i].value != uniq[j].value)
                conflicted[i] = true;
            if (uniq[j].beneficiary_pid != 0 && uniq[j].beneficiary_pid == uniq[i].target.pid) conflicted[i] = true;
        }
        for (const auto& ap : active_) {
            if (ap.rec.kind == uniq[i].kind && same_target(ap.rec.target, uniq[i].target) &&
                ap.rec.value != uniq[i].value)
                conflicted[i] = true;
            if (ap.rec.beneficiary_pid != 0 && ap.rec.beneficiary_pid == uniq[i].target.pid) conflicted[i] = true;
        }
    }

    // Minimal intervention first.
    std::vector<std::size_t> order(uniq.size());
    std::vector<double> costs(uniq.size());
    for (std::size_t i = 0; i < uniq.size(); ++i) {
        order[i] = i;
        costs[i] = intervention_cost(uniq[i]);
    }
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return costs[a] < costs[b]; });

    const bool sim = simulating();
    const Mode effective = cfg_.mode == Mode::Monitor ? Mode::Balanced : cfg_.mode;
    int applied = 0;
    int would = 0;
    out.decisions.reserve(uniq.size());
    for (std::size_t idx : order) {
        const policy::Recommendation& r = uniq[idx];
        const double cost = costs[idx];
        auto blocked = gate(r, now, effective, cost, conflicted[idx], sim ? sim_ : real_, sim);
        Decision d;
        if (sim) {
            d.rec = r;
            d.cost = cost;
            d.outcome = Outcome::Simulated;
            d.would_apply = !blocked;
            d.blocked_by = blocked ? *blocked : (cfg_.mode == Mode::Monitor ? "monitor_mode" : "simulation");
            if (!blocked) {
                ++would;
                sim_.cooldown_until[target_key(r.target)] = now + cfg_.cooldown_ms;
                sim_.applies.push_back(now);
            }
        } else if (blocked) {
            d.rec = r;
            d.cost = cost;
            d.outcome = Outcome::Blocked;
            d.blocked_by = *blocked;
        } else if (cfg_.mode == Mode::Advanced) {
            auto c = consents_.find(r.id);
            if (c == consents_.end()) {
                d.rec = r;
                d.cost = cost;
                d.outcome = Outcome::PendingConsent;
                d.would_apply = true;
                d.blocked_by = "consent_required";
                auto p = std::find_if(pending_.begin(), pending_.end(),
                                      [&](const PendingConsent& pc) { return pc.id == r.id; });
                if (p == pending_.end()) {
                    if (pending_.size() >= cfg_.max_pending_consents) pending_.erase(pending_.begin());
                    pending_.push_back({r.id, r, now, now + cfg_.consent_ttl_ms});
                } else {
                    p->rec = r;  // keep the latest evidence
                }
            } else {
                const std::string consent_id = c->second.consent_id;
                consents_.erase(c);
                d = apply(r, cost, now, current, consent_id);
                if (d.outcome == Outcome::Applied) ++applied;
            }
        } else {
            d = apply(r, cost, now, current, std::string());
            if (d.outcome == Outcome::Applied) ++applied;
        }
        out.decisions.push_back(std::move(d));
    }
    if (sim)
        out.reason = std::to_string(would) + " of " + std::to_string(uniq.size()) +
                     " recommendation(s) would be applied (" + (cfg_.mode == Mode::Monitor ? "monitor" : "simulation") +
                     " mode: nothing changed)";
    else
        out.reason = std::to_string(applied) + " of " + std::to_string(uniq.size()) + " recommendation(s) applied";
    return out;
}

Decision ControlEngine::apply(const policy::Recommendation& r, double cost, std::int64_t now,
                              const policy::VerificationMetrics& current, const std::string& consent_id) {
    Decision d;
    d.rec = r;
    d.cost = cost;
    if (!guarded_bool([&] { return act_.alive(r.target); })) {
        d.outcome = Outcome::Blocked;
        d.blocked_by = "target_gone";
        return d;
    }
    journal::Transaction tx;
    tx.kind = "policy";
    tx.trigger = r.reason;
    tx.evidence = r.evidence;
    tx.preconditions = {"mode=" + std::string(to_string(cfg_.mode)), "target alive and start time matches",
                        std::string("workload=") + telemetry::to_string(r.workload), "not on the protection list",
                        "sustained for " + std::to_string(cfg_.hysteresis_ticks) + " ticks", "cost=" + fmt(cost)};
    if (!consent_id.empty()) tx.preconditions.push_back("consent=" + consent_id);
    tx.target_pid = r.target.pid;
    tx.target = r.target.name;
    tx.target_start_ms = r.target.process_start_ms;
    tx.action = telemetry::to_string(r.kind);
    tx.value = r.value;
    tx.required_auth = cfg_.mode == Mode::Advanced ? "admin" : "user";
    tx.verification = "compare foreground responsiveness, cpu queue, hard faults and commit over " +
                      std::to_string(cfg_.verify_window_ms / 1000) + " s; retain only if improved";
    tx.rollback = "restore the original value through the actuator (skipped if the process exited)";
    tx.cooldown_until = now + cfg_.cooldown_ms;
    tx.attempt = 1;
    tx.max_attempts = 1;

    const auto id = journal_.begin(std::move(tx), now);  // write-ahead: before touching the system
    if (!id) {
        d.outcome = Outcome::Blocked;
        d.blocked_by = "journal_unavailable";
        return d;
    }
    d.tx_id = *id;
    real_.applies.push_back(now);
    real_.cooldown_until[target_key(r.target)] = now + cfg_.cooldown_ms;

    const telemetry::ApplyResult res = guarded([&] { return act_.apply(r.kind, r.target, r.value); });
    if (!res.ok) {
        journal_.update(*id, now, TxState::Failed, "apply_failed", res.error);
        d.outcome = Outcome::Failed;
        d.blocked_by = "apply_failed";
        return d;
    }
    journal_.update(*id, now, TxState::Applied, "applied", "original=" + std::to_string(res.original),
                    [&](journal::Transaction& t) {
                        t.original = res.original;
                        t.has_original = true;
                    });
    journal_.update(*id, now, TxState::Verifying, "verifying",
                    "until " + std::to_string(now + cfg_.verify_window_ms));
    ActivePolicy ap;
    ap.tx_id = *id;
    ap.rec = r;
    ap.original = res.original;
    ap.applied_at = now;
    ap.verify_until = now + cfg_.verify_window_ms;
    ap.retain_until = ap.verify_until + cfg_.max_retain_ms;
    ap.verifying = true;
    ap.before = current;
    active_.push_back(std::move(ap));
    d.outcome = Outcome::Applied;
    d.would_apply = true;
    return d;
}

bool ControlEngine::improved(const ActivePolicy& ap, const policy::VerificationMetrics& after,
                             std::string& why) const {
    struct M {
        double before, after, floor;
        const char* name;
        bool known() const { return before >= 0.0 && after >= 0.0; }
    };
    const auto& b = ap.before;
    const M fg{b.fg_latency_ms, after.fg_latency_ms, 2.0, "fg_latency_ms"};
    const M hf{b.hard_faults, after.hard_faults, 5.0, "hard_faults"};
    const M q{b.cpu_queue, after.cpu_queue, 0.25, "cpu_queue"};
    const M cm{b.commit_percent, after.commit_percent, 1.0, "commit_percent"};
    for (const M* m : {&fg, &hf, &q, &cm}) {
        if (m->known() && m->after > m->before * (1.0 + cfg_.worsen_threshold) + m->floor) {
            why = std::string("worsened: ") + m->name + " " + fmt(m->before) + " -> " + fmt(m->after);
            return false;
        }
    }
    const M* primary = nullptr;
    if (ap.rec.area == policy::Area::Cpu)
        primary = fg.known() ? &fg : (q.known() ? &q : nullptr);
    else
        primary = hf.known() ? &hf : (cm.known() ? &cm : (fg.known() ? &fg : nullptr));
    if (!primary) {
        why = "no evidence of improvement (metrics unknown)";
        return false;
    }
    if (primary->before > 0.0 && primary->after <= primary->before * (1.0 - cfg_.improve_threshold)) {
        why = std::string("improved: ") + primary->name + " " + fmt(primary->before) + " -> " + fmt(primary->after);
        return true;
    }
    why = std::string("no improvement: ") + primary->name + " " + fmt(primary->before) + " -> " + fmt(primary->after);
    return false;
}

void ControlEngine::verify_active(std::int64_t now, const policy::VerificationMetrics& current, TickResult& out) {
    for (auto it = active_.begin(); it != active_.end();) {
        ActivePolicy& ap = *it;
        bool remove = false;
        if (!guarded_bool([&] { return act_.alive(ap.rec.target); })) {
            journal_.update(ap.tx_id, now, TxState::Expired, "target_exited", "process exited; nothing to restore");
            out.verifications.push_back({ap.tx_id, false, "target_exited"});
            remove = true;
        } else if (ap.verifying && now >= ap.verify_until) {
            std::string why;
            if (improved(ap, current, why)) {
                journal_.update(ap.tx_id, now, TxState::Retained, "verified", why,
                                [&](journal::Transaction& t) { t.verification_result = why; });
                ap.verifying = false;
                ap.retain_until = now + cfg_.max_retain_ms;
                out.verifications.push_back({ap.tx_id, true, why});
            } else {
                journal_.update(ap.tx_id, now, TxState::Verifying, "verification_failed", why,
                                [&](journal::Transaction& t) { t.verification_result = why; });
                RollbackSummary s;
                rollback(ap, now, "verification_failed", s);
                out.verifications.push_back({ap.tx_id, false, why});
                remove = true;
            }
        } else if (!ap.verifying && now >= ap.retain_until) {
            RollbackSummary s;
            rollback(ap, now, "retention_elapsed", s);
            remove = true;
        }
        it = remove ? active_.erase(it) : it + 1;
    }
}

bool ControlEngine::rollback(ActivePolicy& ap, std::int64_t now, std::string_view reason, RollbackSummary& sum) {
    real_.cooldown_until[target_key(ap.rec.target)] = now + cfg_.cooldown_ms;
    // PID-reuse guard: never "restore" a value onto a different process.
    if (!guarded_bool([&] { return act_.alive(ap.rec.target); })) {
        journal_.update(ap.tx_id, now, TxState::Expired, "target_gone", std::string(reason) + "; nothing restored");
        ++sum.expired;
        return true;
    }
    const auto res = guarded([&] { return act_.restore(ap.rec.kind, ap.rec.target, ap.original); });
    if (res.ok) {
        journal_.update(ap.tx_id, now, TxState::RolledBack, "rolled_back", reason);
        ++sum.restored;
        return true;
    }
    journal_.update(ap.tx_id, now, TxState::Failed, "rollback_failed", std::string(reason) + ": " + res.error);
    ++sum.failed;
    return false;
}

RollbackSummary ControlEngine::rollback_where(std::int64_t now, std::string_view reason,
                                              const std::function<bool(const ActivePolicy&)>& pred) {
    RollbackSummary sum;
    for (auto it = active_.begin(); it != active_.end();) {
        if (pred(*it)) {
            rollback(*it, now, reason, sum);
            it = active_.erase(it);
        } else {
            ++it;
        }
    }
    return sum;
}

RollbackSummary ControlEngine::rollback_all(std::int64_t now, std::string_view reason) {
    return rollback_where(now, reason, [](const ActivePolicy&) { return true; });
}

bool ControlEngine::disable_policy(const std::string& tx_id, std::int64_t now) {
    bool found = false;
    const auto sum = rollback_where(now, "disabled_by_admin", [&](const ActivePolicy& ap) {
        if (ap.tx_id != tx_id) return false;
        found = true;
        return true;
    });
    return found && sum.failed == 0;
}

RollbackSummary ControlEngine::recover(std::int64_t now) {
    RollbackSummary sum;
    std::vector<std::string> ids;
    for (const auto* t : journal_.unfinished(true)) ids.push_back(t->tx_id);
    for (const auto& id : ids) {
        const journal::Transaction* t = journal_.find(id);
        if (!t) continue;
        if (t->kind == "config") {
            if (t->state != TxState::Retained)
                journal_.update(id, now, TxState::Expired, "interrupted", "service restarted before completion");
            continue;
        }
        if (t->kind != "policy") continue;  // recovery transactions belong to SweepHeal
        const auto kind = parse_policy_kind(t->action);
        if (!kind) {
            journal_.update(id, now, TxState::Failed, "crash_recovery", "unknown action; cannot restore");
            ++sum.failed;
            continue;
        }
        telemetry::PolicyTarget target;
        target.pid = t->target_pid;
        target.name = t->target;
        target.process_start_ms = t->target_start_ms;
        {
            // A rolled-back target cools down like any other rollback (and never less than journaled).
            auto& cd = real_.cooldown_until[target_key(target)];
            cd = std::max({cd, t->cooldown_until, now + cfg_.cooldown_ms});
        }
        const bool known = t->has_original;
        const std::int64_t original = known ? t->original : default_value(*kind);
        if (!guarded_bool([&] { return act_.alive(target); })) {
            journal_.update(id, now, TxState::Expired, "crash_recovery", "process gone; nothing restored");
            ++sum.expired;
            continue;
        }
        const auto res = guarded([&] { return act_.restore(*kind, target, original); });
        if (res.ok) {
            journal_.update(id, now, TxState::RolledBack, "crash_recovery",
                            known ? "restored original " + std::to_string(original)
                                  : "original unknown (interrupted before apply returned); restored default " +
                                        std::to_string(original));
            ++sum.restored;
        } else {
            journal_.update(id, now, TxState::Failed, "crash_recovery", "restore failed: " + res.error);
            ++sum.failed;
        }
    }
    return sum;
}

bool ControlEngine::journal_config(std::int64_t now, std::string_view action, std::int64_t value,
                                   std::int64_t original, std::string_view detail) {
    journal::Transaction tx;
    tx.kind = "config";
    tx.trigger = "admin request";
    tx.action = std::string(action);
    tx.value = value;
    tx.original = original;
    tx.has_original = true;
    tx.required_auth = "admin";
    tx.verification = "none (configuration change)";
    tx.rollback = "apply the original value";
    tx.evidence.push_back(std::string(detail));
    const auto id = journal_.begin(std::move(tx), now);
    if (!id) return false;
    return journal_.update(*id, now, TxState::Retained, "applied", detail);
}

ModeChange ControlEngine::set_mode(Mode m, std::int64_t now) {
    ModeChange mc;
    const Mode old = cfg_.mode;
    if (m == old) {
        mc.ok = true;
        return mc;
    }
    const bool journaled = journal_config(now, "set_mode", static_cast<std::int64_t>(m), static_cast<std::int64_t>(old),
                                          std::string(to_string(old)) + " -> " + to_string(m));
    // De-escalation always proceeds (it only removes interventions); escalation needs the journal.
    if (!journaled && m != Mode::Monitor && !(old == Mode::Advanced && m == Mode::Balanced)) return mc;
    cfg_.mode = m;
    mc.ok = true;
    if (m == Mode::Monitor) {
        mc.rolled_back = rollback_all(now, "mode_monitor");
    } else if (m == Mode::Balanced) {
        mc.rolled_back = rollback_where(now, "mode_balanced", [this](const ActivePolicy& ap) {
            return !allow_listed(Mode::Balanced, ap.rec);
        });
    }
    if (m != Mode::Advanced) {
        pending_.clear();
        consents_.clear();
    }
    return mc;
}

RollbackSummary ControlEngine::set_opt_in(PolicyKind k, bool on, std::int64_t now) {
    RollbackSummary sum;
    const bool was = opted_in(k);
    if (was == on) return sum;
    journal_config(now, on ? "opt_in" : "opt_out", static_cast<std::int64_t>(k), was ? 1 : 0,
                   std::string(on ? "opt in: " : "opt out: ") + telemetry::to_string(k));
    if (on) {
        cfg_.opted_in.push_back(k);
    } else {
        std::erase(cfg_.opted_in, k);
        sum = rollback_where(now, "opted_out", [k](const ActivePolicy& ap) { return ap.rec.kind == k; });
    }
    return sum;
}

bool ControlEngine::approve(const std::string& rec_id, const std::string& consent_id, std::int64_t now) {
    if (cfg_.mode != Mode::Advanced || consent_id.empty()) return false;
    auto p = std::find_if(pending_.begin(), pending_.end(), [&](const PendingConsent& pc) { return pc.id == rec_id; });
    if (p == pending_.end()) return false;
    if (p->expires_ms <= now) {
        pending_.erase(p);
        return false;
    }
    pending_.erase(p);
    consents_[rec_id] = {consent_id, now + cfg_.consent_ttl_ms};
    journal_config(now, "consent", 0, 0, "consent " + consent_id + " for " + rec_id);
    return true;
}

}  // namespace opensweep::control
