#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "engine.hpp"
#include "engine_test_util.hpp"
#include "test_framework.hpp"

using namespace opensweep;
using namespace eng_test;
using control::Mode;
using telemetry::PolicyKind;

namespace {

bool any_journal(const std::vector<journal::Transaction>& j, const std::string& action, journal::TxState st) {
    return std::any_of(j.begin(), j.end(), [&](const journal::Transaction& t) { return t.action == action && t.state == st; });
}

telemetry::EventRecord service_failure(std::int64_t ts, const char* svc) {
    telemetry::EventRecord e;
    e.ts_ms = ts;
    e.kind = telemetry::EventKind::ServiceFailure;
    e.provider = "Service Control Manager";
    e.event_id = 7031;
    e.level = 2;
    e.subject = svc;
    e.message = std::string("service ") + svc + " terminated unexpectedly";
    return e;
}

}  // namespace

TEST(engine_settings_json_strict) {
    engine::Settings s;
    s.mode = Mode::Balanced;
    s.simulation = true;
    s.opted_in = {PolicyKind::EcoQoS, PolicyKind::PriorityClass};
    std::string err;
    auto back = engine::settings_from_json(engine::settings_to_json(s), err);
    CHECK(back.has_value());
    CHECK(back && back->mode == Mode::Balanced && back->simulation && back->opted_in == s.opted_in);

    auto bad = [&](const char* text) {
        auto p = json::parse(text);
        std::string e;
        return !p.ok() || !engine::settings_from_json(*p.value, e).has_value();
    };
    CHECK(bad("{\"version\":1,\"mode\":\"turbo\"}"));
    CHECK(bad("{\"version\":2,\"mode\":\"monitor\"}"));
    CHECK(bad("{\"version\":1,\"mode\":\"monitor\",\"simulation\":1}"));
    CHECK(bad("{\"version\":1,\"mode\":\"monitor\",\"optIn\":[\"ecoqos\",\"ecoqos\"]}"));
    CHECK(bad("{\"version\":1,\"mode\":\"monitor\",\"optIn\":[\"kill\"]}"));
    CHECK(bad("{\"version\":1,\"mode\":\"monitor\",\"extra\":true}"));
    CHECK(bad("[]"));
    CHECK(!bad("{\"version\":1,\"mode\":\"advanced\",\"simulation\":false,\"optIn\":[]}"));
    CHECK_EQ(engine::join_path("/data", "a.json"), std::string("/data/a.json"));
    CHECK_EQ(engine::join_path("C:\\ProgramData\\OpenSweep\\core", "a.json"),
             std::string("C:\\ProgramData\\OpenSweep\\core\\a.json"));
}

TEST(engine_default_mode_monitor_and_invalid_config_falls_back) {
    {
        Rig r;
        CHECK(r.eng->mode() == Mode::Monitor);
        CHECK(r.eng->status().simulation);
    }
    {
        Rig r(false);
        r.io.files["/data/config.json"] = "{not json";
        r.make(false);
        const auto rep = r.eng->start();
        CHECK(rep.config_found);
        CHECK(!rep.config_valid);
        CHECK(r.eng->mode() == Mode::Monitor);
        CHECK(!r.logs.empty());
    }
    {
        Rig r(false);
        r.io.files["/data/config.json"] = "{\"version\":1,\"mode\":\"balanced\",\"optIn\":[\"ecoqos\"]}";
        r.make(false);
        const auto rep = r.eng->start();
        CHECK(rep.config_valid);
        CHECK(r.eng->mode() == Mode::Balanced);
        CHECK(!r.eng->status().simulation);
        CHECK(r.eng->status().opted_in == std::vector<PolicyKind>{PolicyKind::EcoQoS});
    }
}

