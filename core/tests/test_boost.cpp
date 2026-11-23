// Game boost (boost.hpp): session timing, target selection, cap, rollback on every end path, crash
// recovery, and no conflict with the Adaptive Control Engine.
#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "boost.hpp"
#include "control.hpp"
#include "engine.hpp"
#include "engine_test_util.hpp"
#include "journal.hpp"
#include "test_framework.hpp"

using namespace opensweep;
using namespace eng_test;
using control::Mode;
using telemetry::PolicyKind;

namespace {

// Actuator with real per-process state, so "everything restored" can be checked exactly.
struct StateActuator : telemetry::Actuator {
    std::map<std::pair<int, std::uint32_t>, std::int64_t> value;  // (kind, pid) -> current
    std::map<std::uint32_t, std::int64_t> live;                     // pid -> start ms
    std::map<std::uint32_t, std::int64_t> initial_mem;              // optional initial memory priority
    int applies = 0;
    int restores = 0;
    bool fail_apply = false;

    static std::int64_t def(PolicyKind k) { return k == PolicyKind::EcoQoS ? -1 : 5; }
    std::int64_t get(PolicyKind k, std::uint32_t pid) const {
        auto it = value.find({static_cast<int>(k), pid});
        if (it != value.end()) return it->second;
        if (k == PolicyKind::MemoryPriority) {
            auto m = initial_mem.find(pid);
            if (m != initial_mem.end()) return m->second;
        }
        return def(k);
    }
    telemetry::ApplyResult apply(PolicyKind k, const telemetry::PolicyTarget& t, std::int64_t v) override {
        ++applies;
        telemetry::ApplyResult r;
        if (fail_apply) {
            r.error = "denied";
            return r;
        }
        r.ok = true;
        r.original = get(k, t.pid);
        value[{static_cast<int>(k), t.pid}] = v;
        return r;
    }
    telemetry::ApplyResult restore(PolicyKind k, const telemetry::PolicyTarget& t, std::int64_t original) override {
        ++restores;
        value[{static_cast<int>(k), t.pid}] = original;
        telemetry::ApplyResult r;
        r.ok = true;
        return r;
    }
    bool alive(const telemetry::PolicyTarget& t) override {
        auto it = live.find(t.pid);
        return it != live.end() && (t.process_start_ms == 0 || it->second == t.process_start_ms);
    }
    bool throttled(std::uint32_t pid) const { return get(PolicyKind::EcoQoS, pid) == 1; }
    // Every process is back at its original values.
    bool pristine() const {
        for (const auto& [key, v] : value) {
            const auto k = static_cast<PolicyKind>(key.first);
            std::int64_t orig = def(k);
            if (k == PolicyKind::MemoryPriority)
                if (auto m = initial_mem.find(key.second); m != initial_mem.end()) orig = m->second;
            if (v != orig) return false;
        }
        return true;
    }
};

telemetry::ProcessSample bp(std::uint32_t pid, const char* name, double cpu, std::uint32_t parent = 1) {
    telemetry::ProcessSample p = proc(pid, name, cpu, 100);
    p.parent_pid = parent;
    return p;
}

constexpr std::uint32_t kGame = 500;

// A game in the foreground plus whatever `extra` contains.
telemetry::Sample scene(std::int64_t ts, const std::vector<telemetry::ProcessSample>& extra,
                        std::uint32_t fg = kGame) {
    telemetry::Sample s = quiet(ts);
    s.top.clear();
    auto game = bp(kGame, "game.exe", 40.0);
    game.foreground = fg == kGame;
    s.top.push_back(game);
    for (auto p : extra) {
        p.foreground = p.pid == fg;
        s.top.push_back(p);
    }
    s.foreground_pid = fg;
    return s;
}

struct BRig {
    journal::MemoryFileIO io;
    std::unique_ptr<journal::Journal> j;
    StateActuator act;
    std::unique_ptr<boost::GameBoost> b;
    std::int64_t t = kT0;
    std::vector<telemetry::ProcessSample> procs;
    std::vector<std::uint32_t> control_pids;

