#include <stdexcept>
#include <string>
#include <vector>

#include "heal.hpp"
#include "test_framework.hpp"

using namespace opensweep;
using heal::Action;
using heal::BreakerState;
using heal::Status;
using journal::TxState;

namespace {

constexpr std::int64_t kMin = 60 * 1000;

struct FakeExecutor : heal::Executor {
    int executed = 0;
    int verified = 0;
    bool ok = true;
    bool verify_ok = true;
    bool throw_exec = false;
    std::vector<std::pair<Action, std::string>> calls;
    heal::ExecResult execute(Action a, const std::string& target) override {
        ++executed;
        calls.emplace_back(a, target);
        if (throw_exec) throw std::runtime_error("exec boom");
        return {ok, ok ? "done" : "failed to start"};
    }
    bool verify(Action, const std::string&) override {
        ++verified;
        return verify_ok;
    }
};

struct Rig {
    FakeExecutor exec;
    journal::MemoryFileIO io;
    journal::Journal j{io, "journal.jsonl"};
    heal::SweepHeal h{j, exec};
    std::int64_t now = 10'000'000;
};

heal::RecoveryRequest req(Action a, const char* target, const char* trigger = "collector stalled") {
    heal::RecoveryRequest r;
    r.action = a;
    r.target = target;
    r.trigger = trigger;
    r.evidence = {"no sample for 30 s"};
    return r;
}

}  // namespace

TEST(heal_catalogue_shape) {
    int automatic = 0;
    for (const auto& e : heal::catalogue()) {
        CHECK(!e.id.empty());
        CHECK(!e.verification.empty());
        CHECK(!e.rollback.empty());
        CHECK(!e.preconditions.empty());
        CHECK(e.max_attempts >= 1);
        CHECK(e.cooldown_ms > 0);
        if (e.automatic) {
            ++automatic;
            CHECK(e.opensweep_owned);  // automatic only for OpenSweep-owned, low-risk actions
            CHECK(e.risk == heal::Risk::Low);
            CHECK(e.auth == heal::Auth::None);
        } else {
            CHECK(e.auth != heal::Auth::None);
        }
        CHECK(heal::find(e.action) == &e);
        CHECK(heal::find(e.id) == &e);
        CHECK(!heal::is_forbidden(e.id));
    }
    CHECK_EQ(automatic, 5);
    for (Action a : {Action::RestartExplorer, Action::RestartService, Action::ResetNetworkStack, Action::RunSfc,
                     Action::RunDismRestoreHealth, Action::TerminateApplication})
        CHECK(heal::find(a) && !heal::find(a)->automatic);
}

TEST(heal_forbidden_actions_absent) {
    CHECK(!heal::forbidden_actions().empty());
    for (auto f : heal::forbidden_actions()) {
        CHECK(heal::is_forbidden(f));
        CHECK(heal::find(f) == nullptr);
        for (const auto& e : heal::catalogue()) CHECK(e.id != f);
    }
    for (auto word : {"disable_defender", "uninstall_driver", "bcdedit", "alter_boot", "disable_firewall",
                      "anything_with_driver", "set_boot_options"})
        CHECK(heal::is_forbidden(word));
    // nothing in the catalogue touches security, drivers or boot configuration
    for (const auto& e : heal::catalogue())
        for (auto token : {"security", "defender", "driver", "boot", "bcd", "firewall"})
            CHECK(e.id.find(token) == std::string_view::npos);
}

TEST(heal_automatic_runs_owned_low_risk) {
    Rig rig;
    auto r = rig.h.request(req(Action::RestartCollector, "process_collector"), rig.now);
    CHECK(r.status == Status::Succeeded);
    CHECK_EQ(rig.exec.executed, 1);
    CHECK_EQ(rig.exec.verified, 1);
    const auto* tx = rig.j.find(r.tx_id);
    CHECK(tx != nullptr);
    CHECK_EQ(tx->kind, std::string("recovery"));
    CHECK(tx->state == TxState::Retained);
    CHECK_EQ(tx->action, std::string("restart_collector"));
    CHECK_EQ(tx->required_auth, std::string("none"));
    CHECK(!tx->preconditions.empty());
    CHECK(!tx->verification.empty());
    CHECK(!tx->rollback.empty());
    CHECK(tx->cooldown_until > rig.now);
    CHECK_EQ(tx->max_attempts, 3);
    CHECK_EQ(tx->verification_result, std::string("passed"));
    CHECK(tx->audit.size() >= 3u);  // intent, executed, verifying, verified
}

