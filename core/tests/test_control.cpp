#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "control.hpp"
#include "test_framework.hpp"

using namespace opensweep;
using control::ControlConfig;
using control::ControlEngine;
using control::Mode;
using control::Outcome;
using journal::TxState;
using telemetry::PolicyKind;

namespace {

constexpr std::int64_t kTick = 2000;

struct FakeActuator : telemetry::Actuator {
    int applies = 0;
    int restores = 0;
    bool throw_on_apply = false;
    bool fail_apply = false;
    bool fail_restore = false;
    std::map<std::uint32_t, std::int64_t> start;  // pid -> start time of the live process
    std::vector<std::pair<std::uint32_t, std::int64_t>> restored;  // (pid, value)
    std::function<void()> on_apply;

    telemetry::ApplyResult apply(PolicyKind, const telemetry::PolicyTarget&, std::int64_t) override {
        ++applies;
        if (on_apply) on_apply();
        if (throw_on_apply) throw std::runtime_error("boom");
        telemetry::ApplyResult r;
        r.ok = !fail_apply;
        r.original = 0;
        if (fail_apply) r.error = "access denied";
        return r;
    }
    telemetry::ApplyResult restore(PolicyKind, const telemetry::PolicyTarget& t, std::int64_t original) override {
        ++restores;
        restored.emplace_back(t.pid, original);
        telemetry::ApplyResult r;
        r.ok = !fail_restore;
        if (fail_restore) r.error = "gone";
        return r;
    }
    bool alive(const telemetry::PolicyTarget& t) override {
        auto it = start.find(t.pid);
        return it != start.end() && (t.process_start_ms == 0 || it->second == t.process_start_ms);
    }
};

policy::Recommendation rec(std::uint32_t pid, PolicyKind kind = PolicyKind::EcoQoS, std::int64_t value = 1,
                           policy::Area area = policy::Area::Cpu) {
    policy::Recommendation r;
    r.area = area;
    r.target.pid = pid;
    r.target.name = "onedrive.exe";
    r.target.process_start_ms = 1000 + pid;
    r.workload = telemetry::WorkloadClass::Background;
    r.kind = kind;
    r.value = value;
    r.beneficiary_pid = 100;
    r.risk = 1;
    r.id = std::string(policy::to_string(area)) + ":" + telemetry::to_string(kind) + ":" + std::to_string(pid);
    r.reason = "test";
    r.evidence = {"cpu 95%"};
    return r;
}

policy::VerificationMetrics metrics(double fg, double queue = 2.0, double hf = 100.0, double commit = 50.0) {
    policy::VerificationMetrics m;
    m.fg_latency_ms = fg;
    m.cpu_queue = queue;
    m.hard_faults = hf;
    m.commit_percent = commit;
    return m;
}

struct Rig {
    FakeActuator act;
    journal::MemoryFileIO io;
    journal::Journal j{io, "journal.jsonl"};
    ControlEngine eng;
    std::int64_t now = 1'000'000;

    explicit Rig(ControlConfig cfg = balanced()) : eng(act, j, cfg) {
        for (std::uint32_t pid = 200; pid < 220; ++pid) act.start[pid] = 1000 + pid;
    }
    static ControlConfig balanced() {
        ControlConfig c;
        c.mode = Mode::Balanced;
        c.opted_in = {PolicyKind::EcoQoS, PolicyKind::MemoryPriority, PolicyKind::PriorityClass};
        return c;
    }
    control::TickResult step(const std::vector<policy::Recommendation>& recs,
                             const policy::VerificationMetrics& m = metrics(100.0)) {
        now += kTick;
        return eng.step(now, recs, m);
    }
    // Repeat until the hysteresis gate passes; returns the last result.
    control::TickResult settle(const std::vector<policy::Recommendation>& recs,
                               const policy::VerificationMetrics& m = metrics(100.0)) {
        control::TickResult r;
        for (int i = 0; i < 3; ++i) r = step(recs, m);
        return r;
    }
};

const control::Decision* find_decision(const control::TickResult& r, std::uint32_t pid, PolicyKind k) {
    for (const auto& d : r.decisions)
        if (d.rec.target.pid == pid && d.rec.kind == k) return &d;
    return nullptr;
}

}  // namespace

