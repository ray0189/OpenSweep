// Kernel-trace detectors (kernel_guard): driver_latency, disk_hog, paging_hog. Synthetic samples
// carry the per-interval kernel data the ETW session would provide.
#include <algorithm>
#include <string>
#include <vector>

#include "commands2.hpp"
#include "engine_test_util.hpp"
#include "kernel_guard.hpp"
#include "test_framework.hpp"

using namespace opensweep;
using eng_test::kTick;
using kguard::KernelGuard;
using kguard::KernelInputs;
using telemetry::KernelDriverSample;
using telemetry::KernelProcessSample;
using telemetry::Sample;

namespace {

constexpr double kMBd = 1024.0 * 1024.0;
constexpr std::int64_t kT0 = eng_test::kT0;  // 40 s into a minute

Sample base_sample(std::int64_t ts) {
    Sample s = eng_test::quiet(ts);
    s.kernel_valid = true;
    s.kernel.running = true;
    return s;
}

KernelDriverSample drv(const char* name, double max_us, double total_us, std::uint64_t count = 10) {
    KernelDriverSample d;
    d.name = name;
    d.max_us = max_us;
    d.total_us = total_us;
    d.count = count;
    return d;
}

// Background sync client doing `bps` of disk I/O while disk latency is `lat` ms.
Sample disk_sample(std::int64_t ts, double bps, double lat, std::uint32_t pid = 300, const char* name = "OneDrive.exe") {
    Sample s = base_sample(ts);
    s.disk_read_latency_ms = lat;
    s.disk_write_latency_ms = lat;
    s.top.push_back(eng_test::proc(pid, name, 2.0, 150));
    KernelProcessSample k;
    k.pid = pid;
    k.name = name;
    k.read_bps = bps * 0.25;
    k.write_bps = bps * 0.75;
    k.mean_latency_ms = lat;
    k.max_latency_ms = lat * 3;
    s.kernel_processes.push_back(k);
    s.kernel.disk_bytes_per_sec = bps * 1.1;
    return s;
}

Sample fault_sample(std::int64_t ts, double faults, double total, std::uint32_t pid = 300,
                    const char* name = "OneDrive.exe") {
    Sample s = base_sample(ts);
    s.top.push_back(eng_test::proc(pid, name, 2.0, 900));
    KernelProcessSample k;
    k.pid = pid;
    k.name = name;
    k.hard_faults_per_sec = faults;
    s.kernel_processes.push_back(k);
    s.kernel.hard_faults_per_sec = total;
    return s;
}

struct Rig {
    KernelGuard g;
    baseline::BaselineSet b;
    incident::IncidentStore store;
    KernelInputs in;
    std::int64_t now = kT0;
    std::vector<policy::Recommendation> last_recs;
    std::vector<std::string> notes;

    explicit Rig(kguard::KernelGuardConfig c = cfg8()) : g(std::move(c)) {}
    static kguard::KernelGuardConfig cfg8() {
        kguard::KernelGuardConfig c;
        c.logical_processors = 8;
        return c;
    }
    template <typename G>
    void run(int n, G&& gen) {
        for (int i = 0; i < n; ++i) {
            now += kTick;
            auto a = g.evaluate(gen(now), b, store, in);
            last_recs = std::move(a.recommendations);
            if (!a.notes.empty()) notes = a.notes;
        }
    }
    const incident::Incident* open(const std::string& detector) const {
        const incident::Incident* found = nullptr;
        store.for_each_open([&](const incident::Incident& i) {
            if (i.detector == detector) found = store.find_id(i.id);
        });
        return found;
    }
    std::size_t count(const std::string& detector) const {
        std::size_t n = 0;
        for (const auto& i : store.list())
            if (i.detector == detector) ++n;
        return n;
    }
};

constexpr int kPerMinute = 30;  // samples per minute at 2 s

}  // namespace