    explicit BRig(boost::BoostConfig cfg = {}) {
        j = std::make_unique<journal::Journal>(io, "/journal.jsonl");
        j->load();
        b = std::make_unique<boost::GameBoost>(act, *j, cfg);
        act.live[kGame] = 1000 + kGame;
        b->set_enabled(true, t);
    }
    void add(const telemetry::ProcessSample& p, bool alive = true) {
        procs.push_back(p);
        if (alive) act.live[p.pid] = p.start_ms;
    }
    // One tick at t + 2 s; `fs` = the app reports the game as fullscreen this tick.
    void step(bool fs, std::uint32_t fg = kGame) {
        t += kTick;
        b->report_foreground(fg, fs, t);
        b->tick(scene(t, procs, fg), control_pids);
    }
    void steps(int n, bool fs, std::uint32_t fg = kGame) {
        for (int i = 0; i < n; ++i) step(fs, fg);
    }
    std::size_t unfinished_boost() const {
        std::size_t n = 0;
        for (const auto* x : j->unfinished(true))
            if (x->trigger == boost::kTrigger) ++n;
        return n;
    }
    std::vector<std::uint32_t> held() const {
        auto h = b->held_pids();
        std::sort(h.begin(), h.end());
        return h;
    }
};

}  // namespace

TEST(boost_session_timing_start_5s_end_10s) {
    BRig r;
    r.add(bp(600, "onedrive.exe", 5.0));
    r.steps(15, false);  // 30 s of history, not fullscreen
    CHECK(!r.b->active());
    CHECK(r.act.applies == 0);

    r.step(true);  // fullscreen first seen
    const std::int64_t first = r.t;
    r.step(true);
    r.step(true);  // +4 s
    CHECK(!r.b->active());
    r.step(true);  // +6 s >= 5 s
    CHECK(r.b->active());
    CHECK(r.t - first >= 5000);
    CHECK_EQ(r.b->view().since, r.t);
    CHECK_EQ(r.b->view().foreground_pid, kGame);
    CHECK(r.act.throttled(600));
    CHECK_EQ(r.act.get(PolicyKind::MemoryPriority, 600), std::int64_t{2});

    // Not fullscreen any more: the session survives 8 s, ends at >= 10 s.
    r.step(false);
    const std::int64_t off = r.t;
    r.steps(4, false);  // +8 s
    CHECK(r.b->active());
    r.step(false);  // +10 s
    CHECK(!r.b->active());
    CHECK(r.t - off >= 10000);
    CHECK(r.act.pristine());
    CHECK_EQ(r.unfinished_boost(), std::size_t{0});
    const auto v = r.b->view();
    CHECK(v.last_session.has_value());
    CHECK(v.last_session && v.last_session->throttled_count == 1 && v.last_session->reason == "fullscreen_ended");
    CHECK(v.last_session && v.last_session->end == r.t);

    // A brief fullscreen drop (< 10 s) does not end a session.
    r.steps(4, true);
    CHECK(r.b->active());
    r.steps(2, false);
    r.steps(2, true);
    CHECK(r.b->active());
}

TEST(boost_needs_opt_in_matching_fresh_hint) {
    {
        BRig r;
        r.b->set_enabled(false, r.t);
        r.add(bp(600, "onedrive.exe", 5.0));
        r.steps(20, true);
        CHECK(!r.b->active());
        CHECK_EQ(r.act.applies, 0);
    }
    {
        // The hint names another process than the collector's foreground: never starts.
        BRig r;
        r.add(bp(600, "onedrive.exe", 5.0));
        for (int i = 0; i < 10; ++i) {
            r.t += kTick;
            r.b->report_foreground(777, true, r.t);
            r.b->tick(scene(r.t, r.procs), {});
        }
        CHECK(!r.b->active());
    }
    {
        // A stale hint (the app stopped reporting) does not start or keep a session.
        BRig r;
        r.add(bp(600, "onedrive.exe", 5.0));
        r.b->report_foreground(kGame, true, r.t);
        r.t += 16'000;
        for (int i = 0; i < 6; ++i) {
            r.t += kTick;
            r.b->tick(scene(r.t, r.procs), {});
        }
        CHECK(!r.b->active());
        r.steps(4, true);
        CHECK(r.b->active());
        for (int i = 0; i < 14; ++i) {  // reports stop: stale after 15 s, then 10 s grace
            r.t += kTick;
            r.b->tick(scene(r.t, r.procs), {});
        }
        CHECK(!r.b->active());
        CHECK(r.act.pristine());
    }
}