TEST(engine_monitor_mode_never_calls_actuator) {
    Rig r;
    // Opt in to everything (opt-ins only take effect outside monitor), stay in monitor.
    CHECK(r.eng->set_mode(Mode::Monitor, std::nullopt,
                          std::vector<PolicyKind>{PolicyKind::EcoQoS, PolicyKind::MemoryPriority,
                                                  PolicyKind::PriorityClass})
              .ok);
    r.run(1800, [](std::int64_t ts) { return contended(ts); });  // 1 hour of contention
    CHECK_EQ(r.act.applies, 0);
    CHECK_EQ(r.act.restores, 0);
    CHECK(r.eng->policies().empty());
    const auto recs = r.eng->recommendations();
    CHECK(!recs.empty());
    for (const auto& d : recs) CHECK(d.outcome == control::Outcome::Simulated);
    CHECK(std::any_of(recs.begin(), recs.end(), [](const control::Decision& d) { return d.rec.target.pid == 200; }));
    for (const auto& d : recs) CHECK(d.rec.target.pid != 100);  // foreground is never a target
    for (const auto& t : r.eng->journal(5000)) CHECK(t.kind != "policy");  // nothing was even attempted
    const auto st = r.eng->status();
    CHECK(st.simulation);
    CHECK_EQ(st.policies_active, std::size_t{0});
    CHECK(st.ticks == 1800u);
}

TEST(engine_balanced_optin_contention_apply_verify_rollback) {
    Rig r;
    const auto mr = r.eng->set_mode(Mode::Balanced, std::nullopt, std::vector<PolicyKind>{PolicyKind::EcoQoS});
    CHECK(mr.ok);
    CHECK(mr.persisted);
    CHECK(r.io.files.count("/data/config.json") == 1);
    CHECK(r.eng->mode() == Mode::Balanced);

    // A few quiet ticks, then sustained contention until something is applied.
    r.run(10, [](std::int64_t ts) { return quiet(ts); });
    CHECK_EQ(r.act.applies, 0);  // high usage alone never acts; nothing contended yet
    int ticks = 0;
    while (r.act.applies == 0 && ticks < 60) {
        r.tick(contended(r.now + kTick));
        ++ticks;
    }
    CHECK_EQ(r.act.applies, 1);
    CHECK(ticks >= 5);  // sustain (scheduler) + hysteresis (control)
    CHECK(!r.act.applied.empty() && r.act.applied[0].first == PolicyKind::EcoQoS && r.act.applied[0].second == 200);
    auto pols = r.eng->policies();
    CHECK_EQ(pols.size(), std::size_t{1});
    CHECK(!pols.empty() && pols[0].verifying);
    const auto j = r.eng->journal(100);
    CHECK(!j.empty() && j.front().kind == "policy");

    // Queue pressure halves after the change: verification retains the policy.
    r.run(40, [](std::int64_t ts) { return contended(ts, 8.0); });
    pols = r.eng->policies();
    CHECK_EQ(pols.size(), std::size_t{1});
    CHECK(!pols.empty() && !pols[0].verifying);
    CHECK_EQ(r.act.applies, 1);  // already active: not re-applied

    // Switching to monitor rolls everything back (journaled).
    const auto back = r.eng->set_mode(Mode::Monitor);
    CHECK(back.ok);
    CHECK_EQ(back.rolled_back.restored, 1);
    CHECK_EQ(r.act.restores, 1);
    CHECK(r.eng->policies().empty());
    CHECK(any_journal(r.eng->journal(100), "ecoqos", journal::TxState::RolledBack));
    // The mode change is an OpenSweepConfig event.
    const auto ev = r.eng->events(10);
    CHECK(!ev.empty() && ev.front().kind == telemetry::EventKind::OpenSweepConfig);
}

TEST(engine_policy_without_improvement_is_rolled_back) {
    Rig r;
    CHECK(r.eng->set_mode(Mode::Balanced, std::nullopt, std::vector<PolicyKind>{PolicyKind::EcoQoS}).ok);
    int ticks = 0;
    while (r.act.applies == 0 && ticks < 60) {
        r.tick(contended(r.now + kTick));
        ++ticks;
    }
    CHECK_EQ(r.act.applies, 1);
    r.run(40, [](std::int64_t ts) { return contended(ts, 20.0); });  // no improvement
    CHECK(r.act.restores >= 1);
}

