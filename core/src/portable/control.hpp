// Adaptive Control Engine (Part 10): turns Recommendations into Decisions and owns every applied
// policy for its whole life (write-ahead journal -> apply -> verify -> retain | rollback).
// Portable: no Windows headers; all changes go through telemetry::Actuator. Owner: engine-act.
//
// Gates, in order (first failing gate is reported as `blocked_by`):
//   invalid_target / workload_not_background   fail-safe re-check of the recommender's verdict
//   not_allow_listed                           per-mode allow-list (balanced: EcoQoS on, memory
//                                              priority 2..4, BELOW_NORMAL; advanced: + CPU Sets)
//   not_opted_in                               the user did not opt into this action kind
//   conflict                                   same target/kind with different values, or a target
//                                              that another recommendation is trying to protect
//   already_active                             a policy of this kind is already applied
//   hysteresis                                 recommendation must persist `hysteresis_ticks`
//   cooldown                                   per target (default 10 min)
//   cost_too_high                              risk x impact x reversibility above the mode limit
//   max_active_policies
//   rate_limit                                 global (default <= 3 actions / 5 min)
//   consent_required                           advanced mode: per-action admin consent
// Monitor mode (and the simulation flag) evaluate exactly the same gates against a separate
// simulated ledger and record the result as `Simulated` with `would_apply` + `blocked_by`;
// the actuator is never called.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "journal.hpp"
#include "policy_types.hpp"
#include "telemetry.hpp"

namespace opensweep::control {

using telemetry::PolicyKind;

enum class Mode { Monitor, Balanced, Advanced };
const char* to_string(Mode m) noexcept;
std::optional<Mode> parse_mode(std::string_view s) noexcept;
std::optional<PolicyKind> parse_policy_kind(std::string_view s) noexcept;

enum class Outcome { Simulated, Applied, Blocked, PendingConsent, Failed };
const char* to_string(Outcome o) noexcept;

struct ControlConfig {
    Mode mode = Mode::Monitor;  // fail-safe default
    bool simulation = false;    // dry run in balanced/advanced
    std::vector<PolicyKind> opted_in;  // user opt-ins (balanced and advanced)
    int hysteresis_ticks = 3;
    std::int64_t cooldown_ms = 10 * 60 * 1000;
    int rate_limit_actions = 3;
    std::int64_t rate_limit_window_ms = 5 * 60 * 1000;
    std::int64_t verify_window_ms = 60 * 1000;
    std::int64_t max_retain_ms = 30 * 60 * 1000;  // retained policies are temporary
    double improve_threshold = 0.10;  // primary metric must improve by >= 10 %
    double worsen_threshold = 0.10;   // any known metric worse by > 10 % (+ noise floor) -> rollback
    double max_cost_balanced = 6.0;
    double max_cost_advanced = 16.0;
    std::size_t max_active = 8;
    std::int64_t consent_ttl_ms = 5 * 60 * 1000;
    std::size_t max_pending_consents = 32;
};

struct Decision {
    policy::Recommendation rec;
    Outcome outcome = Outcome::Blocked;
    bool would_apply = false;
    std::string blocked_by;  // empty when applied
    double cost = 0.0;
    std::string tx_id;
};

struct VerificationOutcome {
    std::string tx_id;
    bool retained = false;
    std::string reason;
};

struct TickResult {
    bool no_action_needed = false;  // first-class "nothing to do"
    std::string reason;
    std::vector<Decision> decisions;
    std::vector<VerificationOutcome> verifications;
};

struct ActivePolicy {
    std::string tx_id;
    policy::Recommendation rec;
    std::int64_t original = 0;
    std::int64_t applied_at = 0;
    std::int64_t verify_until = 0;
    std::int64_t retain_until = 0;
    bool verifying = true;
    policy::VerificationMetrics before;
};

struct PendingConsent {
    std::string id;  // == recommendation id
    policy::Recommendation rec;
    std::int64_t created_ms = 0;
    std::int64_t expires_ms = 0;
};

struct RollbackSummary {
    int restored = 0;
    int failed = 0;
    int expired = 0;  // target gone (PID reuse guard): nothing restored
    void add(const RollbackSummary& o) noexcept {
        restored += o.restored;
        failed += o.failed;
        expired += o.expired;
    }
};

struct ModeChange {
    bool ok = false;  // false: refused (escalation while the journal is unavailable)
    RollbackSummary rolled_back;
};

// risk (1..5) x impact (kind) x reversibility (kind)
double intervention_cost(const policy::Recommendation& r) noexcept;
// Value restored when the original is unknown (crash between intent and applied).
std::int64_t default_value(PolicyKind k) noexcept;

class ControlEngine {
public:
    ControlEngine(telemetry::Actuator& act, journal::Journal& journal, ControlConfig cfg = {});