TEST(boost_target_selection_excludes_foreground_children_protected_and_low_cpu) {
    BRig r;
    r.add(bp(501, "node.exe", 10.0, kGame));      // child of the game (throughput image): excluded
    r.add(bp(502, "ffmpeg.exe", 12.0, 501));      // grandchild: excluded
    r.add(bp(600, "onedrive.exe", 5.0));          // background: throttled
    r.add(bp(601, "ffmpeg.exe", 20.0));           // throughput: throttled
    r.add(bp(602, "dropbox.exe", 0.5));           // below 1 %: not throttled
    auto av = bp(603, "MsMpEng.exe", 10.0);
    av.is_security_software = true;
    r.add(av);
    r.add(bp(604, "audiodg.exe", 6.0));           // protection list
    r.add(bp(605, "svchost.exe", 6.0));
    auto s0 = bp(606, "onedrive.exe", 6.0);       // session 0
    s0.session_id = 0;
    r.add(s0);
    r.add(bp(607, "randomapp.exe", 3.0));         // unknown workload
    r.add(bp(608, "EasyAntiCheat.exe", 5.0));     // anti-cheat
    auto audio = bp(609, "googleupdate.exe", 5.0);
    audio.has_audio = true;
    r.add(audio);
    auto nostart = bp(610, "ffmpeg.exe", 30.0);   // unknown start time (PID-reuse guard)
    nostart.start_ms = 0;
    r.add(nostart);
    auto crit = bp(611, "tiworker.exe", 5.0);
    crit.is_system_critical = true;
    r.add(crit);
    r.add(bp(612, "OpenSweep.exe", 8.0));         // our own app
    r.steps(15, false);
    r.steps(4, true);
    CHECK(r.b->active());
    CHECK((r.held() == std::vector<std::uint32_t>{600, 601}));
    const auto v = r.b->view();
    CHECK_EQ(v.throttled.size(), std::size_t{2});
    CHECK_EQ(v.throttled[0].target.pid, 601u);  // highest CPU first
    CHECK_EQ(std::string(boost::workload_name(v.throttled[0].workload)), std::string("throughput"));
    CHECK_EQ(std::string(boost::workload_name(v.throttled[1].workload)), std::string("background"));
    CHECK(!r.act.throttled(kGame) && !r.act.throttled(501) && !r.act.throttled(502));

    // Journal: kind policy, trigger boost, one transaction per (target, kind), applied with original.
    std::size_t boost_tx = 0;
    for (const auto* x : r.j->unfinished(true)) {
        CHECK(x->kind == "policy" && x->trigger == "boost" && x->state == journal::TxState::Applied && x->has_original);
        CHECK(x->action == "ecoqos" || x->action == "memory_priority");
        ++boost_tx;
    }
    CHECK_EQ(boost_tx, std::size_t{4});

    // A child the game spawns during the session is never throttled.
    r.add(bp(520, "ffmpeg.exe", 25.0, kGame));
    r.steps(16, true);
    CHECK(!r.act.throttled(520));
    // A newly busy background process is picked up while the session runs.
    r.add(bp(630, "googledrivefs.exe", 4.0));
    r.steps(16, true);
    CHECK(r.act.throttled(630));
}

TEST(boost_low_average_over_30s_window) {
    BRig r;
    // 4 % for 6 s, then nothing: average over 30 s < 1 %.
    auto spike = bp(600, "onedrive.exe", 4.0);
    r.add(spike);
    r.steps(3, false);
    r.procs[0].cpu_percent = 0.1;
    r.steps(12, false);
    r.steps(4, true);
    CHECK(r.b->active());
    CHECK(!r.act.throttled(600));
}