TEST(engine_crash_recovery_rolls_back_from_journal) {
    Rig r;
    CHECK(r.eng->set_mode(Mode::Balanced, std::nullopt, std::vector<PolicyKind>{PolicyKind::EcoQoS}).ok);
    int ticks = 0;
    while (r.act.applies == 0 && ticks < 60) {
        r.tick(contended(r.now + kTick));
        ++ticks;
    }
    CHECK_EQ(r.act.applies, 1);
    CHECK_EQ(r.act.restores, 0);
    // "Crash": the engine object goes away without shutdown(); only the files remain.
    r.make(false);
    const auto rep = r.eng->start();
    CHECK_EQ(rep.recovered.restored, 1);
    CHECK_EQ(r.act.restores, 1);
    CHECK(r.eng->policies().empty());
    CHECK(r.eng->mode() == Mode::Balanced);  // the persisted mode survives, the policy does not
    CHECK(rep.journal.loaded >= 1);
}

TEST(engine_shutdown_rolls_back_and_persists) {
    Rig r;
    CHECK(r.eng->set_mode(Mode::Balanced, std::nullopt, std::vector<PolicyKind>{PolicyKind::EcoQoS}).ok);
    for (int i = 0; i < 60 && r.act.applies == 0; ++i) r.tick(contended(r.now + kTick));
    CHECK_EQ(r.act.applies, 1);
    const auto s = r.eng->shutdown();
    CHECK_EQ(s.restored, 1);
    CHECK(r.eng->policies().empty());
    CHECK(r.io.files.count("/data/trace.bin") == 1);
    CHECK(r.io.files.count("/data/baselines.json") == 1);
    CHECK(r.io.files.count("/data/incidents.jsonl") == 1);
    CHECK(r.io.files.count("/data/journal.jsonl") == 1);
    // Restart: nothing left to recover.
    r.make(false);
    CHECK_EQ(r.eng->start().recovered.restored, 0);
    CHECK_EQ(r.act.restores, 1);
}

TEST(engine_synthetic_leak_produces_incident_and_capture) {
    Rig r;
    obs_test::NormalGen gen(7);
    int minute = 0;
    // 3 hours: 2 h of normal behaviour, then a process leaking ~6 MB/min for an hour.
    for (int i = 0; i < 5400; ++i) {
        const std::int64_t ts = r.now + kTick;
        auto s = gen.sample(ts);
        if (i >= 3600) {
            minute = (i - 3600) / 30;
            auto p = proc(5000, "leaky.exe", 1.0, 200.0 + 6.0 * minute + 0.1 * ((i - 3600) % 30));
            s.top.push_back(p);
        }
        r.tick(s);
    }
    const auto incs = r.eng->incidents(50);
    const auto it = std::find_if(incs.begin(), incs.end(), [](const incident::Incident& x) {
        return x.detector == "memory_growth" && !x.contributors.empty() && x.contributors[0].pid == 5000;
    });
    CHECK(it != incs.end());
    if (it != incs.end()) {
        CHECK(it->state == incident::State::Open);
        CHECK(incident::wording_ok(*it));
        CHECK(it->confidence > 0.0 && it->confidence <= 1.0);
    }
    const auto caps = r.eng->captures();
    CHECK(!caps.empty());
    CHECK(caps.size() <= 8);
    if (!caps.empty()) {
        CHECK(!caps.back().rows.empty());
        CHECK(caps.back().rows.size() <= 151);
    }
    // Incidents are persisted on change.
    CHECK(r.io.files.count("/data/incidents.jsonl") == 1);
}