TEST(control_defaults_are_fail_safe) {
    ControlConfig c;
    CHECK(c.mode == Mode::Monitor);
    CHECK(c.opted_in.empty());
    CHECK_EQ(c.cooldown_ms, 600000);
    CHECK_EQ(c.rate_limit_actions, 3);
    CHECK_EQ(c.rate_limit_window_ms, 300000);
    CHECK_EQ(c.verify_window_ms, 60000);
    CHECK(control::parse_mode("advanced") == Mode::Advanced);
    CHECK(!control::parse_mode("turbo"));
    CHECK(control::parse_policy_kind("memory_priority") == PolicyKind::MemoryPriority);
}

TEST(control_no_action_needed_is_first_class) {
    Rig rig;
    auto r = rig.step({});
    CHECK(r.no_action_needed);
    CHECK(r.decisions.empty());
    CHECK(!r.reason.empty());
    CHECK_EQ(rig.act.applies, 0);
}

TEST(control_monitor_never_calls_actuator) {
    ControlConfig c;  // monitor
    Rig rig(c);
    control::TickResult r;
    for (int i = 0; i < 50; ++i)
        r = rig.step({rec(200), rec(201, PolicyKind::PriorityClass, policy::kPriorityBelowNormal),
                      rec(202, PolicyKind::CpuSets, 4)});
    CHECK_EQ(rig.act.applies, 0);
    CHECK_EQ(rig.act.restores, 0);
    CHECK_EQ(rig.j.size(), 0u);  // nothing journaled as a policy either
    CHECK(rig.eng.active().empty());
    for (const auto& d : r.decisions) CHECK(d.outcome == Outcome::Simulated);
    // CPU sets would not be allowed outside advanced
    CHECK_EQ(find_decision(r, 202, PolicyKind::CpuSets)->blocked_by, std::string("not_allow_listed"));
}

TEST(control_monitor_simulation_reports_would_apply) {
    ControlConfig c;
    Rig rig(c);
    auto r1 = rig.step({rec(200)});
    CHECK(!r1.decisions[0].would_apply);
    CHECK_EQ(r1.decisions[0].blocked_by, std::string("hysteresis"));
    rig.step({rec(200)});
    auto r3 = rig.step({rec(200)});
    CHECK(r3.decisions[0].would_apply);
    CHECK_EQ(r3.decisions[0].blocked_by, std::string("monitor_mode"));
    CHECK(r3.reason.find("nothing changed") != std::string::npos);
    // the simulated ledger applies cooldowns too, so the UI does not show it every tick
    auto r4 = rig.step({rec(200)});
    CHECK(!r4.decisions[0].would_apply);
    CHECK_EQ(r4.decisions[0].blocked_by, std::string("cooldown"));
    CHECK_EQ(rig.act.applies, 0);
}

TEST(control_simulation_flag_in_balanced) {
    Rig rig;
    rig.eng.set_simulation(true);
    auto r = rig.settle({rec(200)});
    CHECK(r.decisions[0].outcome == Outcome::Simulated);
    CHECK(r.decisions[0].would_apply);
    CHECK_EQ(r.decisions[0].blocked_by, std::string("simulation"));
    CHECK_EQ(rig.act.applies, 0);
}

TEST(control_balanced_allow_list_and_opt_in) {
    ControlConfig c = Rig::balanced();
    c.opted_in = {PolicyKind::EcoQoS};
    Rig rig(c);
    auto bad_prio = rec(203, PolicyKind::PriorityClass, policy::kPriorityIdle);
    auto mem_too_low = rec(204, PolicyKind::MemoryPriority, 1);
    auto r = rig.settle({rec(200), rec(201, PolicyKind::MemoryPriority, 2), rec(202, PolicyKind::CpuSets, 4), bad_prio,
                         mem_too_low});
    CHECK(find_decision(r, 200, PolicyKind::EcoQoS)->outcome == Outcome::Applied);
    CHECK_EQ(find_decision(r, 201, PolicyKind::MemoryPriority)->blocked_by, std::string("not_opted_in"));
    CHECK_EQ(find_decision(r, 202, PolicyKind::CpuSets)->blocked_by, std::string("not_allow_listed"));
    CHECK_EQ(find_decision(r, 203, PolicyKind::PriorityClass)->blocked_by, std::string("not_allow_listed"));
    CHECK_EQ(find_decision(r, 204, PolicyKind::MemoryPriority)->blocked_by, std::string("not_allow_listed"));
    CHECK_EQ(rig.act.applies, 1);
}