TEST(heal_high_impact_requires_approval) {
    Rig rig;
    for (auto [a, target] : std::vector<std::pair<Action, const char*>>{
             {Action::RestartExplorer, "explorer.exe"},
             {Action::RestartService, "VendorUpdater"},
             {Action::ResetNetworkStack, "network"},
             {Action::RunSfc, "system"},
             {Action::RunDismRestoreHealth, "system"},
             {Action::TerminateApplication, "hungapp.exe"}}) {
        auto r = rig.h.request(req(a, target, "evidence"), rig.now);
        CHECK(r.status == Status::PendingApproval);
        CHECK(!r.approval_id.empty());
        CHECK(r.tx_id.empty());
    }
    CHECK_EQ(rig.exec.executed, 0);  // only proposed
    CHECK_EQ(rig.h.pending().size(), 6u);
    CHECK_EQ(rig.j.size(), 0u);
    for (const auto& p : rig.h.pending()) {
        CHECK(!p.evidence.empty());
        CHECK(p.expires_ms > rig.now);
    }
    // duplicate proposals are merged
    auto again = rig.h.request(req(Action::RunSfc, "system"), rig.now);
    CHECK_EQ(again.approval_id, rig.h.pending()[3].id);
    CHECK_EQ(rig.h.pending().size(), 6u);
}

TEST(heal_approve_executes_only_for_admin) {
    Rig rig;
    auto r = rig.h.request(req(Action::RestartExplorer, "explorer.exe"), rig.now);
    auto denied = rig.h.approve(r.approval_id, rig.now, /*caller_is_admin=*/false);
    CHECK(denied.status == Status::Rejected);
    CHECK_EQ(rig.exec.executed, 0);
    CHECK(rig.h.approve("ap-unknown", rig.now, true).status == Status::Rejected);
    auto ok = rig.h.approve(r.approval_id, rig.now + 1000, true);
    CHECK(ok.status == Status::Succeeded);
    CHECK_EQ(rig.exec.executed, 1);
    CHECK(rig.exec.calls[0].first == Action::RestartExplorer);
    CHECK(rig.h.pending().empty());
    const auto* tx = rig.j.find(ok.tx_id);
    CHECK_EQ(tx->required_auth, std::string("user"));
    bool approved_pre = false;
    for (const auto& p : tx->preconditions)
        if (p.find(r.approval_id) != std::string::npos) approved_pre = true;
    CHECK(approved_pre);
    // an approval cannot be used twice
    CHECK(rig.h.approve(r.approval_id, rig.now + 2000, true).status == Status::Rejected);
}

TEST(heal_approval_expiry_and_reject) {
    Rig rig;
    auto r = rig.h.request(req(Action::RunSfc, "system"), rig.now);
    auto r2 = rig.h.request(req(Action::RunDismRestoreHealth, "system"), rig.now);
    CHECK(rig.h.reject(r2.approval_id));
    CHECK(!rig.h.reject(r2.approval_id));
    rig.h.tick(rig.now + 31 * kMin);
    CHECK(rig.h.pending().empty());
    CHECK(rig.h.approve(r.approval_id, rig.now + 31 * kMin, true).status == Status::Rejected);
    CHECK_EQ(rig.exec.executed, 0);
    auto notes = rig.h.drain_notifications();
    CHECK_EQ(notes.size(), 1u);
    CHECK_EQ(notes[0].kind, std::string("approval_expired"));
    // expiry is also enforced at approve() time without a tick
    auto r3 = rig.h.request(req(Action::RunSfc, "system"), rig.now);
    CHECK(rig.h.approve(r3.approval_id, rig.now + 31 * kMin, true).status == Status::Rejected);
}

TEST(heal_protected_targets_refused) {
    Rig rig;
    for (auto t : {"csrss.exe", "MsMpEng.exe", "lsass.exe", "EasyAntiCheat.exe", "OpenSweepCore.exe", ""})
        CHECK(rig.h.request(req(Action::TerminateApplication, t), rig.now).status == Status::Rejected);
    for (auto t : {"WinDefend", "mpssvc", "wscsvc", "EventLog", "Sense"})
        CHECK(rig.h.request(req(Action::RestartService, t), rig.now).status == Status::Rejected);
    CHECK(rig.h.pending().empty());
    CHECK(rig.h.request(req(Action::RestartService, "VendorUpdater"), rig.now).status == Status::PendingApproval);
}