TEST(engine_persistence_round_trip_across_restart) {
    Rig r;
    obs_test::NormalGen gen(11);
    std::vector<telemetry::EventRecord> ev{service_failure(r.now + kTick, "vendorsvc")};
    r.tick(gen.sample(r.now + kTick), ev);
    for (int i = 0; i < 1200; ++i) r.tick(gen.sample(r.now + kTick));  // 40 min
    CHECK(r.io.files.count("/data/trace.bin") == 1);      // periodic (5 min)
    CHECK(r.io.files.count("/data/baselines.json") == 1);  // periodic (15 min)
    CHECK(r.io.files.count("/data/events.jsonl") == 1);    // on change
    const auto hist_before = r.eng->history(trace::Metric::CpuPercent, trace::Range::Hours24);
    const auto base_before = json::serialize(r.eng->baselines_summary());
    r.eng->shutdown();

    r.make(false);
    const auto rep = r.eng->start();
    CHECK(rep.trace.found && rep.trace.loaded > 0 && rep.trace.corrupt == 0);
    CHECK(rep.baselines.found && rep.baselines.corrupt == 0);
    CHECK(rep.events.loaded >= 1);
    CHECK(rep.journal.corrupt == 0);
    const auto hist_after = r.eng->history(trace::Metric::CpuPercent, trace::Range::Hours24);
    CHECK(!hist_after.empty());
    CHECK(hist_after.size() + 1 >= hist_before.size());  // the open minute bucket is not persisted
    CHECK_EQ(json::serialize(r.eng->baselines_summary()), base_before);
    const auto evs = r.eng->events(10);
    CHECK(std::any_of(evs.begin(), evs.end(), [](const telemetry::EventRecord& e) { return e.subject == "vendorsvc"; }));
}

TEST(engine_corrupt_files_do_not_prevent_start) {
    Rig r(false);
    r.io.files["/data/trace.bin"] = "garbage";
    r.io.files["/data/journal.jsonl"] = "{\"broken\n";
    r.io.files["/data/incidents.jsonl"] = "nope\n";
    r.io.files["/data/baselines.json"] = "{";
    r.make(false);
    const auto rep = r.eng->start();
    CHECK(rep.journal.corrupt >= 1);
    CHECK(r.eng->mode() == Mode::Monitor);
    r.run(5, [](std::int64_t ts) { return quiet(ts); });
    CHECK_EQ(r.eng->status().ticks, std::uint64_t{5});
}

TEST(engine_heal_automatic_and_approval_flow) {
    Rig r;
    heal::RecoveryRequest req;
    req.action = heal::Action::RestartCollector;
    req.target = "system_collector";
    req.trigger = "collect() failed";
    const auto res = r.eng->request_heal(req);
    CHECK(res.status == heal::Status::Succeeded);
    CHECK_EQ(r.exec.calls.size(), std::size_t{1});

    // OpenSweep-owned actions are executed by the engine, not the platform executor.
    heal::RecoveryRequest restore;
    restore.action = heal::Action::RestoreOwnedPolicies;
    restore.target = "policies";
    CHECK(r.eng->request_heal(restore).status == heal::Status::Succeeded);
    CHECK_EQ(r.exec.calls.size(), std::size_t{1});

    // A crash-looping third-party service becomes a pending approval, executed only on approve().
    std::vector<telemetry::EventRecord> ev;
    for (int i = 0; i < 3; ++i) ev.push_back(service_failure(r.now + kTick + i, "vendorsvc"));
    r.tick(quiet(r.now + kTick), ev);
    auto hv = r.eng->heal(50);
    CHECK_EQ(hv.approvals.size(), std::size_t{1});
    CHECK_EQ(r.exec.calls.size(), std::size_t{1});
    if (!hv.approvals.empty()) {
        CHECK(hv.approvals[0].action == heal::Action::RestartService);
        CHECK_EQ(hv.approvals[0].target, std::string("vendorsvc"));
        const auto ar = r.eng->approve(hv.approvals[0].id);
        CHECK(ar.kind == engine::ApproveResult::Kind::Recovery);
        CHECK(!ar.tx_id.empty());
        CHECK_EQ(r.exec.calls.size(), std::size_t{2});
        CHECK(r.exec.calls.back().first == heal::Action::RestartService);
    }
    CHECK(r.eng->approve("ap-unknown").kind == engine::ApproveResult::Kind::Unknown);
    hv = r.eng->heal(50);
    CHECK(hv.approvals.empty());
    CHECK(hv.history.size() >= 3);
    CHECK(!hv.open);
}