TEST(control_fail_safe_rechecks_workload) {
    Rig rig;
    auto unknown = rec(200);
    unknown.workload = telemetry::WorkloadClass::Unknown;
    auto inter = rec(201);
    inter.workload = telemetry::WorkloadClass::Interactive;
    auto sys = rec(4);
    auto r = rig.settle({unknown, inter, sys});
    CHECK_EQ(find_decision(r, 200, PolicyKind::EcoQoS)->blocked_by, std::string("workload_not_background"));
    CHECK_EQ(find_decision(r, 201, PolicyKind::EcoQoS)->blocked_by, std::string("workload_not_background"));
    CHECK_EQ(find_decision(r, 4, PolicyKind::EcoQoS)->blocked_by, std::string("invalid_target"));
    CHECK_EQ(rig.act.applies, 0);
}

TEST(control_hysteresis) {
    Rig rig;
    auto r = rig.step({rec(200)});
    CHECK_EQ(r.decisions[0].blocked_by, std::string("hysteresis"));
    r = rig.step({rec(200)});
    CHECK_EQ(r.decisions[0].blocked_by, std::string("hysteresis"));
    rig.step({});  // condition disappeared: counter resets
    r = rig.step({rec(200)});
    CHECK_EQ(r.decisions[0].blocked_by, std::string("hysteresis"));
    rig.step({rec(200)});
    r = rig.step({rec(200)});
    CHECK(r.decisions[0].outcome == Outcome::Applied);
    CHECK_EQ(rig.act.applies, 1);
}

TEST(control_write_ahead_and_full_lifecycle_retain) {
    Rig rig;
    bool intent_seen_before_apply = false;
    rig.act.on_apply = [&] {
        intent_seen_before_apply = rig.io.files["journal.jsonl"].find("\"state\":\"intent\"") != std::string::npos;
    };
    auto r = rig.settle({rec(200)}, metrics(100.0));
    const auto* d = find_decision(r, 200, PolicyKind::EcoQoS);
    CHECK(d->outcome == Outcome::Applied);
    CHECK(intent_seen_before_apply);
    CHECK_EQ(rig.eng.active().size(), 1u);
    const journal::Transaction* tx = rig.j.find(d->tx_id);
    CHECK(tx && tx->state == TxState::Verifying);
    CHECK(tx->has_original);
    CHECK_EQ(tx->target_start_ms, 1200);
    CHECK_EQ(tx->required_auth, std::string("user"));
    // verification window: foreground latency improved 100 -> 40 ms
    for (int i = 0; i < 30; ++i) rig.step({}, metrics(40.0));
    CHECK(rig.j.find(d->tx_id)->state == TxState::Retained);
    CHECK_EQ(rig.act.restores, 0);
    // retention is temporary: after max_retain the policy is restored
    for (int i = 0; i < 31 * 30; ++i) rig.step({}, metrics(40.0));
    CHECK(rig.j.find(d->tx_id)->state == TxState::RolledBack);
    CHECK_EQ(rig.act.restores, 1);
    CHECK(rig.eng.active().empty());
}