    // Start-up crash recovery: roll back everything the journal says may still be applied.
    RollbackSummary recover(std::int64_t now_ms);

    // One engine tick: verification of active policies, then decisions for `recs`.
    TickResult step(std::int64_t now_ms, std::span<const policy::Recommendation> recs,
                    const policy::VerificationMetrics& current);

    // Admin operations (the IPC layer checks the caller is an elevated administrator).
    // Journaled. -> monitor rolls everything back; advanced -> balanced rolls back CPU Sets.
    // Escalating out of monitor is refused when the journal cannot record it (fail-safe).
    // Journals an admin configuration change (e.g. the game boost switch) as a retained config transaction.
    bool record_config(std::int64_t now, std::string_view action, std::int64_t value, std::int64_t original,
                       std::string_view detail) {
        return journal_config(now, action, value, original, detail);
    }
    ModeChange set_mode(Mode m, std::int64_t now_ms);
    void set_simulation(bool on) noexcept { cfg_.simulation = on; }
    // Journaled. Opting out rolls back active policies of that kind.
    RollbackSummary set_opt_in(PolicyKind k, bool on, std::int64_t now_ms);
    bool approve(const std::string& rec_id, const std::string& consent_id, std::int64_t now_ms);
    RollbackSummary rollback_all(std::int64_t now_ms, std::string_view reason);
    RollbackSummary shutdown(std::int64_t now_ms) { return rollback_all(now_ms, "service_stop"); }
    bool disable_policy(const std::string& tx_id, std::int64_t now_ms);

    Mode mode() const noexcept { return cfg_.mode; }
    bool simulating() const noexcept { return cfg_.mode == Mode::Monitor || cfg_.simulation; }
    const ControlConfig& config() const noexcept { return cfg_; }
    const std::vector<ActivePolicy>& active() const noexcept { return active_; }
    const std::vector<PendingConsent>& pending_consents() const noexcept { return pending_; }
    std::vector<std::uint32_t> active_pids(PolicyKind k) const;

private:
    struct Ledger {
        std::unordered_map<std::string, std::int64_t> cooldown_until;  // per target
        std::deque<std::int64_t> applies;                               // rate-limit window
        void prune(std::int64_t now, std::int64_t window);
    };
    struct Consent {
        std::string consent_id;
        std::int64_t expires_ms = 0;
    };

    bool allow_listed(Mode m, const policy::Recommendation& r) const;
    bool opted_in(PolicyKind k) const;
    std::optional<std::string> gate(const policy::Recommendation& r, std::int64_t now, Mode effective, double cost,
                                    bool conflicted, const Ledger& ledger, bool simulated) const;
    Decision apply(const policy::Recommendation& r, double cost, std::int64_t now,
                   const policy::VerificationMetrics& current, const std::string& consent_id);
    bool rollback(ActivePolicy& ap, std::int64_t now, std::string_view reason, RollbackSummary& sum);
    bool improved(const ActivePolicy& ap, const policy::VerificationMetrics& after, std::string& why) const;
    void verify_active(std::int64_t now, const policy::VerificationMetrics& current, TickResult& out);
    bool journal_config(std::int64_t now, std::string_view action, std::int64_t value, std::int64_t original,
                        std::string_view detail);
    RollbackSummary rollback_where(std::int64_t now, std::string_view reason,
                                   const std::function<bool(const ActivePolicy&)>& pred);

    telemetry::Actuator& act_;
    journal::Journal& journal_;
    ControlConfig cfg_;
    std::vector<ActivePolicy> active_;
    std::vector<PendingConsent> pending_;
    std::unordered_map<std::string, Consent> consents_;
    std::unordered_map<std::string, int> persist_;  // hysteresis counters, key = target + kind
    Ledger real_;
    Ledger sim_;
};

std::string target_key(const telemetry::PolicyTarget& t);

}  // namespace opensweep::control