// ---- driver_latency ---------------------------------------------------------------------------
TEST(kguard_driver_latency_fires_on_repeated_spikes) {
    Rig r;
    int minute_index = 0;
    // 12 minutes: spikes of 1500 us in minutes 1, 4 and 7 (one sample each), 120 us otherwise.
    for (int m = 0; m < 12; ++m, ++minute_index) {
        r.run(kPerMinute, [&](std::int64_t ts) {
            Sample s = base_sample(ts);
            const bool spike = (m == 1 || m == 4 || m == 7) && (ts / kTick) % kPerMinute == 3;
            s.kernel_drivers.push_back(drv("nvlddmkm.sys", spike ? 1500.0 : 120.0, 2000.0));
            s.kernel_drivers.push_back(drv("ndis.sys", 80.0, 500.0));
            return s;
        });
    }
    const incident::Incident* inc = r.open("driver_latency");
    CHECK(inc != nullptr);
    if (inc) {
        CHECK_EQ(inc->key, std::string("driver_latency|nvlddmkm.sys"));
        CHECK(inc->title.find("Driver nvlddmkm.sys correlated with latency spikes (max 1500 ") == 0);
        CHECK(incident::wording_ok(*inc));
        CHECK(!inc->auto_recovery_allowed);
        CHECK(!inc->evidence.empty());
        CHECK(inc->contributors.size() == 1 && inc->contributors[0].kind == "driver");
        bool update = false, power = false;
        for (const auto& a : inc->recommended_actions) {
            if (a.find("graphics driver (NVIDIA) nvlddmkm.sys") != std::string::npos) update = true;
            if (a.find("power") != std::string::npos) power = true;
        }
        CHECK(update);
        CHECK(power);
        CHECK(inc->confidence > 0.0);
    }
    CHECK_EQ(r.count("driver_latency"), 1u);  // ndis.sys stayed quiet
    // Recommendations only: drivers are never policy candidates.
    CHECK(r.last_recs.empty());

    // Quiet for 20 minutes: the 10-minute lookback empties, then 5 minutes of hysteresis.
    r.run(20 * kPerMinute, [&](std::int64_t ts) {
        Sample s = base_sample(ts);
        s.kernel_drivers.push_back(drv("nvlddmkm.sys", 100.0, 2000.0));
        return s;
    });
    CHECK(r.open("driver_latency") == nullptr);
}

TEST(kguard_driver_latency_ignores_short_spikes) {
    Rig r;
    // Two spike minutes (even very long ones) within 10 minutes: not "repeated".
    for (int m = 0; m < 15; ++m) {
        r.run(kPerMinute, [&](std::int64_t ts) {
            Sample s = base_sample(ts);
            const bool spike = (m == 2 || m == 3) && (ts / kTick) % kPerMinute == 3;
            s.kernel_drivers.push_back(drv("storport.sys", spike ? 8000.0 : 90.0, 1500.0));
            s.kernel_drivers.push_back(drv("unknown", 9000.0, 9000.0));  // unmapped routines never name a driver
            return s;
        });
    }
    CHECK(r.open("driver_latency") == nullptr);
    CHECK_EQ(r.count("driver_latency"), 0u);
}

TEST(kguard_driver_latency_share_above_baseline) {
    Rig r;
    // 40 minutes at ~0.05 % of 8 CPUs (8000 us per 2 s sample), then 3 minutes at ~2 %.
    r.run(40 * kPerMinute, [&](std::int64_t ts) {
        Sample s = base_sample(ts);
        s.kernel_drivers.push_back(drv("ndis.sys", 150.0, 8000.0));
        return s;
    });
    CHECK(r.open("driver_latency") == nullptr);
    r.run(3 * kPerMinute + 1, [&](std::int64_t ts) {
        Sample s = base_sample(ts);
        s.kernel_drivers.push_back(drv("ndis.sys", 200.0, 320000.0));
        return s;
    });
    const incident::Incident* inc = r.open("driver_latency");
    CHECK(inc != nullptr);
    if (inc) {
        CHECK(inc->summary.find("far above") != std::string::npos);
        CHECK(incident::wording_ok(*inc));
    }
}

TEST(kguard_no_kernel_data_no_state) {
    Rig r;
    r.run(10 * kPerMinute, [&](std::int64_t ts) {
        Sample s = disk_sample(ts, 60 * kMBd, 80.0);
        s.kernel_valid = false;
        return s;
    });
    CHECK_EQ(r.store.size(), 0u);
    CHECK_EQ(r.g.tracked_processes(), 0u);
}