TEST(control_verification_rollback_when_no_improvement_or_worse) {
    {
        Rig rig;
        auto r = rig.settle({rec(200)}, metrics(100.0));
        const std::string tx = r.decisions[0].tx_id;
        control::TickResult v;
        for (int i = 0; i < 31; ++i) {
            v = rig.step({}, metrics(98.0));  // < 10 % better: not enough
            if (!v.verifications.empty()) break;
        }
        CHECK_EQ(v.verifications.size(), 1u);
        CHECK(!v.verifications[0].retained);
        CHECK(rig.j.find(tx)->state == TxState::RolledBack);
        CHECK_EQ(rig.act.restores, 1);
    }
    {
        Rig rig;
        auto r = rig.settle({rec(200)}, metrics(100.0, 2.0, 100.0));
        const std::string tx = r.decisions[0].tx_id;
        for (int i = 0; i < 31; ++i) rig.step({}, metrics(50.0, 2.0, 400.0));  // fg better, hard faults much worse
        CHECK(rig.j.find(tx)->state == TxState::RolledBack);
        CHECK(rig.j.find(tx)->verification_result.find("worsened") != std::string::npos);
    }
    {
        Rig rig;  // unknown metrics: no evidence -> rollback (fail-safe)
        policy::VerificationMetrics unknown;
        auto r = rig.settle({rec(200)}, unknown);
        const std::string tx = r.decisions[0].tx_id;
        for (int i = 0; i < 31; ++i) rig.step({}, unknown);
        CHECK(rig.j.find(tx)->state == TxState::RolledBack);
    }
    {
        Rig rig;  // memory policy judged on hard faults
        auto r = rig.settle({rec(200, PolicyKind::MemoryPriority, 2, policy::Area::Memory)}, metrics(-1, -1, 500.0));
        const std::string tx = r.decisions[0].tx_id;
        for (int i = 0; i < 31; ++i) rig.step({}, metrics(-1, -1, 200.0));
        CHECK(rig.j.find(tx)->state == TxState::Retained);
    }
}

TEST(control_cooldown_per_target) {
    Rig rig;
    auto r = rig.settle({rec(200)});
    CHECK(r.decisions[0].outcome == Outcome::Applied);
    // a different kind on the same target is blocked by the per-target cooldown
    r = rig.settle({rec(200, PolicyKind::PriorityClass, policy::kPriorityBelowNormal)});
    CHECK_EQ(r.decisions[0].blocked_by, std::string("cooldown"));
    // same kind again: already active
    r = rig.step({rec(200)});
    CHECK_EQ(r.decisions[0].blocked_by, std::string("already_active"));
    // after rollback the cooldown still holds for 10 minutes
    for (int i = 0; i < 31; ++i) rig.step({}, metrics(100.0));  // no improvement -> rolled back
    CHECK(rig.eng.active().empty());
    r = rig.settle({rec(200)});
    CHECK_EQ(r.decisions[0].blocked_by, std::string("cooldown"));
    for (int i = 0; i < 300; ++i) rig.step({});  // 10 minutes
    r = rig.settle({rec(200)});
    CHECK(r.decisions[0].outcome == Outcome::Applied);
}

TEST(control_global_rate_limit) {
    ControlConfig c = Rig::balanced();
    c.max_active = 20;
    Rig rig(c);
    std::vector<policy::Recommendation> recs;
    for (std::uint32_t pid = 200; pid < 206; ++pid) recs.push_back(rec(pid));
    auto r = rig.settle(recs, metrics(100.0));
    int applied = 0, limited = 0;
    for (const auto& d : r.decisions) {
        if (d.outcome == Outcome::Applied) ++applied;
        if (d.blocked_by == "rate_limit") ++limited;
    }
    CHECK_EQ(applied, 3);
    CHECK_EQ(limited, 3);
    // still limited inside the 5 minute window
    std::vector<policy::Recommendation> rest(recs.begin() + 3, recs.end());
    r = rig.step(rest, metrics(40.0));
    for (const auto& d : r.decisions) CHECK_EQ(d.blocked_by, std::string("rate_limit"));
    for (int i = 0; i < 150; ++i) rig.step(rest, metrics(40.0));  // 5 minutes later
    CHECK(rig.act.applies > 3);
    CHECK(rig.act.applies <= 6);
}

TEST(control_conflict_detection) {
    Rig rig;
    // same target/kind, different values (memory vs cpu disagreeing)
    auto a = rec(200, PolicyKind::MemoryPriority, 2, policy::Area::Memory);
    auto b = rec(200, PolicyKind::MemoryPriority, 3, policy::Area::Cpu);
    // a recommendation targets the process another recommendation is trying to protect
    auto c = rec(201);
    auto d = rec(202);
    d.beneficiary_pid = 201;
    auto r = rig.settle({a, b, c, d});
    CHECK_EQ(find_decision(r, 200, PolicyKind::MemoryPriority)->blocked_by, std::string("conflict"));
    CHECK_EQ(find_decision(r, 201, PolicyKind::EcoQoS)->blocked_by, std::string("conflict"));
    CHECK(find_decision(r, 202, PolicyKind::EcoQoS)->outcome == Outcome::Applied);
    // identical recommendations from both areas are merged, not a conflict
    Rig rig2;
    auto m = rec(203, PolicyKind::EcoQoS, 1, policy::Area::Memory);
    auto r2 = rig2.settle({rec(203), m});
    CHECK_EQ(r2.decisions.size(), 1u);
    CHECK(r2.decisions[0].outcome == Outcome::Applied);
}

