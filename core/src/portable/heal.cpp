#include "heal.hpp"

#include <algorithm>
#include <exception>

#include "scheduler_policy.hpp"

namespace opensweep::heal {

using journal::TxState;

namespace {

constexpr std::int64_t kSec = 1000;
constexpr std::int64_t kMin = 60 * kSec;

// clang-format off
constexpr CatalogueEntry kCatalogue[] = {
    {Action::RestartCollector, "restart_collector", "Restart a failed OpenSweep telemetry collector",
     Risk::Low, Auth::None, true, true, 3, 30 * kSec,
     "collector owned by OpenSweep has stopped producing samples",
     "collector produces a fresh sample", "none needed (stateless restart)"},
    {Action::RestartWorker, "restart_worker", "Restart a failed OpenSweep worker thread",
     Risk::Low, Auth::None, true, true, 3, 30 * kSec,
     "worker owned by OpenSweep is not responding to its heartbeat",
     "worker heartbeat resumes", "none needed (stateless restart)"},
    {Action::ReconnectEventSubscription, "reconnect_event_subscription", "Reconnect an OpenSweep event-log/ETW subscription",
     Risk::Low, Auth::None, true, true, 3, 30 * kSec,
     "subscription owned by OpenSweep reports an error or delivered nothing past its deadline",
     "subscription delivers or reports healthy", "close the new subscription"},
    {Action::ClearInternalCache, "clear_internal_cache", "Clear a bounded OpenSweep internal cache",
     Risk::Low, Auth::None, true, true, 3, 5 * kMin,
     "cache owned by OpenSweep failed validation or exceeded its bound",
     "cache rebuilt and validates", "none needed (cache is derived data)"},
    {Action::RestoreOwnedPolicies, "restore_owned_policies", "Restore OpenSweep-owned temporary process policies",
     Risk::Low, Auth::None, true, true, 3, 60 * kSec,
     "OpenSweep policies are active while the engine is unhealthy",
     "no OpenSweep policy remains active", "re-evaluated by the control engine (not re-applied blindly)"},
    {Action::RestartExplorer, "restart_explorer", "Restart Windows Explorer (shell)",
     Risk::Medium, Auth::User, false, false, 2, 10 * kMin,
     "Explorer is hung or has crashed repeatedly",
     "explorer.exe is running and responsive", "start explorer.exe if it is not running"},
    {Action::RestartService, "restart_service", "Restart a third-party Windows service",
     Risk::Medium, Auth::Admin, false, false, 2, 10 * kMin,
     "service has failed and is not a system or security service",
     "service reports RUNNING", "start the service again if it stopped"},
    {Action::ResetNetworkStack, "reset_network_stack", "Reset the network stack (winsock reset, IP/DNS flush)",
     Risk::High, Auth::Admin, false, false, 2, 60 * kMin,
     "network connectivity failure confirmed by evidence; may require a reboot",
     "default gateway and DNS resolve", "none possible; user is told a reboot may be needed"},
    {Action::RunSfc, "run_sfc", "Run System File Checker (sfc /scannow)",
     Risk::Medium, Auth::Admin, false, false, 1, 24 * 60 * kMin,
     "evidence of system file corruption",
     "sfc reports no integrity violations or repaired them", "none (repairs from the component store)"},
    {Action::RunDismRestoreHealth, "run_dism_restorehealth", "Run DISM /Online /Cleanup-Image /RestoreHealth",
     Risk::Medium, Auth::Admin, false, false, 1, 24 * 60 * kMin,
     "evidence of component store corruption",
     "DISM reports the store is repaired", "none (repairs the component store)"},
    {Action::TerminateApplication, "terminate_application", "Terminate a hung application chosen by the user",
     Risk::High, Auth::User, false, false, 1, 10 * kMin,
     "application is hung and the user agreed; unsaved work may be lost",
     "process no longer exists", "none possible (the user may restart the application)"},
};
// clang-format on

constexpr std::string_view kForbidden[] = {
    "disable_defender", "disable_firewall", "disable_security", "disable_smartscreen", "disable_uac",
    "stop_security_service", "uninstall_driver", "remove_driver", "disable_driver", "bcdedit", "alter_boot",
    "boot_config", "disable_secure_boot", "disable_hvci", "test_signing",
};

constexpr std::string_view kForbiddenTokens[] = {"defender", "firewall", "security", "driver", "boot", "bcd", "uac"};

constexpr std::string_view kProtectedServices[] = {
    "windefend", "wdnissvc", "sense", "mpssvc", "wscsvc", "securityhealthservice", "bfe", "eventlog", "rpcss",
    "rpceptmapper", "dcomlaunch", "lsm", "samss", "plugplay", "power", "profsvc", "winmgmt", "cryptsvc",
    "trustedinstaller", "opensweepcore",
};

}  // namespace

const char* to_string(Risk r) noexcept {
    switch (r) {
        case Risk::Low: return "low";
        case Risk::Medium: return "medium";
        case Risk::High: return "high";
    }
    return "high";
}

const char* to_string(Auth a) noexcept {
    switch (a) {
        case Auth::None: return "none";
        case Auth::User: return "user";
        case Auth::Admin: return "admin";
    }
    return "admin";
}

const char* to_string(Status s) noexcept {
    switch (s) {
        case Status::Succeeded: return "succeeded";
        case Status::Failed: return "failed";
        case Status::PendingApproval: return "pending_approval";
        case Status::BreakerOpen: return "breaker_open";
        case Status::Cooldown: return "cooldown";
        case Status::MaxAttempts: return "max_attempts";
        case Status::Rejected: return "rejected";
        case Status::JournalUnavailable: return "journal_unavailable";
    }
    return "rejected";
}

const char* to_string(BreakerState s) noexcept {
    switch (s) {
        case BreakerState::Closed: return "closed";
        case BreakerState::Open: return "open";
        case BreakerState::HalfOpen: return "half_open";
    }
    return "open";
}

std::span<const CatalogueEntry> catalogue() noexcept { return kCatalogue; }

const CatalogueEntry* find(Action a) noexcept {
    for (const auto& e : kCatalogue)
        if (e.action == a) return &e;
    return nullptr;
}

const CatalogueEntry* find(std::string_view id) noexcept {
    if (is_forbidden(id)) return nullptr;
    for (const auto& e : kCatalogue)
        if (e.id == id) return &e;
    return nullptr;
}

std::span<const std::string_view> forbidden_actions() noexcept { return kForbidden; }

bool is_forbidden(std::string_view id) noexcept {
    for (const auto& f : kForbidden)
        if (id == f) return true;
    for (const auto& t : kForbiddenTokens)
        if (id.find(t) != std::string_view::npos) return true;
    return false;
}

bool is_protected_target(Action a, std::string_view target) {
    const std::string n = sched::normalize_image(target);
    if (n.empty()) return true;
    switch (a) {
        case Action::TerminateApplication:
            return sched::is_builtin_protected_image(n) || sched::is_anti_cheat_image(n) || n == "opensweepcore";
        case Action::RestartService:
            for (const auto& s : kProtectedServices)
                if (n == s) return true;
            return sched::is_builtin_protected_image(n) || sched::is_anti_cheat_image(n);
        default: return false;
    }
}

SweepHeal::SweepHeal(journal::Journal& journal, Executor& exec, HealConfig cfg)
    : journal_(journal), exec_(exec), cfg_(cfg) {
    if (cfg_.breaker_threshold < 1) cfg_.breaker_threshold = 1;
}

void SweepHeal::notify(std::int64_t now, std::string kind, std::string detail) {
    if (notes_.size() >= cfg_.max_notifications) notes_.pop_front();
    notes_.push_back({now, std::move(kind), std::move(detail)});
}

std::vector<Notification> SweepHeal::drain_notifications() {
    std::vector<Notification> out(notes_.begin(), notes_.end());
    notes_.clear();
    return out;
}

void SweepHeal::prune_failures(KeyState& k, std::int64_t now) const {
    while (!k.failures.empty() && k.failures.front() <= now - cfg_.breaker_window_ms) k.failures.pop_front();
}

SweepHeal::KeyState& SweepHeal::state_for(const std::string& key, std::int64_t now) {
    auto it = keys_.find(key);
    if (it == keys_.end()) {
        if (keys_.size() >= cfg_.max_keys) {
            // evict the least recently used closed breaker (open ones must be remembered)
            auto victim = keys_.end();
            for (auto i = keys_.begin(); i != keys_.end(); ++i)
                if (i->second.state == BreakerState::Closed &&
                    (victim == keys_.end() || i->second.last_used < victim->second.last_used))
                    victim = i;
            if (victim != keys_.end()) keys_.erase(victim);
        }
        it = keys_.emplace(key, KeyState{}).first;
    }
    it->second.last_used = now;
    return it->second;
}

BreakerStatus SweepHeal::breaker(Action a, const std::string& target) const {
    BreakerStatus s;
    const CatalogueEntry* e = find(a);
    if (!e) return s;
    auto it = keys_.find(std::string(e->id) + "|" + target);
    if (it == keys_.end()) return s;
    s.state = it->second.state;
    s.failures = static_cast<int>(it->second.failures.size());
    s.opened_at = it->second.opened_at;
    return s;
}

bool SweepHeal::any_breaker_open() const {
    for (const auto& [key, k] : keys_)
        if (k.state == BreakerState::Open) return true;
    return false;
}

int SweepHeal::recover(std::int64_t now) {
    std::vector<std::string> ids;
    for (const auto* t : journal_.unfinished(false))
        if (t->kind == "recovery") ids.push_back(t->tx_id);
    for (const auto& id : ids)
        journal_.update(id, now, TxState::Expired, "interrupted", "service restarted mid-recovery; not re-run automatically");
    return static_cast<int>(ids.size());
}

void SweepHeal::tick(std::int64_t now) {
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (it->expires_ms <= now) {
            notify(now, "approval_expired", it->id + ": " + it->target);
            it = pending_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto& [key, k] : keys_) {
        prune_failures(k, now);
        if (k.state == BreakerState::Open && now >= k.opened_at + cfg_.breaker_cooloff_ms) {
            k.state = BreakerState::HalfOpen;
            notify(now, "breaker_half_open", key + ": one trial recovery allowed");
        }
    }
}

RequestResult SweepHeal::request(const RecoveryRequest& req, std::int64_t now) {
    RequestResult r;
    const CatalogueEntry* e = find(req.action);
    if (!e) {
        r.detail = "action not in catalogue";
        return r;
    }
    if (is_protected_target(req.action, req.target)) {
        r.detail = "target is a protected system or security component";
        return r;
    }
    if (e->automatic && e->opensweep_owned && e->risk == Risk::Low) return run(*e, req, now, false, std::string());

    // Approval required: propose only.
    for (const auto& p : pending_) {
        if (p.action == req.action && p.target == req.target) {
            r.status = Status::PendingApproval;
            r.approval_id = p.id;
            r.detail = "already awaiting approval";
            return r;
        }
    }
    if (pending_.size() >= cfg_.max_pending) {
        r.detail = "too many pending approvals";
        return r;
    }
    Approval a;
    a.id = "ap-" + std::to_string(now) + "-" + std::to_string(++approval_seq_);
    a.action = req.action;
    a.target = req.target;
    a.trigger = req.trigger;
    a.evidence = req.evidence;
    if (a.evidence.size() > 16) a.evidence.resize(16);
    a.risk = e->risk;
    a.auth = e->auth;
    a.created_ms = now;
    a.expires_ms = now + cfg_.approval_ttl_ms;
    r.status = Status::PendingApproval;
    r.approval_id = a.id;
    r.detail = std::string(e->description) + " requires approval";
    pending_.push_back(std::move(a));
    return r;
}

RequestResult SweepHeal::approve(const std::string& approval_id, std::int64_t now, bool caller_is_admin) {
    RequestResult r;
    if (!caller_is_admin) {
        r.detail = "approval requires an elevated administrator client";
        return r;
    }
    auto it = std::find_if(pending_.begin(), pending_.end(), [&](const Approval& a) { return a.id == approval_id; });
    if (it == pending_.end()) {
        r.detail = "unknown approval id";
        return r;
    }
    if (it->expires_ms <= now) {
        notify(now, "approval_expired", it->id + ": " + it->target);
        pending_.erase(it);
        r.detail = "approval expired";
        return r;
    }
    RecoveryRequest req;
    req.action = it->action;
    req.target = it->target;
    req.trigger = it->trigger;
    req.evidence = it->evidence;
    pending_.erase(it);
    const CatalogueEntry* e = find(req.action);
    if (!e || is_protected_target(req.action, req.target)) {
        r.detail = "action no longer permitted";
        return r;
    }
    return run(*e, req, now, true, approval_id);
}

bool SweepHeal::reject(const std::string& approval_id) {
    return std::erase_if(pending_, [&](const Approval& a) { return a.id == approval_id; }) > 0;
}

RequestResult SweepHeal::run(const CatalogueEntry& e, const RecoveryRequest& req, std::int64_t now, bool approved,
                             const std::string& approval_id) {
    RequestResult r;
    const std::string key = std::string(e.id) + "|" + req.target;
    KeyState& k = state_for(key, now);
    prune_failures(k, now);
    if (k.state == BreakerState::Open && now >= k.opened_at + cfg_.breaker_cooloff_ms) {
        k.state = BreakerState::HalfOpen;
        notify(now, "breaker_half_open", key + ": one trial recovery allowed");
    }
    if (now < k.cooldown_until) {
        r.status = Status::Cooldown;
        r.detail = "cooling down until " + std::to_string(k.cooldown_until);
        return r;
    }
    if (k.state == BreakerState::Open && !approved) {
        r.status = Status::BreakerOpen;
        r.detail = "automatic recovery suspended after repeated failures";
        return r;
    }
    const int attempt = static_cast<int>(k.failures.size()) + 1;
    if (!approved && k.state != BreakerState::HalfOpen && attempt > e.max_attempts) {
        r.status = Status::MaxAttempts;
        r.detail = "maximum attempts reached";
        return r;
    }

    journal::Transaction tx;
    tx.kind = "recovery";
    tx.trigger = req.trigger.empty() ? std::string(e.description) : req.trigger;
    tx.evidence = req.evidence;
    tx.preconditions.push_back(std::string(e.preconditions));
    tx.preconditions.push_back(std::string("risk=") + to_string(e.risk));
    if (approved) tx.preconditions.push_back("approved via " + approval_id);
    if (k.state == BreakerState::HalfOpen) tx.preconditions.push_back("circuit breaker half-open trial");
    tx.target = req.target;
    tx.action = std::string(e.id);
    tx.required_auth = to_string(e.auth);
    tx.verification = std::string(e.verification);
    tx.rollback = std::string(e.rollback);
    tx.cooldown_until = now + e.cooldown_ms;
    tx.attempt = attempt;
    tx.max_attempts = e.max_attempts;
    const auto id = journal_.begin(std::move(tx), now);  // write-ahead
    if (!id) {
        r.status = Status::JournalUnavailable;
        r.detail = "journal unavailable; recovery not attempted";
        return r;
    }
    r.tx_id = *id;
    k.cooldown_until = now + e.cooldown_ms;

    ExecResult ex;
    try {
        ex = exec_.execute(e.action, req.target);
    } catch (const std::exception& err) {
        ex.ok = false;
        ex.detail = std::string("executor exception: ") + err.what();
    } catch (...) {
        ex.ok = false;
        ex.detail = "executor exception";
    }
    bool verified = false;
    if (ex.ok) {
        journal_.update(*id, now, TxState::Applied, "executed", ex.detail);
        journal_.update(*id, now, TxState::Verifying, "verifying", std::string(e.verification));
        try {
            verified = exec_.verify(e.action, req.target);
        } catch (...) {
            verified = false;
        }
    }
    if (ex.ok && verified) {
        journal_.update(*id, now, TxState::Retained, "verified", "recovery verified",
                        [](journal::Transaction& t) { t.verification_result = "passed"; });
        k.failures.clear();
        if (k.state != BreakerState::Closed) notify(now, "breaker_closed", key + ": recovery succeeded");
        k.state = BreakerState::Closed;
        r.status = Status::Succeeded;
        r.detail = ex.detail;
        return r;
    }
    const std::string why = ex.ok ? "verification failed" : (ex.detail.empty() ? "execution failed" : ex.detail);
    journal_.update(*id, now, TxState::Failed, "failed", why,
                    [&](journal::Transaction& t) { t.verification_result = ex.ok ? "failed" : "not run"; });
    k.failures.push_back(now);
    while (k.failures.size() > static_cast<std::size_t>(cfg_.breaker_threshold) * 2) k.failures.pop_front();
    if (k.state == BreakerState::HalfOpen ||
        (k.state == BreakerState::Closed && k.failures.size() >= static_cast<std::size_t>(cfg_.breaker_threshold))) {
        k.state = BreakerState::Open;
        k.opened_at = now;
        notify(now, "breaker_open",
               key + ": " + std::to_string(k.failures.size()) +
                   " failed recoveries; automatic recovery suspended, user attention needed");
    }
    r.status = Status::Failed;
    r.detail = why;
    return r;
}

}  // namespace opensweep::heal