// ---- disk_hog ---------------------------------------------------------------------------------
TEST(kguard_disk_hog_fires_and_recommends_background_candidate) {
    Rig r;
    r.run(70, [&](std::int64_t ts) { return disk_sample(ts, 30 * kMBd, 40.0); });  // 140 s
    const incident::Incident* inc = r.open("disk_hog");
    CHECK(inc != nullptr);
    if (inc) {
        CHECK_EQ(inc->key, std::string("disk_hog|OneDrive.exe|300"));
        CHECK(incident::wording_ok(*inc));
        CHECK(inc->title.find("correlated with") != std::string::npos);
        CHECK(!inc->contributors.empty() && inc->contributors[0].pid == 300);
    }
    CHECK_EQ(r.last_recs.size(), 1u);
    if (!r.last_recs.empty()) {
        const auto& rec = r.last_recs[0];
        CHECK(rec.area == policy::Area::Disk);
        CHECK(rec.kind == telemetry::PolicyKind::EcoQoS);
        CHECK_EQ(rec.value, 1);
        CHECK_EQ(rec.target.pid, 300u);
        CHECK_EQ(rec.target.process_start_ms, 1300);
        CHECK(rec.workload == telemetry::WorkloadClass::Background);
        CHECK_EQ(rec.beneficiary_pid, 100u);
        CHECK_EQ(rec.id, std::string("disk:ecoqos:300"));
    }
    // Escalation: EcoQoS already applied by OpenSweep -> BELOW_NORMAL.
    r.in.ecoqos_active = {300};
    r.run(1, [&](std::int64_t ts) { return disk_sample(ts, 30 * kMBd, 40.0); });
    CHECK(r.last_recs.size() == 1 && r.last_recs[0].kind == telemetry::PolicyKind::PriorityClass &&
          r.last_recs[0].value == policy::kPriorityBelowNormal);
    // Stops: resolves after 2 min below half the rate.
    r.run(65, [&](std::int64_t ts) { return disk_sample(ts, 1 * kMBd, 40.0); });
    CHECK(r.open("disk_hog") == nullptr);
    CHECK(r.last_recs.empty());
}

TEST(kguard_disk_hog_needs_sustained_io_and_slow_disk) {
    {
        Rig r;  // a 1-minute burst
        r.run(30, [&](std::int64_t ts) { return disk_sample(ts, 80 * kMBd, 60.0); });
        r.run(60, [&](std::int64_t ts) { return disk_sample(ts, 0.5 * kMBd, 2.0); });
        CHECK_EQ(r.count("disk_hog"), 0u);
    }
    {
        Rig r;  // heavy I/O but the disk keeps up (latency normal)
        r.run(120, [&](std::int64_t ts) { return disk_sample(ts, 80 * kMBd, 3.0); });
        CHECK_EQ(r.count("disk_hog"), 0u);
        CHECK(r.last_recs.empty());
    }
}

TEST(kguard_disk_hog_never_targets_foreground_or_protected) {
    {
        Rig r;  // the foreground app itself (e.g. a game loading levels): not a hog
        r.run(120, [&](std::int64_t ts) { return disk_sample(ts, 50 * kMBd, 50.0, 100, "word.exe"); });
        CHECK_EQ(r.count("disk_hog"), 0u);
        CHECK(r.last_recs.empty());
    }
    {
        Rig r;  // a protected process: incident (information) but never a candidate
        r.run(120, [&](std::int64_t ts) {
            Sample s = disk_sample(ts, 50 * kMBd, 50.0, 400, "MsMpEng.exe");
            s.top.back().is_security_software = true;
            return s;
        });
        CHECK(r.open("disk_hog") != nullptr);
        CHECK(r.last_recs.empty());
        bool noted = false;
        for (const auto& n : r.notes)
            if (n.find("security_software") != std::string::npos) noted = true;
        CHECK(noted);
    }
    {
        Rig r;  // unknown workload class (not verified background): no candidate
        r.run(120, [&](std::int64_t ts) { return disk_sample(ts, 50 * kMBd, 50.0, 500, "mystery.exe"); });
        CHECK(r.open("disk_hog") != nullptr);
        CHECK(r.last_recs.empty());
    }
    {
        Rig r;  // not in the sampled process list: cannot be verified
        r.run(120, [&](std::int64_t ts) {
            Sample s = disk_sample(ts, 50 * kMBd, 50.0);
            s.top.pop_back();
            return s;
        });
        CHECK(r.open("disk_hog") != nullptr);
        CHECK(r.last_recs.empty());
    }
}