TEST(control_cost_scoring) {
    auto eco = rec(1);
    auto mem = rec(1, PolicyKind::MemoryPriority, 2);
    auto prio = rec(1, PolicyKind::PriorityClass, policy::kPriorityBelowNormal);
    prio.risk = 2;
    auto sets = rec(1, PolicyKind::CpuSets, 4);
    sets.risk = 2;
    CHECK(control::intervention_cost(mem) < control::intervention_cost(eco));
    CHECK(control::intervention_cost(eco) < control::intervention_cost(prio));
    CHECK(control::intervention_cost(prio) < control::intervention_cost(sets));
    Rig rig;
    auto risky = rec(200);
    risky.risk = 5;  // 5 * 2 = 10 > 6
    auto r = rig.settle({risky});
    CHECK_EQ(r.decisions[0].blocked_by, std::string("cost_too_high"));
    // cheapest first
    Rig rig2;
    auto r2 = rig2.settle({prio, eco, mem});
    CHECK(r2.decisions[0].rec.kind == PolicyKind::MemoryPriority);
}

TEST(control_advanced_requires_consent) {
    ControlConfig c = Rig::balanced();
    c.mode = Mode::Advanced;
    c.opted_in.push_back(PolicyKind::CpuSets);
    Rig rig(c);
    auto sets = rec(200, PolicyKind::CpuSets, 4);
    sets.risk = 2;
    auto r = rig.settle({sets});
    CHECK(r.decisions[0].outcome == Outcome::PendingConsent);
    CHECK_EQ(r.decisions[0].blocked_by, std::string("consent_required"));
    CHECK_EQ(rig.eng.pending_consents().size(), 1u);
    CHECK_EQ(rig.act.applies, 0);
    CHECK(!rig.eng.approve("cpu:cpu_sets:999", "c-1", rig.now));
    CHECK(!rig.eng.approve(sets.id, "", rig.now));
    CHECK(rig.eng.approve(sets.id, "consent-42", rig.now));
    r = rig.step({sets});
    CHECK(r.decisions[0].outcome == Outcome::Applied);
    const auto* tx = rig.j.find(r.decisions[0].tx_id);
    CHECK_EQ(tx->required_auth, std::string("admin"));
    bool has_consent = false;
    for (const auto& p : tx->preconditions)
        if (p == "consent=consent-42") has_consent = true;
    CHECK(has_consent);
    // consent is single use
    CHECK(!rig.eng.approve(sets.id, "consent-42", rig.now));
}

TEST(control_consent_expires) {
    ControlConfig c = Rig::balanced();
    c.mode = Mode::Advanced;
    Rig rig(c);
    rig.settle({rec(200)});
    CHECK_EQ(rig.eng.pending_consents().size(), 1u);
    for (int i = 0; i < 160; ++i) rig.step({});  // > 5 min
    CHECK(rig.eng.pending_consents().empty());
    CHECK(!rig.eng.approve(rec(200).id, "late", rig.now));
}

