#include "boost.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <functional>
#include <unordered_map>

namespace opensweep::boost {

using journal::TxState;
using telemetry::PolicyKind;
using telemetry::WorkloadClass;

namespace {

// Actuator calls must never throw into the engine loop.
telemetry::ApplyResult guarded_call(const std::function<telemetry::ApplyResult()>& f) {
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

bool alive_safe(telemetry::Actuator& a, const telemetry::PolicyTarget& t) {
    try {
        return a.alive(t);
    } catch (...) {
        return false;
    }
}

std::string fmt1(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f", std::isfinite(v) ? v : 0.0);
    return buf;
}

bool contains(std::span<const std::uint32_t> v, std::uint32_t x) { return std::find(v.begin(), v.end(), x) != v.end(); }

bool own_process(std::string_view name) {
    const std::string n = sched::normalize_image(name);
    return n == "opensweepcore" || n == "opensweep" || n.rfind("opensweep", 0) == 0;
}

}  // namespace

const char* workload_name(WorkloadClass c) noexcept {
    switch (c) {
        case WorkloadClass::LatencyCritical: return "latency_critical";
        case WorkloadClass::Interactive: return "interactive";
        case WorkloadClass::Throughput: return "throughput";
        case WorkloadClass::Background: return "background";
        case WorkloadClass::Efficiency: return "efficiency";
        case WorkloadClass::Unknown: break;
    }
    return "unknown";
}

GameBoost::GameBoost(telemetry::Actuator& act, journal::Journal& journal, BoostConfig cfg)
    : act_(act), journal_(journal), cfg_(std::move(cfg)) {
    if (cfg_.window_samples == 0) cfg_.window_samples = 1;
    if (cfg_.max_parent_depth < 1) cfg_.max_parent_depth = 1;
}

// ---- crash recovery ---------------------------------------------------------------------------
control::RollbackSummary GameBoost::recover(std::int64_t now) {
    control::RollbackSummary sum;
    std::vector<std::string> ids;
    for (const auto* t : journal_.unfinished(true))
        if (t->kind == "policy" && t->trigger == kTrigger) ids.push_back(t->tx_id);
    for (const auto& id : ids) {
        const journal::Transaction* t = journal_.find(id);
        if (!t) continue;
        const auto kind = control::parse_policy_kind(t->action);
        if (!kind) {
            journal_.update(id, now, TxState::Failed, "crash_recovery", "unknown action; cannot restore");
            ++sum.failed;
            continue;
        }
        telemetry::PolicyTarget target;
        target.pid = t->target_pid;
        target.name = t->target;
        target.process_start_ms = t->target_start_ms;
        const bool known = t->has_original;
        const std::int64_t original = known ? t->original : control::default_value(*kind);
        if (!alive_safe(act_, target)) {
            journal_.update(id, now, TxState::Expired, "crash_recovery", "process gone; nothing restored");
            ++sum.expired;
            continue;
        }
        const auto res = guarded_call([&] { return act_.restore(*kind, target, original); });
        if (res.ok) {
            journal_.update(id, now, TxState::RolledBack, "crash_recovery",
                            known ? "boost session interrupted; restored original " + std::to_string(original)
                                  : "boost session interrupted before apply returned; restored default " +
                                        std::to_string(original));
            ++sum.restored;
        } else {
            journal_.update(id, now, TxState::Failed, "crash_recovery", "restore failed: " + res.error);
            ++sum.failed;
        }
    }
    return sum;
}

// ---- switches ---------------------------------------------------------------------------------
control::RollbackSummary GameBoost::set_enabled(bool on, std::int64_t now) {
    control::RollbackSummary sum;
    enabled_ = on;
    if (!on) {
        sum = end(now, "disabled");
        hist_.clear();
        fs_pid_ = 0;
        fs_since_ = 0;
        fullscreen_now_ = false;
    }
    return sum;
}

void GameBoost::report_foreground(std::uint32_t pid, bool fullscreen, std::int64_t now) {
    hint_.pid = pid;
    hint_.fullscreen = fullscreen;
    hint_.ts = now;
}

// ---- per-process CPU history --------------------------------------------------------------------
void GameBoost::record(const telemetry::Sample& s) {
    const std::size_t w = cfg_.window_samples;
    for (const auto& p : s.top) {
        if (p.pid == 0) continue;
        auto it = hist_.find(p.pid);
        if (it == hist_.end()) {
            if (hist_.size() >= cfg_.max_tracked) continue;  // bounded; the oldest are pruned below
            it = hist_.emplace(p.pid, History{}).first;
            it->second.cpu.assign(w, 0.0f);
            it->second.stamp.assign(w, 0);
            it->second.start_ms = p.start_ms;
        } else if (it->second.start_ms != p.start_ms) {  // PID reuse: start over
            it->second.cpu.assign(w, 0.0f);
            it->second.stamp.assign(w, 0);
            it->second.start_ms = p.start_ms;
        }
        History& h = it->second;
        const std::size_t slot = static_cast<std::size_t>(tick_ % w);
        const double cpu = std::isfinite(p.cpu_percent) ? std::clamp(p.cpu_percent, 0.0, 100.0) : 0.0;
        h.cpu[slot] = static_cast<float>(cpu);
        h.stamp[slot] = tick_;
        h.last_tick = tick_;
    }
    // Forget processes not seen for a whole window.
    for (auto it = hist_.begin(); it != hist_.end();) {
        if (tick_ - it->second.last_tick >= w)
            it = hist_.erase(it);
        else
            ++it;
    }
}

sched::HistoryView GameBoost::history_view(std::uint32_t pid) const {
    sched::HistoryView v;
    auto it = hist_.find(pid);
    if (it == hist_.end()) return v;
    const History& h = it->second;
    const std::size_t w = cfg_.window_samples;
    double sum = 0.0;
    double mn = 1e9;
    int n = 0;
    for (std::size_t i = 0; i < w; ++i) {
        if (h.stamp[i] == 0 || tick_ - h.stamp[i] >= w) continue;
        sum += h.cpu[i];
        mn = std::min(mn, static_cast<double>(h.cpu[i]));
        ++n;
    }
    v.samples = n;
    v.avg_cpu = sum / static_cast<double>(w);  // absent samples count as 0
    v.min_cpu = n == static_cast<int>(w) ? mn : 0.0;
    v.present = h.last_tick == tick_;
    return v;
}

// ---- session ------------------------------------------------------------------------------------
bool GameBoost::fullscreen_foreground(const telemetry::Sample& s) const {
    if (hint_.pid == 0 || !hint_.fullscreen) return false;
    if (s.ts_ms - hint_.ts > cfg_.hint_ttl_ms) return false;  // stale: the app stopped reporting
    return s.foreground_pid != 0 && s.foreground_pid == hint_.pid;  // the collector verified it
}

void GameBoost::start_session(const telemetry::Sample& s, std::int64_t t) {
    active_ = true;
    since_ = t;
    not_fs_since_ = 0;
    game_ = {};
    game_.pid = fs_pid_;
    for (const auto& p : s.top) {
        if (p.pid == fs_pid_) {
            game_.name = p.name;
            game_.process_start_ms = p.start_ms;
            break;
        }
    }
    family_.clear();
    throttled_.clear();
    throttled_count_ = 0;
}

control::RollbackSummary GameBoost::end(std::int64_t now, std::string_view reason, bool suppress) {
    control::RollbackSummary sum;
    if (suppress) suppressed_ = true;
    if (!active_) return sum;
    for (auto& tt : throttled_) restore_target(tt, now, reason, sum);
    SessionSummary ss;
    ss.start = since_;
    ss.end = now;
    ss.throttled_count = throttled_count_;
    ss.reason = std::string(reason);
    ss.restore = sum;
    last_ = std::move(ss);
    throttled_.clear();
    family_.clear();
    active_ = false;
    since_ = 0;
    game_ = {};
    not_fs_since_ = 0;
    fs_pid_ = 0;
    fs_since_ = 0;
    return sum;
}

void GameBoost::restore_target(ThrottledTarget& tt, std::int64_t now, std::string_view reason,
                               control::RollbackSummary& sum) {
    const bool alive = alive_safe(act_, tt.target);
    for (auto& a : tt.applied) {
        if (!alive) {
            journal_.update(a.tx_id, now, TxState::Expired, "target_gone", std::string(reason) + "; nothing restored");
            ++sum.expired;
            continue;
        }
        const auto res = guarded_call([&] { return act_.restore(a.kind, tt.target, a.original); });
        if (res.ok) {
            journal_.update(a.tx_id, now, TxState::RolledBack, "rolled_back", reason);
            ++sum.restored;
        } else {
            journal_.update(a.tx_id, now, TxState::Failed, "rollback_failed", std::string(reason) + ": " + res.error);
            ++sum.failed;
        }
    }
    tt.applied.clear();
}

void GameBoost::update_family(const telemetry::Sample& s) {
    std::unordered_map<std::uint32_t, std::uint32_t> parent;
    parent.reserve(s.top.size());
    for (const auto& p : s.top) parent[p.pid] = p.parent_pid;
    auto is_root = [&](std::uint32_t pid) {
        return pid != 0 && (pid == game_.pid || pid == s.foreground_pid || family_.count(pid) != 0);
    };
    for (const auto& p : s.top) {
        if (p.pid == 0 || p.pid == 4 || is_root(p.pid)) continue;
        std::uint32_t cur = p.parent_pid;
        for (int depth = 0; depth < cfg_.max_parent_depth && cur != 0 && cur != 4; ++depth) {
            if (is_root(cur)) {
                if (family_.size() < cfg_.max_family) family_.insert(p.pid);
                break;
            }
            auto it = parent.find(cur);
            if (it == parent.end() || it->second == cur) break;
            cur = it->second;
        }
    }
}

bool GameBoost::throttle(const telemetry::ProcessSample& p, WorkloadClass cls, double avg, std::int64_t t) {
    ThrottledTarget tt;
    tt.target.pid = p.pid;
    tt.target.name = p.name;
    tt.target.process_start_ms = p.start_ms;
    tt.workload = cls;
    tt.avg_cpu = avg;
    const std::pair<PolicyKind, std::int64_t> actions[] = {{PolicyKind::EcoQoS, 1},
                                                          {PolicyKind::MemoryPriority, cfg_.memory_priority}};
    bool journal_ok = true;
    for (const auto& [kind, value] : actions) {
        journal::Transaction tx;
        tx.kind = "policy";
        tx.trigger = kTrigger;
        tx.evidence = {"game boost session for " + (game_.name.empty() ? std::string("pid ") + std::to_string(game_.pid)
                                                                         : game_.name) +
                           " (fullscreen foreground)",
                       "average CPU " + fmt1(avg) + " % over the last 30 s",
                       std::string("workload=") + workload_name(cls)};
        tx.preconditions = {"boost enabled by the user", "target alive and start time matches",
                            "not the foreground app or one of its child processes", "not on the protection list",
                            "not held by another OpenSweep policy"};
        tx.target_pid = p.pid;
        tx.target = p.name;
        tx.target_start_ms = p.start_ms;
        tx.action = telemetry::to_string(kind);
        tx.value = value;
        tx.required_auth = "admin";
        tx.verification = "none (restored when the boost session ends)";
        tx.rollback = "restore the original value through the actuator when the boost session ends "
                      "(skipped if the process exited)";
        const auto id = journal_.begin(std::move(tx), t);  // write-ahead: before touching the system
        if (!id) {
            journal_ok = false;
            break;
        }
        const auto res = guarded_call([&] { return act_.apply(kind, tt.target, value); });
        if (!res.ok) {
            journal_.update(*id, t, TxState::Failed, "apply_failed", res.error);
            continue;
        }
        journal_.update(*id, t, TxState::Applied, "applied", "original=" + std::to_string(res.original),
                        [&](journal::Transaction& x) {
                            x.original = res.original;
                            x.has_original = true;
                        });
        if (kind == PolicyKind::MemoryPriority && res.original >= 1 && res.original < value) {
            // It was already lower than what boost sets: put it back right away, never raise it.
            const auto back = guarded_call([&] { return act_.restore(kind, tt.target, res.original); });
            journal_.update(*id, t, back.ok ? TxState::RolledBack : TxState::Failed, "already_lower",
                            back.ok ? "memory priority was already lower; restored" : "restore failed: " + back.error);
            continue;
        }
        tt.applied.push_back({kind, *id, res.original});
    }
    if (!tt.applied.empty()) {
        throttled_.push_back(std::move(tt));
        ++throttled_count_;
    }
    return journal_ok;
}

void GameBoost::select_targets(const telemetry::Sample& s, std::span<const std::uint32_t> control_pids,
                               std::int64_t t) {
    if (throttled_.size() >= cfg_.max_targets) return;
    struct Cand {
        const telemetry::ProcessSample* p;
        WorkloadClass cls;
        double avg;
    };
    std::vector<Cand> cands;
    for (const auto& p : s.top) {
        if (p.pid <= 4 || p.session_id == 0 || p.start_ms == 0) continue;
        if (p.pid == game_.pid || p.pid == s.foreground_pid || p.foreground || family_.count(p.pid)) continue;
        if (own_process(p.name) || contains(control_pids, p.pid)) continue;
        if (std::any_of(throttled_.begin(), throttled_.end(),
                        [&](const ThrottledTarget& x) { return x.target.pid == p.pid; }))
            continue;
        const sched::HistoryView h = history_view(p.pid);
        if (!(h.avg_cpu >= cfg_.min_avg_cpu)) continue;
        const WorkloadClass cls = sched::classify(p, h, cfg_.lists);
        if (sched::protection_reason(p, cls, cfg_.lists)) continue;
        if (cls != WorkloadClass::Background && cls != WorkloadClass::Throughput) continue;  // verified only
        cands.push_back({&p, cls, h.avg_cpu});
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
        return a.avg != b.avg ? a.avg > b.avg : a.p->pid < b.p->pid;
    });
    for (const auto& c : cands) {
        if (throttled_.size() >= cfg_.max_targets) break;
        if (!alive_safe(act_, {c.p->pid, c.p->name, c.p->start_ms})) continue;
        if (!throttle(*c.p, c.cls, c.avg, t)) break;  // journal unavailable: stop (fail-safe)
    }
}

void GameBoost::tick(const telemetry::Sample& s, std::span<const std::uint32_t> control_pids) {
    const std::int64_t t = s.ts_ms;
    ++tick_;
    last_fg_pid_ = s.foreground_pid;
    last_fg_name_.clear();
    for (const auto& p : s.top)
        if (p.pid == s.foreground_pid) {
            last_fg_name_ = p.name;
            break;
        }
    fullscreen_now_ = fullscreen_foreground(s);
    if (!enabled_) {
        if (active_) end(t, "disabled");
        return;
    }
    record(s);
    if (!fullscreen_now_) suppressed_ = false;

    if (!active_) {
        if (!fullscreen_now_ || suppressed_) {
            fs_pid_ = 0;
            fs_since_ = 0;
            return;
        }
        if (fs_pid_ != s.foreground_pid) {
            fs_pid_ = s.foreground_pid;
            fs_since_ = t;
        }
        if (t - fs_since_ < cfg_.start_after_ms) return;
        start_session(s, t);
    } else {
        if (!alive_safe(act_, game_)) {
            end(t, "foreground_exited");
            return;
        }
        if (fullscreen_now_ && s.foreground_pid == game_.pid) {
            not_fs_since_ = 0;
        } else {
            if (not_fs_since_ == 0) not_fs_since_ = t;
            if (t - not_fs_since_ >= cfg_.end_after_ms) {
                end(t, "fullscreen_ended");
                return;
            }
        }
    }

    // Keep the exclusions current and release targets that must not stay throttled.
    update_family(s);
    for (auto it = throttled_.begin(); it != throttled_.end();) {
        const std::uint32_t pid = it->target.pid;
        const bool now_excluded = pid == s.foreground_pid || pid == game_.pid || family_.count(pid) != 0 ||
                                  contains(control_pids, pid);
        if (now_excluded) {
            control::RollbackSummary sum;
            restore_target(*it, t, "target_became_foreground", sum);
            it = throttled_.erase(it);
        } else if (!alive_safe(act_, it->target)) {
            control::RollbackSummary sum;
            restore_target(*it, t, "target_exited", sum);  // journals expired
            it = throttled_.erase(it);
        } else {
            ++it;
        }
    }
    select_targets(s, control_pids, t);
}

std::vector<std::uint32_t> GameBoost::held_pids() const {
    std::vector<std::uint32_t> out;
    out.reserve(throttled_.size());
    for (const auto& tt : throttled_) out.push_back(tt.target.pid);
    return out;
}

View GameBoost::view() const {
    View v;
    v.enabled = enabled_;
    v.active = active_;
    v.since = active_ ? since_ : 0;
    if (active_) {
        v.foreground_pid = game_.pid;
        v.foreground_name = game_.name;
    } else {
        v.foreground_pid = last_fg_pid_;
        v.foreground_name = last_fg_name_;
    }
    v.throttled = throttled_;
    v.last_session = last_;
    return v;
}

}  // namespace opensweep::boost
