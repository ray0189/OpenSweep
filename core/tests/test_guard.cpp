#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "baseline.hpp"
#include "guard.hpp"
#include "incident.hpp"
#include "observe_test_util.hpp"
#include "test_framework.hpp"

using namespace opensweep;
using namespace opensweep::guard;
using incident::Incident;
using incident::Level;
using incident::Severity;
using incident::State;
using telemetry::EventKind;
using telemetry::EventRecord;
using telemetry::Sample;
using obs_test::kGBu;
using obs_test::kMB;
using obs_test::proc;

namespace {

constexpr std::int64_t kT0 = 1'700'000'000'000;  // minute aligned
constexpr std::int64_t kMin = 60'000;
constexpr double kMinD = 60'000.0;

baseline::BaselineSetConfig fast_baselines() {
    baseline::BaselineSetConfig c;
    c.overall.ready_after = 30;  // 30 minutes instead of 120 to keep tests quick
    c.context.ready_after = 20;
    return c;
}

struct Harness {
    explicit Harness(GuardConfig g = {}, unsigned seed = 42)
        : guard(std::make_unique<Guard>(g)), base(fast_baselines()), gen(seed) {}

    std::unique_ptr<Guard> guard;
    baseline::BaselineSet base;
    incident::IncidentStore store;
    obs_test::NormalGen gen;
    std::int64_t ts = kT0;
    std::size_t opened_reports = 0;

    void step(const Sample& s, const std::vector<EventRecord>& ev = {}) {
        base.update(s);
        base.observe_events(ev);
        guard->evaluate(s, ev, base, store);
        opened_reports += guard->last_opened().size();
    }
    // Runs `seconds` of 2-s samples; `mod(sample, elapsed_ms)` shapes each sample.
    void run(int seconds, const std::function<void(Sample&, std::int64_t)>& mod = {}) {
        const std::int64_t start = ts;
        for (int i = 0; i < seconds / 2; ++i) {
            Sample s = gen.sample(ts);
            if (mod) mod(s, ts - start);
            step(s);
            ts += 2000;
        }
    }
    void warmup(int minutes = 35) { run(minutes * 60); }
    // One event at the current time (with a normal sample).
    void event(EventKind kind, const std::string& subject, std::uint8_t level = 2, double value = 0.0) {
        EventRecord e;
        e.ts_ms = ts;
        e.kind = kind;
        e.provider = "test";
        e.event_id = 1;
        e.level = level;
        e.subject = subject;
        e.value = value;
        step(gen.sample(ts), {e});
        ts += 2000;
    }
    // Moves time forward with one sample per minute (fast-forward for event tests).
    void sparse(int minutes) {
        for (int i = 0; i < minutes; ++i) {
            ts += kMin;
            step(gen.sample(ts));
        }
    }
    std::vector<Incident> by(const std::string& detector) const {
        std::vector<Incident> out;
        for (const auto& i : store.list())
            if (i.detector == detector) out.push_back(i);
        return out;
    }
    const Incident* open_by(const std::string& detector) const {
        static thread_local Incident keep;
        for (const auto& i : store.list()) {
            if (i.detector == detector && i.state == State::Open) {
                keep = i;
                return &keep;
            }
        }
        return nullptr;
    }
    bool wording_all_ok() const {
        for (const auto& i : store.list()) {
            if (!incident::wording_ok(i)) return false;
            if (i.auto_recovery_allowed) return false;
            if (i.evidence.empty() || i.recommended_actions.empty() || i.symptoms.empty()) return false;
            if (i.confidence < 0.0 || i.confidence > 1.0) return false;
        }
        return true;
    }
};

bool has_contributor(const Incident& inc, const std::string& name) {
    for (const auto& c : inc.contributors)
        if (c.name == name) return true;
    return false;
}

bool contains(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }

}  // namespace