TEST(control_rollback_on_mode_change_and_shutdown) {
    Rig rig;
    rig.settle({rec(200), rec(201), rec(202)});
    CHECK_EQ(rig.eng.active().size(), 3u);
    auto mc = rig.eng.set_mode(Mode::Monitor, rig.now);
    CHECK(mc.ok);
    CHECK_EQ(mc.rolled_back.restored, 3);
    CHECK(rig.eng.active().empty());
    CHECK_EQ(rig.act.restores, 3);
    // mode change is journaled as a config transaction
    bool config_seen = false;
    for (const auto* t : rig.j.recent(10))
        if (t->kind == "config" && t->action == "set_mode") config_seen = true;
    CHECK(config_seen);

    Rig rig2;
    rig2.settle({rec(205)});
    auto s = rig2.eng.shutdown(rig2.now);
    CHECK_EQ(s.restored, 1);
    CHECK(rig2.j.unfinished(true).empty());

    // advanced -> balanced removes CPU sets only
    ControlConfig c = Rig::balanced();
    c.mode = Mode::Advanced;
    c.opted_in.push_back(PolicyKind::CpuSets);
    Rig rig3(c);
    auto sets = rec(206, PolicyKind::CpuSets, 4);
    sets.risk = 2;
    rig3.settle({sets, rec(207)});
    CHECK(rig3.eng.approve(sets.id, "a", rig3.now));
    CHECK(rig3.eng.approve(rec(207).id, "b", rig3.now));
    rig3.step({sets, rec(207)});
    CHECK_EQ(rig3.eng.active().size(), 2u);
    auto mc3 = rig3.eng.set_mode(Mode::Balanced, rig3.now);
    CHECK_EQ(mc3.rolled_back.restored, 1);
    CHECK_EQ(rig3.eng.active().size(), 1u);
    CHECK(rig3.eng.active()[0].rec.kind == PolicyKind::EcoQoS);
}

TEST(control_opt_out_rolls_back_kind) {
    Rig rig;
    rig.settle({rec(200), rec(201, PolicyKind::MemoryPriority, 2, policy::Area::Memory)});
    CHECK_EQ(rig.eng.active().size(), 2u);
    auto s = rig.eng.set_opt_in(PolicyKind::EcoQoS, false, rig.now);
    CHECK_EQ(s.restored, 1);
    CHECK_EQ(rig.eng.active().size(), 1u);
    auto r = rig.settle({rec(210)});
    CHECK_EQ(r.decisions[0].blocked_by, std::string("not_opted_in"));
}

TEST(control_disable_policy) {
    Rig rig;
    auto r = rig.settle({rec(200)});
    CHECK(!rig.eng.disable_policy("tx-nope", rig.now));
    CHECK(rig.eng.disable_policy(r.decisions[0].tx_id, rig.now));
    CHECK(rig.eng.active().empty());
    CHECK(rig.j.find(r.decisions[0].tx_id)->state == TxState::RolledBack);
}

TEST(control_pid_reuse_guard) {
    Rig rig;
    auto r = rig.settle({rec(200)});
    const std::string tx = r.decisions[0].tx_id;
    rig.act.start[200] = 999999;  // the pid now belongs to a different process
    rig.step({});
    CHECK(rig.j.find(tx)->state == TxState::Expired);
    CHECK_EQ(rig.act.restores, 0);  // never restore onto the new process
    CHECK(rig.eng.active().empty());
    // target vanished before apply: nothing journaled as applied, actuator not called
    rig.act.start.erase(201);
    auto r2 = rig.settle({rec(201)});
    CHECK_EQ(r2.decisions[0].blocked_by, std::string("target_gone"));
    CHECK_EQ(rig.act.applies, 1);
}

TEST(control_actuator_failures_are_journaled) {
    Rig rig;
    rig.act.throw_on_apply = true;
    bool intent_seen = false;
    rig.act.on_apply = [&] {
        intent_seen = rig.io.files["journal.jsonl"].find("\"state\":\"intent\"") != std::string::npos;
    };
    auto r = rig.settle({rec(200)});
    CHECK(intent_seen);  // write-ahead even though the actuator throws
    CHECK(r.decisions[0].outcome == Outcome::Failed);
    CHECK(rig.j.find(r.decisions[0].tx_id)->state == TxState::Failed);
    CHECK(rig.eng.active().empty());
    // journal unavailable -> the actuator is never called
    Rig rig2;
    rig2.io.fail_append = true;
    auto r2 = rig2.settle({rec(200)});
    CHECK_EQ(r2.decisions[0].blocked_by, std::string("journal_unavailable"));
    CHECK_EQ(rig2.act.applies, 0);
    // escalating the mode is refused without a journal; de-escalation always works
    CHECK(!rig2.eng.set_mode(Mode::Advanced, rig2.now).ok);
    CHECK(rig2.eng.mode() == Mode::Balanced);
    CHECK(rig2.eng.set_mode(Mode::Monitor, rig2.now).ok);
    CHECK(rig2.eng.mode() == Mode::Monitor);
    // failed restore is reported
    Rig rig3;
    rig3.settle({rec(200)});
    rig3.act.fail_restore = true;
    auto s = rig3.eng.rollback_all(rig3.now, "test");
    CHECK_EQ(s.failed, 1);
}