// ---- paging_hog -------------------------------------------------------------------------------
TEST(kguard_paging_hog_during_memory_pressure) {
    Rig r;
    // An open memory incident to attach the contributor to.
    incident::Incident commit;
    commit.detector = "commit_exhaustion";
    commit.key = "commit_exhaustion";
    commit.title = "Commit charge close to the limit";
    r.store.open_or_merge(commit, kT0);
    r.in.memory_level = memory::PressureLevel::Significant;
    r.run(40, [&](std::int64_t ts) { return fault_sample(ts, 800.0, 1000.0); });  // 80 s
    const incident::Incident* inc = r.open("paging_hog");
    CHECK(inc != nullptr);
    if (inc) {
        CHECK(incident::wording_ok(*inc));
        CHECK(inc->title.find("OneDrive.exe correlated with most hard page faults") == 0);
    }
    const incident::Incident* c = r.store.find_open("commit_exhaustion");
    CHECK(c != nullptr);
    if (c) {
        CHECK_EQ(c->contributors.size(), 1u);
        if (!c->contributors.empty()) {
            CHECK_EQ(c->contributors[0].pid, 300u);
            CHECK_EQ(c->contributors[0].kind, std::string("process"));
        }
    }
    CHECK_EQ(r.last_recs.size(), 1u);
    if (!r.last_recs.empty()) {
        CHECK(r.last_recs[0].area == policy::Area::Memory);
        CHECK(r.last_recs[0].kind == telemetry::PolicyKind::MemoryPriority);
        CHECK_EQ(r.last_recs[0].value, policy::kMemoryPriorityLow);
    }
    // Pressure gone: resolves after 2 min, no more candidates.
    r.in.memory_level = memory::PressureLevel::Low;
    r.run(65, [&](std::int64_t ts) { return fault_sample(ts, 800.0, 1000.0); });
    CHECK(r.open("paging_hog") == nullptr);
    CHECK(r.last_recs.empty());
}

TEST(kguard_paging_hog_not_without_pressure_or_majority) {
    {
        Rig r;  // no memory pressure
        r.run(60, [&](std::int64_t ts) { return fault_sample(ts, 800.0, 1000.0); });
        CHECK_EQ(r.count("paging_hog"), 0u);
    }
    {
        Rig r;  // pressure, but the process has only a minority of the faults
        r.in.memory_level = memory::PressureLevel::Critical;
        r.run(60, [&](std::int64_t ts) { return fault_sample(ts, 300.0, 1000.0); });
        CHECK_EQ(r.count("paging_hog"), 0u);
    }
    {
        Rig r;  // pressure, majority, but foreground: incident, no candidate
        r.in.memory_level = memory::PressureLevel::Critical;
        r.run(60, [&](std::int64_t ts) { return fault_sample(ts, 900.0, 1000.0, 100, "word.exe"); });
        CHECK(r.open("paging_hog") != nullptr);
        CHECK(r.last_recs.empty());
    }
}

TEST(kguard_tracking_is_bounded) {
    Rig r;
    r.run(200, [&](std::int64_t ts) {
        Sample s = base_sample(ts);
        const auto base = static_cast<std::uint32_t>((ts / kTick) % 1000) * 10;
        for (std::uint32_t i = 0; i < 24; ++i) {
            KernelProcessSample k;
            k.pid = 10000 + base + i;
            k.name = "p" + std::to_string(k.pid) + ".exe";
            k.write_bps = 25 * kMBd;
            s.kernel_processes.push_back(k);
            s.kernel_drivers.push_back(drv(("d" + std::to_string(base + i) + ".sys").c_str(), 50.0, 50.0));
        }
        return s;
    });
    CHECK(r.g.tracked_processes() <= kguard::kMaxTrackedProcs);
    CHECK(r.g.tracked_drivers() <= kguard::kMaxTrackedDrivers);
}