TEST(heal_circuit_breaker_open_half_open_close) {
    Rig rig;
    rig.exec.ok = false;
    std::int64_t t = rig.now;
    const auto R = req(Action::RestartWorker, "sampler");
    for (int i = 0; i < 3; ++i) {
        auto r = rig.h.request(R, t);
        CHECK(r.status == Status::Failed);
        CHECK(rig.j.find(r.tx_id)->state == TxState::Failed);
        CHECK_EQ(rig.j.find(r.tx_id)->attempt, i + 1);
        t += 31 * 1000;  // past the 30 s cooldown
    }
    auto b = rig.h.breaker(Action::RestartWorker, "sampler");
    CHECK(b.state == BreakerState::Open);
    CHECK_EQ(b.failures, 3);
    CHECK(rig.h.any_breaker_open());
    auto notes = rig.h.drain_notifications();
    CHECK(!notes.empty());
    CHECK_EQ(notes.back().kind, std::string("breaker_open"));
    // suspended: no further automatic attempts
    CHECK(rig.h.request(R, t).status == Status::BreakerOpen);
    CHECK_EQ(rig.exec.executed, 3);
    // other targets are unaffected
    rig.exec.ok = true;
    CHECK(rig.h.request(req(Action::RestartWorker, "other"), t).status == Status::Succeeded);
    // half-open after the cool-off: one trial; failure re-opens
    rig.exec.ok = false;
    t += 15 * kMin;
    rig.h.tick(t);
    CHECK(rig.h.breaker(Action::RestartWorker, "sampler").state == BreakerState::HalfOpen);
    CHECK(rig.h.request(R, t).status == Status::Failed);
    CHECK(rig.h.breaker(Action::RestartWorker, "sampler").state == BreakerState::Open);
    // next half-open trial succeeds -> closed, failures cleared
    rig.exec.ok = true;
    t += 15 * kMin;
    auto ok = rig.h.request(R, t);  // run() itself moves Open -> HalfOpen after the cool-off
    CHECK(ok.status == Status::Succeeded);
    b = rig.h.breaker(Action::RestartWorker, "sampler");
    CHECK(b.state == BreakerState::Closed);
    CHECK_EQ(b.failures, 0);
    CHECK(!rig.h.any_breaker_open());
    bool closed_note = false;
    for (const auto& n : rig.h.drain_notifications())
        if (n.kind == "breaker_closed") closed_note = true;
    CHECK(closed_note);
}

TEST(heal_failures_outside_window_do_not_open_breaker) {
    Rig rig;
    rig.exec.ok = false;
    std::int64_t t = rig.now;
    for (int i = 0; i < 6; ++i) {
        rig.h.request(req(Action::ReconnectEventSubscription, "system_log"), t);
        t += 16 * kMin;  // only ~2 failures inside any 30 min window
    }
    CHECK(rig.h.breaker(Action::ReconnectEventSubscription, "system_log").state == BreakerState::Closed);
    CHECK_EQ(rig.exec.executed, 6);
}

TEST(heal_cooldown_and_verification_failure) {
    Rig rig;
    auto r = rig.h.request(req(Action::ClearInternalCache, "baselines"), rig.now);
    CHECK(r.status == Status::Succeeded);
    CHECK(rig.h.request(req(Action::ClearInternalCache, "baselines"), rig.now + 1000).status == Status::Cooldown);
    rig.exec.verify_ok = false;
    auto v = rig.h.request(req(Action::ClearInternalCache, "baselines"), rig.now + 6 * kMin);
    CHECK(v.status == Status::Failed);
    CHECK(rig.j.find(v.tx_id)->state == TxState::Failed);
    CHECK_EQ(rig.j.find(v.tx_id)->verification_result, std::string("failed"));
}

TEST(heal_executor_exception_and_journal_failure) {
    Rig rig;
    rig.exec.throw_exec = true;
    auto r = rig.h.request(req(Action::RestoreOwnedPolicies, "control"), rig.now);
    CHECK(r.status == Status::Failed);
    CHECK(rig.j.find(r.tx_id)->state == TxState::Failed);
    Rig rig2;
    rig2.io.fail_append = true;
    auto r2 = rig2.h.request(req(Action::RestartCollector, "c"), rig2.now);
    CHECK(r2.status == Status::JournalUnavailable);
    CHECK_EQ(rig2.exec.executed, 0);  // write-ahead: nothing runs without an intent record
}

TEST(heal_crash_recovery_marks_interrupted_expired) {
    journal::MemoryFileIO io;
    std::string id;
    {
        journal::Journal j(io, "journal.jsonl");
        journal::Transaction t;
        t.kind = "recovery";
        t.action = "restart_collector";
        t.target = "c";
        id = *j.begin(t, 1);
        journal::Transaction p;
        p.kind = "policy";
        p.action = "ecoqos";
        j.begin(p, 2);
    }
    journal::Journal j2(io, "journal.jsonl");
    j2.load();
    FakeExecutor exec;
    heal::SweepHeal h(j2, exec);
    CHECK_EQ(h.recover(100), 1);
    CHECK(j2.find(id)->state == TxState::Expired);
    CHECK_EQ(exec.executed, 0);              // never re-run automatically
    CHECK_EQ(j2.unfinished().size(), 1u);    // the policy one is left for Control
}

TEST(heal_bounded_state) {
    Rig rig;
    heal::HealConfig cfg;
    cfg.max_pending = 4;
    cfg.max_keys = 8;
    heal::SweepHeal h(rig.j, rig.exec, cfg);
    for (int i = 0; i < 10; ++i) h.request(req(Action::RestartService, ("svc" + std::to_string(i)).c_str()), rig.now);
    CHECK_EQ(h.pending().size(), 4u);
    for (int i = 0; i < 50; ++i) h.request(req(Action::RestartCollector, ("c" + std::to_string(i)).c_str()), rig.now);
    CHECK_EQ(rig.exec.executed, 50);
}