TEST(engine_bounded_memory_and_tick_cost) {
    Rig r;
    obs_test::NormalGen gen(3);
    std::vector<telemetry::EventRecord> ev;
    std::uint64_t total_us = 0, max_us = 0;
    const int kTicks = 8 * 1800;  // 8 hours
    for (int i = 0; i < kTicks; ++i) {
        const std::int64_t ts = r.now + kTick;
        auto s = gen.sample(ts);
        // Process churn: a new short-lived pid every tick plus up to 40 processes per sample.
        for (int k = 0; k < 35; ++k) {
            const auto pid = static_cast<std::uint32_t>(10000 + (i * 7 + k * 131) % 50000);
            s.top.push_back(proc(pid, "churn.exe", 0.5, 20.0 + k));
        }
        ev.clear();
        for (int k = 0; k < 3; ++k) {
            telemetry::EventRecord e;
            e.ts_ms = ts;
            e.kind = telemetry::EventKind::Other;
            e.provider = "test";
            e.event_id = static_cast<std::uint32_t>(k);
            e.message = "noise";
            ev.push_back(e);
        }
        const auto t0 = std::chrono::steady_clock::now();
        r.tick(s, ev);
        const auto us = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
        total_us += us;
        max_us = std::max(max_us, us);
    }
    const double avg = static_cast<double>(total_us) / kTicks;
    std::printf("  engine tick cost over %d ticks: avg %.1f us, max %llu us (engine-reported last %llu us)\n", kTicks,
                avg, static_cast<unsigned long long>(max_us),
                static_cast<unsigned long long>(r.eng->status().tick_cost_us));
    CHECK(avg < 5000.0);  // generous bound for slow CI machines / debug builds
    CHECK(r.eng->events(0).size() <= trace::kMaxEvents);
    CHECK(r.eng->incidents(0).size() <= incident::IncidentStore::kDefaultCapacity);
    CHECK(r.eng->captures().size() <= 8);
    CHECK(r.eng->tracked_processes() <= guard::kMaxTrackedProcesses + 512);
    CHECK(r.eng->journal(100000).size() <= 5000);
    // Event file is compacted, not unbounded (<= 2 x kMaxEvents lines + one batch).
    CHECK(obs_test::count_lines(r.io.files["/data/events.jsonl"]) <= 2 * trace::kMaxEvents + 3);
    // Persistent writes stay rare: trace every 5 min, baselines every 15 min.
    CHECK(r.io.atomic_writes < kTicks / 50);
}

TEST(engine_runner_ticks_on_its_own_thread) {
    Rig r;
    std::int64_t ts = kT0;
    int collected = 0;
    {
        engine::Runner run(
            *r.eng,
            [&](telemetry::Sample& s, std::vector<telemetry::EventRecord>&) {
                ts += kTick;
                s = quiet(ts);
                ++collected;
                return true;
            },
            std::chrono::milliseconds(5));
        run.start();
        for (int i = 0; i < 400 && r.eng->status().ticks < 5; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        CHECK(r.eng->status().running);
        // Reads from another thread while ticking.
        for (int i = 0; i < 20; ++i) (void)r.eng->status();
        run.stop();
    }
    CHECK(r.eng->status().ticks >= 5);
    CHECK(!r.eng->status().running);
    CHECK(collected >= 5);
}
