// SweepHeal (Part 8): transactional self-healing with a fixed recovery catalogue, user approval for
// anything that is not an OpenSweep-owned low-risk action, and a circuit breaker.
// Portable: no Windows headers; the actual work is done by an injected Executor. Owner: engine-act.
//
// Automatic (no approval): restart an OpenSweep collector / worker, reconnect an OpenSweep event
//   subscription, clear a bounded OpenSweep cache, restore OpenSweep-owned temporary policies.
// Proposed only (pending approval with id, evidence, risk, expiry; executed after approve() from an
//   admin client): restart Explorer, restart a third-party service, reset the network stack, run
//   SFC, run DISM /RestoreHealth, terminate an application.
// Never (not in the catalogue, refused by name): disable security protections, uninstall drivers,
//   alter boot configuration.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "journal.hpp"

namespace opensweep::heal {

enum class Action {
    RestartCollector,
    RestartWorker,
    ReconnectEventSubscription,
    ClearInternalCache,
    RestoreOwnedPolicies,
    RestartExplorer,
    RestartService,
    ResetNetworkStack,
    RunSfc,
    RunDismRestoreHealth,
    TerminateApplication,
};

enum class Risk { Low, Medium, High };
enum class Auth { None, User, Admin };
const char* to_string(Risk r) noexcept;
const char* to_string(Auth a) noexcept;

struct CatalogueEntry {
    Action action;
    std::string_view id;
    std::string_view description;
    Risk risk;
    Auth auth;
    bool automatic;        // may run without approval (OpenSweep-owned + low risk only)
    bool opensweep_owned;
    int max_attempts;      // consecutive failures tolerated before refusing (per action/target)
    std::int64_t cooldown_ms;
    std::string_view preconditions;
    std::string_view verification;
    std::string_view rollback;
};

std::span<const CatalogueEntry> catalogue() noexcept;
const CatalogueEntry* find(Action a) noexcept;
const CatalogueEntry* find(std::string_view id) noexcept;
// Action ids/keywords that must never be executed (security, drivers, boot configuration).
std::span<const std::string_view> forbidden_actions() noexcept;
bool is_forbidden(std::string_view id) noexcept;
// Targets a third-party restart/terminate may never touch (system/security processes & services).
bool is_protected_target(Action a, std::string_view target);

struct ExecResult {
    bool ok = false;
    std::string detail;
};

class Executor {
public:
    virtual ~Executor() = default;
    virtual ExecResult execute(Action a, const std::string& target) = 0;
    // Verification procedure after a successful execute (e.g. collector producing samples again).
    virtual bool verify(Action a, const std::string& target) = 0;
};

struct HealConfig {
    int breaker_threshold = 3;
    std::int64_t breaker_window_ms = 30 * 60 * 1000;
    std::int64_t breaker_cooloff_ms = 15 * 60 * 1000;
    std::int64_t approval_ttl_ms = 30 * 60 * 1000;
    std::size_t max_pending = 16;
    std::size_t max_keys = 64;
    std::size_t max_notifications = 64;
};

struct RecoveryRequest {
    Action action = Action::RestartCollector;
    std::string target;  // component / service / image name
    std::string trigger;
    std::vector<std::string> evidence;
};

enum class Status { Succeeded, Failed, PendingApproval, BreakerOpen, Cooldown, MaxAttempts, Rejected, JournalUnavailable };
const char* to_string(Status s) noexcept;

struct RequestResult {
    Status status = Status::Rejected;
    std::string tx_id;
    std::string approval_id;
    std::string detail;
};

struct Approval {
    std::string id;
    Action action = Action::RestartExplorer;
    std::string target;
    std::string trigger;
    std::vector<std::string> evidence;
    Risk risk = Risk::High;
    Auth auth = Auth::Admin;
    std::int64_t created_ms = 0;
    std::int64_t expires_ms = 0;
};

enum class BreakerState { Closed, Open, HalfOpen };
const char* to_string(BreakerState s) noexcept;

struct BreakerStatus {
    BreakerState state = BreakerState::Closed;
    int failures = 0;  // within the window
    std::int64_t opened_at = 0;
};

struct Notification {
    std::int64_t ts = 0;
    std::string kind;  // "breaker_open" | "breaker_half_open" | "breaker_closed" | "approval_expired"
    std::string detail;
};

class SweepHeal {
public:
    SweepHeal(journal::Journal& journal, Executor& exec, HealConfig cfg = {});

    // Start-up: recovery transactions interrupted by a crash are marked expired (never re-run).
    int recover(std::int64_t now_ms);

    // Automatic actions run now; approval-required actions become a pending Approval.
    RequestResult request(const RecoveryRequest& req, std::int64_t now_ms);
    // Executes a pending approval. `caller_is_admin` must come from the IPC layer's token check.
    RequestResult approve(const std::string& approval_id, std::int64_t now_ms, bool caller_is_admin);
    bool reject(const std::string& approval_id);

    // Expires approvals and moves open breakers to half-open after the cool-off.
    void tick(std::int64_t now_ms);

    const std::vector<Approval>& pending() const noexcept { return pending_; }
    BreakerStatus breaker(Action a, const std::string& target) const;
    bool any_breaker_open() const;
    std::vector<Notification> drain_notifications();
    const std::deque<Notification>& notifications() const noexcept { return notes_; }

private:
    struct KeyState {
        BreakerState state = BreakerState::Closed;
        std::deque<std::int64_t> failures;
        std::int64_t opened_at = 0;
        std::int64_t cooldown_until = 0;
        std::int64_t last_used = 0;
    };
    RequestResult run(const CatalogueEntry& e, const RecoveryRequest& req, std::int64_t now, bool approved,
                      const std::string& approval_id);
    KeyState& state_for(const std::string& key, std::int64_t now);
    void notify(std::int64_t now, std::string kind, std::string detail);
    void prune_failures(KeyState& k, std::int64_t now) const;

    journal::Journal& journal_;
    Executor& exec_;
    HealConfig cfg_;
    std::vector<Approval> pending_;
    std::map<std::string, KeyState> keys_;
    std::deque<Notification> notes_;
    std::uint64_t approval_seq_ = 0;
};

}  // namespace opensweep::heal