TEST(boost_caps_at_16_targets) {
    BRig r;
    for (std::uint32_t i = 0; i < 20; ++i) r.add(bp(700 + i, "ffmpeg.exe", 2.0 + i));
    r.steps(15, false);
    r.steps(4, true);
    CHECK(r.b->active());
    const auto h = r.held();
    CHECK_EQ(h.size(), std::size_t{16});
    CHECK(h.front() == 704u && h.back() == 719u);  // the 16 busiest
    r.steps(10, true);
    CHECK_EQ(r.held().size(), std::size_t{16});
    CHECK_EQ(r.act.applies, 32);
    r.b->end(r.t, "service_stop");
    CHECK(r.act.pristine());
    CHECK_EQ(r.b->view().last_session->throttled_count, std::size_t{16});
}

TEST(boost_rollback_on_every_end_path) {
    auto session = [](BRig& r) {
        r.add(bp(600, "onedrive.exe", 5.0));
        r.add(bp(601, "ffmpeg.exe", 9.0));
        r.steps(15, false);
        r.steps(4, true);
        CHECK(r.b->active());
        CHECK(r.act.throttled(600) && r.act.throttled(601));
    };
    auto restored = [](BRig& r, const char* reason) {
        CHECK(!r.b->active());
        CHECK(r.act.pristine());
        CHECK_EQ(r.unfinished_boost(), std::size_t{0});
        CHECK(r.b->held_pids().empty());
        const auto v = r.b->view();
        CHECK(v.last_session && v.last_session->reason == reason);
    };
    {  // the game exits
        BRig r;
        session(r);
        r.act.live.erase(kGame);
        r.step(true);
        restored(r, "foreground_exited");
    }
    {  // setBoost false
        BRig r;
        session(r);
        const auto s = r.b->set_enabled(false, r.t);
        CHECK_EQ(s.restored, 4);
        restored(r, "disabled");
        r.steps(10, true);
        CHECK(!r.b->active());
    }
    {  // service stop
        BRig r;
        session(r);
        r.b->end(r.t, "service_stop");
        restored(r, "service_stop");
    }
    {  // rollbackAll: no new session until the game leaves fullscreen
        BRig r;
        session(r);
        r.b->end(r.t, "admin_rollback_all", true);
        restored(r, "admin_rollback_all");
        r.steps(10, true);
        CHECK(!r.b->active());
        r.step(false);
        r.steps(4, true);
        CHECK(r.b->active());
    }
    {  // a target exits during the session: journaled expired, nothing restored onto a reused pid
        BRig r;
        session(r);
        r.act.live.erase(600);
        r.step(true);
        CHECK((r.held() == std::vector<std::uint32_t>{601}));
        std::size_t expired = 0;
        for (const auto* x : r.j->recent(100))
            if (x->target_pid == 600 && x->state == journal::TxState::Expired) ++expired;
        CHECK_EQ(expired, std::size_t{2});
    }
    {  // a target becomes the foreground: restored at once
        BRig r;
        session(r);
        r.step(false, 600);
        CHECK(!r.act.throttled(600));
        CHECK(r.act.get(PolicyKind::MemoryPriority, 600) == 5);
        CHECK(r.act.throttled(601));
        CHECK(r.b->active());  // the game has 10 s to come back
    }
}