TEST(control_crash_recovery_from_journal) {
    FakeActuator act;
    journal::MemoryFileIO io;
    for (std::uint32_t pid = 200; pid < 210; ++pid) act.start[pid] = 1000 + pid;
    std::string verifying_tx, retained_tx, intent_tx, gone_tx;
    {
        journal::Journal j(io, "journal.jsonl");
        ControlEngine eng(act, j, Rig::balanced());
        std::int64_t now = 0;
        auto run = [&](std::vector<policy::Recommendation> recs, policy::VerificationMetrics m) {
            control::TickResult last;
            for (int i = 0; i < 3; ++i) last = eng.step(now += kTick, recs, m);
            return last;
        };
        retained_tx = run({rec(200)}, metrics(100.0)).decisions[0].tx_id;
        for (int i = 0; i < 31; ++i) eng.step(now += kTick, {}, metrics(10.0));
        CHECK(j.find(retained_tx)->state == TxState::Retained);
        verifying_tx = run({rec(201)}, metrics(100.0)).decisions[0].tx_id;
        gone_tx = run({rec(202)}, metrics(100.0)).decisions[0].tx_id;
        // simulate a crash between "intent" and "applied"
        journal::Transaction t;
        t.kind = "policy";
        t.target_pid = 203;
        t.target = "onedrive.exe";
        t.target_start_ms = 1203;
        t.action = "priority_class";
        t.value = policy::kPriorityBelowNormal;
        intent_tx = *j.begin(t, now);
        // ...process dies hard: no shutdown()
    }
    act.start.erase(202);  // exited while the service was down
    const int restores_before = act.restores;
    journal::Journal j2(io, "journal.jsonl");
    CHECK_EQ(j2.load().corrupt, 0u);
    CHECK_EQ(j2.unfinished(true).size(), 4u);
    ControlEngine eng2(act, j2, Rig::balanced());
    auto s = eng2.recover(5'000'000);
    CHECK_EQ(s.restored, 3);
    CHECK_EQ(s.expired, 1);
    CHECK_EQ(act.restores - restores_before, 3);
    CHECK(j2.find(retained_tx)->state == TxState::RolledBack);
    CHECK(j2.find(verifying_tx)->state == TxState::RolledBack);
    CHECK(j2.find(gone_tx)->state == TxState::Expired);
    CHECK(j2.find(intent_tx)->state == TxState::RolledBack);
    // original unknown for the interrupted intent: Windows default (NORMAL priority) restored
    bool default_restored = false;
    for (const auto& [pid, v] : act.restored)
        if (pid == 203 && v == static_cast<std::int64_t>(policy::kPriorityNormal)) default_restored = true;
    CHECK(default_restored);
    CHECK(j2.unfinished(true).empty());
    // cooldowns survive the restart
    const std::vector<policy::Recommendation> again{rec(201)};
    auto r = eng2.step(5'000'000 + kTick, again, metrics(100.0));
    eng2.step(5'000'000 + 2 * kTick, again, metrics(100.0));
    r = eng2.step(5'000'000 + 3 * kTick, again, metrics(100.0));
    CHECK_EQ(r.decisions[0].blocked_by, std::string("cooldown"));
}

TEST(control_max_active_bound) {
    ControlConfig c = Rig::balanced();
    c.max_active = 2;
    c.rate_limit_actions = 10;
    Rig rig(c);
    auto r = rig.settle({rec(200), rec(201), rec(202)});
    int applied = 0;
    for (const auto& d : r.decisions)
        if (d.outcome == Outcome::Applied) ++applied;
    CHECK_EQ(applied, 2);
    CHECK_EQ(find_decision(r, 202, PolicyKind::EcoQoS)->blocked_by, std::string("max_active_policies"));
}