// ---------------------------------------------------------------------------------------------
TEST(guard_hysteresis) {
    Hysteresis h(10'000, 6'000, 5'000);
    CHECK(h.update(true, 0) == Hysteresis::Edge::None);
    CHECK(h.update(true, 4'000) == Hysteresis::Edge::None);
    CHECK(h.update(false, 6'000) == Hysteresis::Edge::None);  // interrupted before 10 s
    CHECK(h.update(true, 8'000) == Hysteresis::Edge::None);
    CHECK(h.update(true, 12'000) == Hysteresis::Edge::None);
    CHECK(h.update(true, 16'000) == Hysteresis::Edge::None);
    CHECK(h.update(true, 18'000) == Hysteresis::Edge::Opened);
    CHECK(h.is_open());
    CHECK(h.update(true, 20'000) == Hysteresis::Edge::None);  // no duplicate open
    CHECK(h.update(false, 22'000) == Hysteresis::Edge::None);
    CHECK(h.update(true, 24'000) == Hysteresis::Edge::None);  // relapse restarts the recovery timer
    CHECK(h.update(false, 26'000) == Hysteresis::Edge::None);
    CHECK(h.update(false, 30'000) == Hysteresis::Edge::None);
    CHECK(h.update(false, 32'000) == Hysteresis::Edge::Resolved);
    CHECK(!h.is_open());
    // Gap > max_gap restarts the timer.
    Hysteresis g(4'000, 0, 5'000);
    g.update(true, 0);
    g.update(true, 2'000);
    CHECK(g.update(true, 60'000) == Hysteresis::Edge::None);
    CHECK(g.update(true, 62'000) == Hysteresis::Edge::None);
    CHECK(g.update(true, 64'000) == Hysteresis::Edge::Opened);
    // Two thresholds: neither on nor off keeps the incident open.
    Hysteresis t(0, 2'000);
    CHECK(t.update(true, false, 0) == Hysteresis::Edge::Opened);
    CHECK(t.update(false, false, 2'000) == Hysteresis::Edge::None);
    CHECK(t.update(false, false, 10'000) == Hysteresis::Edge::None);
    CHECK(t.is_open());
}

TEST(guard_normal_load_raises_nothing) {
    GuardConfig cfg;
    cfg.logical_processors = 8;
    Harness h(cfg, 7);
    // 3 hours of noisy normal operation with short spikes and a busy foreground game.
    h.run(3 * 3600, [](Sample& s, std::int64_t el) {
        const std::int64_t sec = el / 1000;
        if (sec % 600 == 0) {  // a single-sample spike every 10 minutes
            s.cpu_percent = 100;
            s.disk_read_latency_ms = 400;
            s.dpc_percent = 15;
            s.cpu_freq_percent = 40;
        }
        if (sec % 3600 >= 1200 && sec % 3600 < 1220) {  // 20 s commit spike to 92 %
            s.commit_total = static_cast<std::uint64_t>(0.92 * static_cast<double>(s.commit_limit));
        }
        if (el >= 3600'000 && el < 5400'000) {  // 30 min busy foreground game (96 % of a core)
            auto p = proc(7777, "game.exe", 12.0, 3000);
            p.foreground = true;
            s.top.push_back(p);
            s.foreground_pid = 7777;
        }
        // Background process with modest steady CPU (40 % of a core)
        s.top.push_back(proc(8888, "indexer.exe", 5.0, 150));
    });
    CHECK_EQ(h.store.size(), 0u);
    for (const auto& i : h.store.list()) std::fprintf(stderr, "    unexpected incident: %s\n", i.title.c_str());
    CHECK_EQ(h.opened_reports, 0u);
    const auto sc = h.guard->score(h.gen.sample(h.ts), h.store);
    CHECK_EQ(sc.score, 100);
    CHECK(sc.reasons.empty());
}

// ---------------------------------------------------------------------------------------------
TEST(guard_memory_leak_fires) {
    Harness h;
    h.warmup();
    std::int64_t opened_at = -1;
    const std::int64_t start = h.ts;
    h.run(30 * 60, [&](Sample& s, std::int64_t el) {
        const double mins = static_cast<double>(el) / kMinD;
        s.top.push_back(proc(5000, "leaky.exe", 1.0, 200 + 5.0 * mins + h.gen.u(-1, 1)));
        if (opened_at < 0 && h.store.find_open("memory_growth|leaky.exe|5000")) opened_at = s.ts_ms - start;
    });
    const auto* inc = h.open_by("memory_growth");
    CHECK(inc != nullptr);
    if (!inc) return;
    CHECK(opened_at >= 10 * kMin && opened_at <= 13 * kMin);
    CHECK(has_contributor(*inc, "leaky.exe"));
    CHECK(contains(inc->title, "leak"));  // classified as suspected leak after >= 20 min
    CHECK(contains(inc->summary, "MB/min"));
    CHECK(inc->evidence[0].observed > 4.0 && inc->evidence[0].observed < 6.0);
    CHECK(inc->confidence_level != Level::Low);
    CHECK_EQ(inc->occurrences, 1u);
    CHECK_EQ(h.by("memory_growth").size(), 1u);  // one incident, refreshed, not duplicated
    CHECK(h.wording_all_ok());
}

TEST(guard_memory_noise_sawtooth_step_do_not_fire) {
    Harness h;
    h.warmup();
    h.run(45 * 60, [&](Sample& s, std::int64_t el) {
        const std::int64_t m = el / kMin;
        s.top.push_back(proc(6001, "noisy.exe", 1.0, 500 + h.gen.n(0, 30)));
        // GC-style sawtooth: +100 MB/min for 9 minutes, then released.
        s.top.push_back(proc(6002, "gc.exe", 1.0, 300 + 100.0 * static_cast<double>(m % 10)));
        // One-off step of +500 MB then flat.
        s.top.push_back(proc(6003, "step.exe", 1.0, (m >= 5 ? 800 : 300) + h.gen.u(-2, 2)));
        // Small slow growth below the threshold (1 MB/min).
        s.top.push_back(proc(6004, "slow.exe", 1.0, 100 + static_cast<double>(el) / kMinD));
    });
    CHECK_EQ(h.by("memory_growth").size(), 0u);
}

TEST(guard_memory_leak_resolves_after_recovery) {
    Harness h;
    h.warmup();
    h.run(20 * 60, [&](Sample& s, std::int64_t el) {
        s.top.push_back(proc(5000, "leaky.exe", 1.0, 200 + 5.0 * static_cast<double>(el) / kMinD));
    });
    CHECK(h.open_by("memory_growth") != nullptr);
    h.run(40 * 60, [&](Sample& s, std::int64_t) { s.top.push_back(proc(5000, "leaky.exe", 1.0, 300 + h.gen.u(-1, 1))); });
    CHECK(h.open_by("memory_growth") == nullptr);
    const auto all = h.by("memory_growth");
    CHECK_EQ(all.size(), 1u);
    CHECK(!all.empty() && all[0].state == State::Resolved);
}

TEST(guard_memory_leak_process_exit_resolves) {
    Harness h;
    h.warmup();
    h.run(15 * 60, [&](Sample& s, std::int64_t el) {
        s.top.push_back(proc(5000, "leaky.exe", 1.0, 200 + 6.0 * static_cast<double>(el) / kMinD));
    });
    CHECK(h.open_by("memory_growth") != nullptr);
    h.run(7 * 60);  // process gone
    CHECK(h.open_by("memory_growth") == nullptr);
}

// ---------------------------------------------------------------------------------------------
TEST(guard_commit_exhaustion_fires_with_contributor) {
    Harness h;
    h.warmup();
    h.run(25 * 60, [&](Sample& s, std::int64_t el) {
        const double frac = static_cast<double>(el) / (25.0 * kMinD);
        const double pct = 0.50 + 0.46 * frac;
        s.commit_total = static_cast<std::uint64_t>(pct * static_cast<double>(s.commit_limit));
        const double hog_mb = 500 + 0.46 * frac * 20.0 * 1024.0;
        s.top.push_back(proc(4242, "hog.exe", 2.0, hog_mb));
    });
    const auto* inc = h.open_by("commit_exhaustion");
    CHECK(inc != nullptr);
    if (inc) {
        CHECK(has_contributor(*inc, "hog.exe"));
        CHECK(contains(inc->summary, "correlated with"));
        CHECK(inc->risk == Level::High);
        bool tte = false;
        for (const auto& e : inc->evidence) tte = tte || e.metric == "commitTimeToLimit";
        CHECK(tte);
    }
    CHECK(h.wording_all_ok());
    // Recovery below 75 % for 5 minutes resolves it.
    h.run(7 * 60);
    CHECK(h.open_by("commit_exhaustion") == nullptr);
}

TEST(guard_commit_spike_and_flat_high_do_not_fire) {
    Harness h;
    h.warmup();
    h.run(40, [](Sample& s, std::int64_t) { s.commit_total = static_cast<std::uint64_t>(0.95 * static_cast<double>(s.commit_limit)); });
    h.run(5 * 60);
    // Flat at 85 % (below the high mark, no growth) for 20 minutes.
    h.run(20 * 60, [&](Sample& s, std::int64_t) {
        s.commit_total = static_cast<std::uint64_t>((0.85 + h.gen.n(0, 0.003)) * static_cast<double>(s.commit_limit));
    });
    CHECK_EQ(h.by("commit_exhaustion").size(), 0u);
}

// ---------------------------------------------------------------------------------------------
TEST(guard_cpu_runaway) {
    GuardConfig cfg;
    cfg.logical_processors = 8;
    Harness h(cfg);
    h.warmup();
    // 4 minutes is not enough.
    h.run(4 * 60, [](Sample& s, std::int64_t) { s.top.push_back(proc(9000, "spin.exe", 12.0, 50)); });
    CHECK_EQ(h.by("cpu_runaway").size(), 0u);
    h.run(2 * 60);  // break
    // Foreground process at the same load: never.
    h.run(8 * 60, [](Sample& s, std::int64_t) {
        auto p = proc(9001, "editor.exe", 12.0, 50);
        s.top.push_back(p);
        s.foreground_pid = 9001;
    });
    CHECK_EQ(h.by("cpu_runaway").size(), 0u);
    // Background, 96 % of a core for 6 minutes: fires once.
    h.run(6 * 60, [](Sample& s, std::int64_t) { s.top.push_back(proc(9000, "spin.exe", 12.0, 50)); });
    const auto* inc = h.open_by("cpu_runaway");
    CHECK(inc != nullptr);
    if (inc) {
        CHECK(has_contributor(*inc, "spin.exe"));
        CHECK(contains(inc->title, "spin.exe"));
        CHECK(!inc->auto_recovery_allowed);
    }
    CHECK_EQ(h.by("cpu_runaway").size(), 1u);
    // Drops to 8 % of a core: resolved after 2 minutes.
    h.run(3 * 60, [](Sample& s, std::int64_t) { s.top.push_back(proc(9000, "spin.exe", 1.0, 50)); });
    CHECK(h.open_by("cpu_runaway") == nullptr);
    CHECK(h.wording_all_ok());
}

// ---------------------------------------------------------------------------------------------
TEST(guard_handle_and_thread_growth_per_process) {
    Harness h;
    h.warmup();
    h.run(20 * 60, [](Sample& s, std::int64_t el) {
        const double m = static_cast<double>(el) / kMinD;
        s.top.push_back(proc(3100, "handles.exe", 1.0, 100, static_cast<std::uint32_t>(20000 + 500 * m), 30));
        s.top.push_back(proc(3101, "threads.exe", 1.0, 100, 900, static_cast<std::uint32_t>(600 + 20 * m)));
        s.top.push_back(proc(3102, "bigflat.exe", 1.0, 100, 50000, 700));  // large but flat
    });
    const auto hg = h.by("handle_growth");
    CHECK_EQ(hg.size(), 1u);
    CHECK(!hg.empty() && has_contributor(hg[0], "handles.exe"));
    const auto tg = h.by("thread_growth");
    CHECK_EQ(tg.size(), 1u);
    CHECK(!tg.empty() && has_contributor(tg[0], "threads.exe"));
    CHECK(h.wording_all_ok());
}

TEST(guard_system_handle_exhaustion_needs_baseline) {
    // Without a baseline: no detection (no absolute rule for handles).
    Harness cold;
    cold.run(8 * 60, [](Sample& s, std::int64_t) { s.handles = 200000; });
    CHECK_EQ(cold.by("handle_exhaustion").size(), 0u);
    // With a baseline: 2x the median for 6 minutes fires; contributors moved with it.
    Harness h;
    h.warmup();
    h.run(8 * 60, [](Sample& s, std::int64_t el) {
        const double m = static_cast<double>(el) / kMinD;
        s.handles = static_cast<std::uint32_t>(200000 + 1000 * m);
        s.top.push_back(proc(3200, "hoarder.exe", 1.0, 100, static_cast<std::uint32_t>(90000 + 1000 * m), 30));
    });
    const auto* inc = h.open_by("handle_exhaustion");
    CHECK(inc != nullptr);
    if (inc) CHECK(has_contributor(*inc, "hoarder.exe"));
    // Short excursion (1 minute) elsewhere does not open thread_exhaustion.
    h.run(60, [](Sample& s, std::int64_t) { s.threads = 9000; });
    CHECK_EQ(h.by("thread_exhaustion").size(), 0u);
}

// ---------------------------------------------------------------------------------------------
TEST(guard_kernel_pool) {
    Harness h;
    h.warmup();
    h.run(60, [](Sample& s, std::int64_t) { s.kernel_nonpaged = static_cast<std::uint64_t>(1200 * kMB); });
    h.run(2 * 60);
    CHECK_EQ(h.by("kernel_pool").size(), 0u);  // 1-minute excursion
    h.run(6 * 60, [](Sample& s, std::int64_t) { s.kernel_nonpaged = static_cast<std::uint64_t>(1200 * kMB); });
    const auto* inc = h.open_by("kernel_pool");
    CHECK(inc != nullptr);
    if (inc) {
        CHECK_EQ(inc->key, std::string("kernel_pool|nonpaged"));
        CHECK(inc->confidence_level != Level::Low);
    }
    // Before a baseline exists only the absolute limit applies, with low confidence.
    Harness cold;
    cold.run(6 * 60, [](Sample& s, std::int64_t) { s.kernel_nonpaged = static_cast<std::uint64_t>(1200 * kMB); });
    CHECK_EQ(cold.by("kernel_pool").size(), 0u);
    cold.run(6 * 60, [](Sample& s, std::int64_t) { s.kernel_nonpaged = 4 * kGBu; });
    const auto* ci = cold.open_by("kernel_pool");
    CHECK(ci != nullptr);
    if (ci) CHECK(ci->confidence_level == Level::Low);
    CHECK(h.wording_all_ok() && cold.wording_all_ok());
}

// ---------------------------------------------------------------------------------------------
TEST(guard_storage_latency_fires_and_merges_repeats) {
    Harness h;
    h.warmup();
    bool reported = false;
    h.run(30, [](Sample& s, std::int64_t) { s.disk_read_latency_ms = 120; });
    reported = h.opened_reports > 0;
    CHECK(reported);
    const auto* inc = h.open_by("storage_latency");
    CHECK(inc != nullptr);
    if (inc) CHECK(inc->confidence_level != Level::Low);
    h.run(2 * 60);  // recovers (resolve after 60 s)
    CHECK(h.open_by("storage_latency") == nullptr);
    h.run(30, [](Sample& s, std::int64_t) { s.disk_write_latency_ms = 150; });
    const auto all = h.by("storage_latency");
    CHECK_EQ(all.size(), 1u);  // re-opened within the merge window, not duplicated
    CHECK(!all.empty() && all[0].occurrences == 2u && all[0].state == State::Open);
    CHECK(h.wording_all_ok());
}

TEST(guard_storage_latency_no_false_positives) {
    Harness h;
    h.warmup();
    // Single-sample spikes once a minute for 20 minutes.
    h.run(20 * 60, [](Sample& s, std::int64_t el) {
        if ((el / 1000) % 60 == 0) s.disk_read_latency_ms = 300;
    });
    // Sustained but below the absolute floor.
    h.run(5 * 60, [](Sample& s, std::int64_t) { s.disk_read_latency_ms = 40; });
    // High for 10 s, sleep for 5 minutes (gap), high for 14 s: never 20 s continuous.
    h.run(10, [](Sample& s, std::int64_t) { s.disk_read_latency_ms = 200; });
    h.ts += 5 * kMin;
    h.run(14, [](Sample& s, std::int64_t) { s.disk_read_latency_ms = 200; });
    CHECK_EQ(h.by("storage_latency").size(), 0u);
}

TEST(guard_confidence_lower_without_baseline) {
    Harness warm;
    warm.warmup();
    warm.run(40, [](Sample& s, std::int64_t) { s.disk_read_latency_ms = 120; });
    Harness cold;
    cold.run(40, [](Sample& s, std::int64_t) { s.disk_read_latency_ms = 120; });
    const auto* a = warm.open_by("storage_latency");
    CHECK(a != nullptr);
    const double ca = a ? a->confidence : 0.0;
    const auto* b = cold.open_by("storage_latency");
    CHECK(b != nullptr);
    if (b) {
        CHECK(b->confidence < ca);
        CHECK(b->confidence_level == Level::Low);
    }
}

// ---------------------------------------------------------------------------------------------
TEST(guard_dpc_isr) {
    Harness h;
    h.warmup();
    h.run(5 * 60, [](Sample& s, std::int64_t) { s.dpc_percent = 3.0; });  // elevated but below floor
    CHECK_EQ(h.by("dpc_isr").size(), 0u);
    h.run(20, [](Sample& s, std::int64_t) { s.interrupt_percent = 9.0; });  // too short
    h.run(60);
    CHECK_EQ(h.by("dpc_isr").size(), 0u);
    h.event(EventKind::DriverFailure, "netdrv.sys");
    h.run(40, [](Sample& s, std::int64_t) { s.dpc_percent = 8.0; });
    const auto* inc = h.open_by("dpc_isr");
    CHECK(inc != nullptr);
    if (inc) CHECK(has_contributor(*inc, "netdrv.sys"));
    CHECK(h.wording_all_ok());
}

TEST(guard_thermal_throttling) {
    Harness h;  // no baseline: absolute rule (<= 60 % while busy)
    h.run(5 * 60, [](Sample& s, std::int64_t) {  // low clocks while idle: normal power saving
        s.cpu_percent = 5;
        s.cpu_freq_percent = 40;
    });
    h.run(5 * 60, [](Sample& s, std::int64_t) {  // busy at full speed
        s.cpu_percent = 85;
        s.cpu_freq_percent = 105;
    });
    h.run(90, [](Sample& s, std::int64_t) {  // throttled, but only 90 s
        s.cpu_percent = 85;
        s.cpu_freq_percent = 50;
    });
    h.run(60);
    CHECK_EQ(h.by("thermal_throttling").size(), 0u);
    h.run(3 * 60, [](Sample& s, std::int64_t) {
        s.cpu_percent = 85;
        s.cpu_freq_percent = 50;
    });
    const auto* inc = h.open_by("thermal_throttling");
    CHECK(inc != nullptr);
    if (inc) {
        CHECK(contains(inc->summary, "correlated"));
        CHECK(inc->confidence_level == Level::Low);  // busy-time baseline not ready
    }
    CHECK(h.wording_all_ok());
}

// ---------------------------------------------------------------------------------------------
TEST(guard_service_crash_loop) {
    Harness h;
    h.event(EventKind::ServiceFailure, "Spooler");
    h.sparse(5);
    h.event(EventKind::ServiceFailure, "Spooler");
    CHECK_EQ(h.by("service_crash_loop").size(), 0u);  // 2 is not a loop
    h.event(EventKind::ServiceFailure, "OtherSvc");   // different service
    h.event(EventKind::ServiceFailure, "ThirdSvc");
    CHECK_EQ(h.by("service_crash_loop").size(), 0u);
    h.sparse(5);
    h.event(EventKind::ServiceFailure, "Spooler");  // third within the hour
    auto all = h.by("service_crash_loop");
    CHECK_EQ(all.size(), 1u);
    if (!all.empty()) {
        CHECK(has_contributor(all[0], "Spooler"));
        CHECK(all[0].severity == Severity::Error);
        CHECK(contains(all[0].title, "Spooler"));
    }
    h.event(EventKind::ServiceFailure, "Spooler");
    h.event(EventKind::ServiceFailure, "Spooler");
    all = h.by("service_crash_loop");
    CHECK_EQ(all.size(), 1u);  // repeats merged
    CHECK(!all.empty() && all[0].occurrences == 3u);
    // Quiet for an hour: resolved.
    h.sparse(65);
    CHECK(h.open_by("service_crash_loop") == nullptr);
    CHECK(h.wording_all_ok());
}

TEST(guard_service_failures_spread_out_do_not_loop) {
    Harness h;
    for (int i = 0; i < 4; ++i) {
        h.event(EventKind::ServiceFailure, "Spooler");
        h.sparse(70);
    }
    CHECK_EQ(h.by("service_crash_loop").size(), 0u);
}

TEST(guard_app_reliability_and_disk) {
    Harness h;
    h.event(EventKind::AppCrash, "game.exe");
    h.event(EventKind::AppHang, "game.exe");
    CHECK_EQ(h.by("app_reliability").size(), 0u);
    h.event(EventKind::AppCrash, "game.exe");
    CHECK_EQ(h.by("app_reliability").size(), 1u);

    h.event(EventKind::DiskError, "Disk0", 3);
    CHECK_EQ(h.by("disk_deterioration").size(), 0u);  // one warning is not deterioration
    h.sparse(60);
    h.event(EventKind::DiskError, "Disk0", 3);
    h.sparse(60);
    h.event(EventKind::DiskError, "Disk0", 3);
    auto d = h.by("disk_deterioration");
    CHECK_EQ(d.size(), 1u);
    CHECK(!d.empty() && d[0].risk == Level::High);

    Harness c;
    c.event(EventKind::DiskError, "Disk1", 1);  // a critical disk event opens immediately
    CHECK_EQ(c.by("disk_deterioration").size(), 1u);
    CHECK(h.wording_all_ok() && c.wording_all_ok());
}

TEST(guard_event_driven_severities) {
    Harness h;
    h.event(EventKind::Bugcheck, "", 1);
    h.event(EventKind::UnexpectedShutdown, "", 1);
    h.event(EventKind::DriverFailure, "storport.sys");
    h.event(EventKind::GpuTimeout, "");
    h.event(EventKind::Whea, "", 3);
    h.event(EventKind::ResourceExhaustion, "");
    h.event(EventKind::Thermal, "");
    h.event(EventKind::WindowsUpdate, "KB1");   // informational only
    h.event(EventKind::OpenSweepConfig, "mode");
    auto sev = [&](const char* det) {
        const auto v = h.by(det);
        return v.empty() ? Severity::Info : v[0].severity;
    };
    CHECK(sev("bugcheck") == Severity::Critical);
    CHECK(sev("unexpected_shutdown") == Severity::Error);
    CHECK(sev("driver_failure") == Severity::Error);
    CHECK(sev("gpu_timeout") == Severity::Warning);
    CHECK(sev("whea_error") == Severity::Warning);  // corrected error
    CHECK(sev("resource_exhaustion") == Severity::Error);
    CHECK(sev("thermal_event") == Severity::Warning);
    CHECK_EQ(h.store.size(), 7u);
    // Escalation on repeats / fatal errors.
    h.event(EventKind::GpuTimeout, "");
    h.event(EventKind::GpuTimeout, "");
    CHECK(sev("gpu_timeout") == Severity::Error);
    h.event(EventKind::Whea, "", 1);
    CHECK(sev("whea_error") == Severity::Critical);
    CHECK_EQ(h.by("gpu_timeout").size(), 1u);
    CHECK(h.wording_all_ok());
    // Score reflects the damage and stays in range.
    const auto sc = h.guard->score(h.gen.sample(h.ts), h.store);
    CHECK(sc.score >= 0 && sc.score < 50);
    CHECK(!sc.reasons.empty());
}

TEST(guard_boot_degradation) {
    Harness h;
    for (double v : {30000.0, 31000.0, 29000.0}) h.event(EventKind::BootPerformance, "", 4, v);
    CHECK_EQ(h.by("boot_degradation").size(), 0u);
    h.event(EventKind::BootPerformance, "", 4, 34000.0);  // slightly slower: normal variation
    CHECK_EQ(h.by("boot_degradation").size(), 0u);
    h.event(EventKind::BootPerformance, "", 4, 90000.0);
    const auto* inc = h.open_by("boot_degradation");
    CHECK(inc != nullptr);
    if (inc) CHECK(inc->evidence[0].observed == 90000.0);
    h.event(EventKind::BootPerformance, "", 4, 31000.0);
    CHECK(h.open_by("boot_degradation") == nullptr);
    CHECK(h.wording_all_ok());
}

// ---------------------------------------------------------------------------------------------
TEST(guard_process_tracking_bounded) {
    Harness h;
    std::uint32_t pid = 10000;
    h.run(20 * 60, [&](Sample& s, std::int64_t) {
        for (int i = 0; i < 5; ++i) {
            ++pid;
            s.top.push_back(proc(pid, "short.exe", 1.0, 10));
        }
    });
    CHECK(h.guard->tracked_processes() <= kMaxTrackedProcesses);
    CHECK(h.guard->tracked_processes() > 0u);
    CHECK_EQ(h.store.size(), 0u);
}

TEST(guard_stale_incidents_resolved_after_restart) {
    Harness h;
    Incident old;
    old.detector = "storage_latency";
    old.key = "storage_latency";
    old.title = "Storage latency spike";
    h.store.open_or_merge(old, h.ts);
    Incident ev;
    ev.detector = "service_crash_loop";
    ev.key = "service_crash_loop|X";
    ev.title = "Service X keeps failing";
    h.store.open_or_merge(ev, h.ts);
    h.run(32 * 60);  // normal operation, a fresh guard never refreshes these
    // Level-detector incident not refreshed for 30 min: resolved.
    CHECK(h.store.find_open("storage_latency") == nullptr);
    // Event incident: its own quiet period (1 h for service loops) applies.
    CHECK(h.store.find_open("service_crash_loop|X") != nullptr);
    h.sparse(30);
    CHECK(h.store.find_open("service_crash_loop|X") == nullptr);
}

TEST(guard_stability_score_function) {
    const auto clean = compute_stability_score({});
    CHECK_EQ(clean.score, 100);
    CHECK(clean.reasons.empty());
    ScoreInputs worst;
    worst.open_critical = 100;
    worst.open_error = 100;
    worst.open_warning = 100;
    worst.open_info = 100;
    worst.crashes_24h = 1000;
    worst.service_failures_24h = 1000;
    worst.shutdowns_7d = 1000;
    worst.whea_7d = 1000;
    worst.disk_errors_7d = 1000;
    worst.commit_percent = 99;
    worst.mem_load_percent = 99;
    const auto w = compute_stability_score(worst);
    CHECK_EQ(w.score, 0);
    CHECK(w.reasons.size() >= 10u);
    ScoreInputs one;
    one.open_warning = 1;
    ScoreInputs two = one;
    two.open_warning = 2;
    ScoreInputs err;
    err.open_error = 1;
    CHECK(compute_stability_score(one).score > compute_stability_score(two).score);
    CHECK(compute_stability_score(one).score > compute_stability_score(err).score);
    ScoreInputs nan_in;
    nan_in.commit_percent = std::nan("");
    nan_in.mem_load_percent = std::nan("");
    CHECK_EQ(compute_stability_score(nan_in).score, 100);
    ScoreInputs press;
    press.commit_percent = 96;
    const auto p = compute_stability_score(press);
    CHECK_EQ(p.score, 90);
    CHECK(!p.reasons.empty() && contains(p.reasons[0], "commit"));
}