TEST(boost_crash_recovery_restores_from_journal) {
    BRig r;
    r.add(bp(600, "onedrive.exe", 5.0));
    r.add(bp(601, "ffmpeg.exe", 9.0));
    r.steps(15, false);
    r.steps(4, true);
    CHECK(r.act.throttled(600) && r.act.throttled(601));
    // An interrupted apply (intent only, no original recorded).
    journal::Transaction tx;
    tx.kind = "policy";
    tx.trigger = boost::kTrigger;
    tx.target_pid = 602;
    tx.target = "dropbox.exe";
    tx.target_start_ms = 1602;
    tx.action = "ecoqos";
    tx.value = 1;
    CHECK(r.j->begin(tx, r.t).has_value());
    r.act.live[602] = 1602;
    r.act.value[{static_cast<int>(PolicyKind::EcoQoS), 602}] = 1;

    // "Crash": a new process loads the same journal. The control engine leaves boost transactions alone.
    journal::Journal j2(r.io, "/journal.jsonl");
    j2.load();
    control::ControlEngine ctl(r.act, j2);
    const auto cs = ctl.recover(r.t + 60'000);
    CHECK_EQ(cs.restored + cs.failed + cs.expired, 0);
    boost::GameBoost b2(r.act, j2);
    const auto s = b2.recover(r.t + 60'000);
    CHECK_EQ(s.restored, 5);
    CHECK(r.act.pristine());
    std::size_t left = 0;
    for (const auto* x : j2.unfinished(true))
        if (x->trigger == boost::kTrigger) ++left;
    CHECK_EQ(left, std::size_t{0});
    // Idempotent.
    CHECK_EQ(b2.recover(r.t + 70'000).restored, 0);
}

TEST(boost_skips_targets_control_holds_and_never_raises_memory_priority) {
    BRig r;
    r.add(bp(600, "onedrive.exe", 5.0));
    r.add(bp(601, "ffmpeg.exe", 9.0));
    r.add(bp(602, "googledrivefs.exe", 6.0));
    r.act.initial_mem[602] = 1;  // already very low
    r.control_pids = {600};
    r.steps(15, false);
    r.steps(4, true);
    CHECK(r.b->active());
    CHECK(!r.act.throttled(600));
    CHECK(r.act.throttled(601));
    CHECK(r.act.throttled(602));
    CHECK_EQ(r.act.get(PolicyKind::MemoryPriority, 602), std::int64_t{1});  // never raised
    // Control takes a target over: boost lets go first.
    r.control_pids = {600, 601};
    r.step(true);
    CHECK(!r.act.throttled(601));
    CHECK((r.held() == std::vector<std::uint32_t>{602}));
}

TEST(boost_journal_unavailable_applies_nothing) {
    BRig r;
    r.add(bp(600, "onedrive.exe", 5.0));
    r.steps(15, false);
    r.io.fail_append = true;
    r.steps(4, true);
    CHECK(r.b->active());
    CHECK_EQ(r.act.applies, 0);
    CHECK(r.b->held_pids().empty());
}

// ---- engine integration ------------------------------------------------------------------------
namespace {

telemetry::Sample game_scene(std::int64_t ts) {
    telemetry::Sample s = quiet(ts);  // word.exe (100) foreground, SearchProtocolHost (200)
    s.top[1].cpu_percent = 5.0;
    s.top.push_back(proc(300, "ffmpeg.exe", 15.0, 100));
    return s;
}

void boost_ticks(Rig& r, int n, bool fs, telemetry::Sample (*gen)(std::int64_t) = game_scene) {
    for (int i = 0; i < n; ++i) {
        r.now += kTick;
        r.eng->report_foreground(100, fs);
        r.tick(gen(r.now));
    }
}

}  // namespace

TEST(engine_boost_setting_persisted_and_backward_compatible) {
    {
        Rig r;
        CHECK(!r.eng->boost_enabled());
        const auto res = r.eng->set_boost(true);
        CHECK(res.enabled && res.persisted && !res.active);
        CHECK(r.io.files["/data/config.json"].find("\"boost\":true") != std::string::npos);
        r.make();
        CHECK(r.eng->boost_enabled());
        CHECK(r.eng->status().boost_enabled);
    }
    {
        Rig r(false);
        r.io.files["/data/config.json"] = "{\"version\":1,\"mode\":\"balanced\",\"simulation\":false,\"optIn\":[]}";
        r.make();
        CHECK(r.eng->mode() == Mode::Balanced);
        CHECK(!r.eng->boost_enabled());
    }
    {
        Rig r(false);
        r.io.files["/data/config.json"] = "{\"version\":1,\"mode\":\"balanced\",\"boost\":\"yes\"}";
        r.make(false);
        const auto rep = r.eng->start();
        CHECK(!rep.config_valid);
        CHECK(!r.eng->boost_enabled());
        CHECK(r.eng->mode() == Mode::Monitor);
    }
}

TEST(engine_boost_session_in_monitor_mode_shutdown_and_crash_recovery) {
    Rig r;
    r.act.start[300] = 1300;
    CHECK(r.eng->mode() == Mode::Monitor);  // works in any mode (explicit opt-in)
    r.eng->set_boost(true);
    boost_ticks(r, 15, false);
    boost_ticks(r, 4, true);
    auto v = r.eng->boost_view();
    CHECK(v.active);
    CHECK_EQ(v.foreground_pid, 100u);
    CHECK_EQ(v.foreground_name, std::string("word.exe"));
    CHECK_EQ(v.throttled.size(), std::size_t{2});  // ffmpeg (300) + SearchProtocolHost (200)
    CHECK_EQ(r.act.applies, 4);
    CHECK(r.eng->status().boost_active);
    CHECK(r.eng->policies().empty());  // boost targets are not control policies

    // Shutdown restores everything.
    r.eng->shutdown();
    CHECK_EQ(r.act.restores, 4);
    CHECK(!r.eng->boost_view().active);

    // New session, then a "crash" (no shutdown): the next start restores from the journal.
    r.make();
    boost_ticks(r, 15, false);
    boost_ticks(r, 4, true);
    CHECK(r.eng->boost_view().active);
    CHECK_EQ(r.act.applies, 8);
    r.make();  // destroys the engine without shutdown(), loads the same files
    CHECK_EQ(r.act.restores, 8);
    bool journaled = false;
    for (const auto& t : r.eng->journal(50))
        if (t.trigger == "boost" && t.state == journal::TxState::RolledBack && !t.audit.empty() &&
            t.audit.back().event == "crash_recovery")
            journaled = true;
    CHECK(journaled);
}

TEST(engine_boost_rollback_all_and_disable) {
    Rig r;
    r.act.start[300] = 1300;
    r.eng->set_boost(true);
    boost_ticks(r, 15, false);
    boost_ticks(r, 4, true);
    CHECK(r.eng->boost_view().active);
    const auto s = r.eng->rollback_all();
    CHECK_EQ(s.restored, 4);
    CHECK(!r.eng->boost_view().active);
    boost_ticks(r, 5, true);
    CHECK(!r.eng->boost_view().active);  // suppressed until fullscreen ends
    boost_ticks(r, 1, false);
    boost_ticks(r, 4, true);
    CHECK(r.eng->boost_view().active);
    const auto off = r.eng->set_boost(false);
    CHECK(!off.enabled && !off.active);
    CHECK_EQ(off.restored.restored, 4);
    CHECK_EQ(r.act.applies, r.act.restores);
}

TEST(engine_boost_and_control_never_hold_the_same_process) {
    {
        // Control holds SearchProtocolHost (200) first: boost leaves it alone.
        Rig r;
        CHECK(r.eng->set_mode(Mode::Balanced, std::nullopt, std::vector<PolicyKind>{PolicyKind::EcoQoS}).ok);
        for (int i = 0; i < 60 && r.act.applies == 0; ++i) r.tick(contended(r.now + kTick));
        CHECK_EQ(r.act.applies, 1);
        CHECK(!r.eng->policies().empty() && r.eng->policies()[0].rec.target.pid == 200u);
        r.eng->set_boost(true);
        boost_ticks(r, 15, false, [](std::int64_t ts) { return contended(ts, 8.0); });
        boost_ticks(r, 4, true, [](std::int64_t ts) { return contended(ts, 8.0); });
        CHECK(r.eng->boost_view().active);
        for (const auto& [k, pid] : r.act.applied) CHECK(!(pid == 200u && k == PolicyKind::MemoryPriority));
        CHECK(r.eng->boost_view().throttled.empty());
    }
    {
        // Boost holds 200 first: the control engine never acts on it (its recommendations are dropped).
        Rig r;
        r.eng->set_boost(true);
        boost_ticks(r, 15, false, [](std::int64_t ts) { return contended(ts, 8.0); });
        boost_ticks(r, 4, true, [](std::int64_t ts) { return contended(ts, 8.0); });
        CHECK_EQ(r.eng->boost_view().throttled.size(), std::size_t{1});
        CHECK_EQ(r.act.applies, 2);
        CHECK(r.eng->set_mode(Mode::Balanced, std::nullopt, std::vector<PolicyKind>{PolicyKind::EcoQoS}).ok);
        boost_ticks(r, 60, true, [](std::int64_t ts) { return contended(ts, 8.0); });
        CHECK_EQ(r.act.applies, 2);
        CHECK(r.eng->policies().empty());
        for (const auto& d : r.eng->recommendations()) CHECK(d.rec.target.pid != 200u);
        CHECK(r.eng->boost_view().active);  // mode changes do not end a boost session
    }
}