// ---- engine integration: control path, Monitor = simulation only ------------------------------
TEST(kguard_engine_monitor_mode_never_acts) {
    eng_test::Rig rig;
    rig.act.start[300] = 1300;
    bool disk = false, would = false, other_outcome = false;
    for (int i = 0; i < 90; ++i) {
        rig.tick(disk_sample(rig.now + kTick, 30 * kMBd, 40.0));
        for (const auto& d : rig.eng->recommendations()) {
            if (d.rec.area != policy::Area::Disk) continue;
            disk = true;
            if (d.outcome != control::Outcome::Simulated) other_outcome = true;
            if (d.would_apply) {
                would = true;
                CHECK_EQ(d.blocked_by, std::string("monitor_mode"));
                CHECK_EQ(std::string(commands2::decision_to_json(d).find("area")->as_string()), std::string("disk"));
            }
        }
    }
    CHECK_EQ(rig.act.applies, 0);
    CHECK(disk);
    CHECK(would);  // what opting in would do is visible
    CHECK(!other_outcome);
    bool incident = false;
    for (const auto& i : rig.eng->incidents(50))
        if (i.detector == "disk_hog") incident = true;
    CHECK(incident);
}

TEST(kguard_engine_balanced_applies_journals_and_rolls_back) {
    eng_test::Rig rig;
    rig.act.start[300] = 1300;
    const auto mr = rig.eng->set_mode(control::Mode::Balanced, false, std::vector<telemetry::PolicyKind>{telemetry::PolicyKind::EcoQoS});
    CHECK(mr.ok);
    rig.run(90, [](std::int64_t ts) { return disk_sample(ts, 30 * kMBd, 40.0); });
    CHECK_EQ(rig.act.applies, 1);
    CHECK(rig.act.applied.size() == 1 && rig.act.applied[0].second == 300u &&
          rig.act.applied[0].first == telemetry::PolicyKind::EcoQoS);
    const auto pols = rig.eng->policies();
    CHECK_EQ(pols.size(), 1u);
    if (!pols.empty()) CHECK(pols[0].rec.area == policy::Area::Disk);
    bool journaled = false;
    for (const auto& t : rig.eng->journal(20))
        if (t.kind == "policy" && t.target_pid == 300) journaled = true;
    CHECK(journaled);
    // Disk latency did not improve within the verification window -> rolled back.
    rig.run(40, [](std::int64_t ts) { return disk_sample(ts, 30 * kMBd, 40.0); });
    CHECK(rig.act.restores >= 1);
    // Back to monitor: nothing else is applied.
    rig.eng->set_mode(control::Mode::Monitor);
    const int applies = rig.act.applies;
    rig.run(60, [](std::int64_t ts) { return disk_sample(ts, 30 * kMBd, 40.0); });
    CHECK_EQ(rig.act.applies, applies);
}

TEST(kguard_settings_etw_flag) {
    std::string err;
    auto v = json::parse(R"({"version":1,"mode":"monitor","simulation":false,"optIn":[]})");
    auto s = engine::settings_from_json(*v.value, err);
    CHECK(s && s->etw);  // older files: default on
    v = json::parse(R"({"version":1,"mode":"monitor","etw":false})");
    s = engine::settings_from_json(*v.value, err);
    CHECK(s && !s->etw);
    v = json::parse(R"({"version":1,"mode":"monitor","etw":"no"})");
    CHECK(!engine::settings_from_json(*v.value, err));
    engine::Settings st;
    st.etw = false;
    const auto j = engine::settings_to_json(st);
    CHECK(j.find("etw") && !j.find("etw")->as_bool());

    eng_test::Rig rig;
    CHECK(rig.eng->etw_enabled());
    CHECK(rig.eng->set_etw(false));
    CHECK(!rig.eng->etw_enabled());
    rig.make();  // reload from config.json
    CHECK(!rig.eng->etw_enabled());
    // A later setMode keeps the etw switch.
    rig.eng->set_mode(control::Mode::Monitor);
    rig.make();
    CHECK(!rig.eng->etw_enabled());
}
