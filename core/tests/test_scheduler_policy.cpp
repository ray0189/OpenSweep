#include <string>
#include <vector>

#include "scheduler_policy.hpp"
#include "test_framework.hpp"

using namespace opensweep;
using sched::HistoryView;
using telemetry::PolicyKind;
using telemetry::ProcessSample;
using telemetry::WorkloadClass;

namespace {

ProcessSample proc(std::uint32_t pid, const char* name, double cpu, std::uint32_t session = 1) {
    ProcessSample p;
    p.pid = pid;
    p.name = name;
    p.cpu_percent = cpu;
    p.session_id = session;
    p.priority_class = policy::kPriorityNormal;
    p.start_ms = 1000 + pid;
    return p;
}

HistoryView sustained(double cpu) {
    HistoryView h;
    h.samples = 10;
    h.avg_cpu = cpu;
    h.min_cpu = cpu;
    h.present = true;
    return h;
}

sched::Topology topo8(bool hybrid = false) {
    sched::Topology t;
    t.logical_count = 8;
    t.hybrid = hybrid;
    if (hybrid) t.efficiency_class = {1, 1, 1, 1, 0, 0, 0, 0};
    return t;
}

// A contended machine: fg app + a background indexer (verified background) eating CPU.
telemetry::Sample contended(double cpu = 95.0, double queue = 16.0) {
    telemetry::Sample s;
    s.cpu_percent = cpu;
    s.cpu_queue_length = queue;
    s.foreground_pid = 100;
    auto fg = proc(100, "word.exe", 20.0);
    fg.foreground = true;
    s.top.push_back(fg);
    s.top.push_back(proc(200, "SearchProtocolHost.exe", 40.0));  // verified background
    s.top.push_back(proc(300, "mystery.exe", 30.0));             // unknown
    auto av = proc(400, "MsMpEng.exe", 30.0, 0);
    av.is_security_software = true;
    av.is_service = true;
    s.top.push_back(av);
    return s;
}

}  // namespace

TEST(sched_normalize_image) {
    CHECK_EQ(sched::normalize_image("C:\\Program Files\\X\\MsMpEng.EXE"), std::string("msmpeng"));
    CHECK_EQ(sched::normalize_image("chrome.exe"), std::string("chrome"));
    CHECK_EQ(sched::normalize_image("Memory Compression"), std::string("memory compression"));
    CHECK(sched::is_builtin_protected_image("csrss.exe"));
    CHECK(sched::is_builtin_protected_image("MsMpEng.exe"));
    CHECK(sched::is_anti_cheat_image("EasyAntiCheat.exe"));
    CHECK(sched::is_anti_cheat_image("xigncode3.exe"));  // prefix entry
    CHECK(!sched::is_builtin_protected_image("onedrive.exe"));
}

TEST(sched_classification_table) {
    const sched::ClassifierLists lists;
    const HistoryView none;

    auto fg = proc(1, "notepad.exe", 1.0);
    fg.foreground = true;
    CHECK(sched::classify(fg, none, lists) == WorkloadClass::Interactive);
    auto fg_game = proc(2, "cs2.exe", 50.0);
    fg_game.foreground = true;
    CHECK(sched::classify(fg_game, none, lists) == WorkloadClass::LatencyCritical);
    auto audio = proc(3, "randomplayer.exe", 1.0);
    audio.has_audio = true;
    CHECK(sched::classify(audio, none, lists) == WorkloadClass::LatencyCritical);
    CHECK(sched::classify(proc(4, "Ableton Live 12 Suite.exe", 5.0), none, lists) == WorkloadClass::LatencyCritical);
    CHECK(sched::classify(proc(5, "vlc.exe", 5.0), none, lists) == WorkloadClass::LatencyCritical);
    CHECK(sched::classify(proc(6, "EasyAntiCheat.exe", 1.0), none, lists) == WorkloadClass::LatencyCritical);
    CHECK(sched::classify(proc(7, "anything.exe", 1.0), none, lists, /*self_ecoqos=*/true) == WorkloadClass::Efficiency);
    auto idle = proc(8, "anything.exe", 1.0);
    idle.priority_class = policy::kPriorityIdle;
    CHECK(sched::classify(idle, none, lists) == WorkloadClass::Efficiency);
    CHECK(sched::classify(proc(9, "cl.exe", 12.0), none, lists) == WorkloadClass::Throughput);
    CHECK(sched::classify(proc(10, "OneDrive.exe", 2.0), none, lists) == WorkloadClass::Background);
    auto svc = proc(11, "vendorupdater.exe", 2.0, 0);
    svc.is_service = true;
    CHECK(sched::classify(svc, none, lists) == WorkloadClass::Background);
    auto crit_svc = svc;
    crit_svc.is_system_critical = true;
    CHECK(sched::classify(crit_svc, none, lists) == WorkloadClass::Unknown);
    CHECK(sched::classify(proc(12, "mystery.exe", 30.0), sustained(30.0), lists) == WorkloadClass::Throughput);
    CHECK(sched::classify(proc(13, "mystery.exe", 30.0), none, lists) == WorkloadClass::Unknown);
    CHECK(sched::classify(proc(14, "mystery.exe", 1.0), sustained(1.0), lists) == WorkloadClass::Unknown);

    sched::ClassifierLists extra;
    extra.extra_background.push_back("MyBackup.exe");
    extra.extra_latency_critical.push_back("mysynth*");
    CHECK(sched::classify(proc(15, "mybackup.exe", 1.0), none, extra) == WorkloadClass::Background);
    CHECK(sched::classify(proc(16, "MySynthPro.exe", 1.0), none, extra) == WorkloadClass::LatencyCritical);
}

TEST(sched_protected_processes_never_targets) {
    const sched::ClassifierLists lists;
    auto fg = proc(1, "onedrive.exe", 1.0);
    fg.foreground = true;
    CHECK(sched::protection_reason(fg, WorkloadClass::Background, lists).value_or("") == "foreground");
    auto audio = proc(2, "onedrive.exe", 1.0);
    audio.has_audio = true;
    CHECK(sched::protection_reason(audio, WorkloadClass::Background, lists).value_or("") == "audio");
    CHECK(sched::protection_reason(proc(3, "csrss.exe", 1.0), WorkloadClass::Background, lists).value_or("") ==
          "protected_image");
    CHECK(sched::protection_reason(proc(44, "MsMpEng.exe", 1.0), WorkloadClass::Background, lists).value_or("") ==
          "protected_image");
    auto sec = proc(5, "vendoredr.exe", 1.0);
    sec.is_security_software = true;
    CHECK(sched::protection_reason(sec, WorkloadClass::Background, lists).value_or("") == "security_software");
    CHECK(sched::protection_reason(proc(6, "BEService.exe", 1.0), WorkloadClass::Background, lists).value_or("") ==
          "anti_cheat");
    CHECK(sched::protection_reason(proc(7, "mystery.exe", 1.0), WorkloadClass::Unknown, lists).value_or("") ==
          "unknown_workload");
    auto crit = proc(8, "onedrive.exe", 1.0);
    crit.is_system_critical = true;
    CHECK(sched::protection_reason(crit, WorkloadClass::Background, lists).value_or("") == "system_critical");
    CHECK(sched::protection_reason(proc(9, "vendorsvc.exe", 1.0, 0), WorkloadClass::Background, lists).value_or("") ==
          "session0_not_owned");
    CHECK(!sched::protection_reason(proc(10, "OpenSweepCore.exe", 1.0, 0), WorkloadClass::Background, lists));
    CHECK(sched::protection_reason(proc(0, "x", 1.0), WorkloadClass::Background, lists).value_or("") == "reserved_pid");
    CHECK(sched::protection_reason(proc(4, "System", 1.0), WorkloadClass::Background, lists).value_or("") == "reserved_pid");
    auto high = proc(11, "onedrive.exe", 1.0);
    high.priority_class = policy::kPriorityHigh;
    CHECK(sched::protection_reason(high, WorkloadClass::Background, lists).value_or("") == "elevated_priority");
    CHECK(!sched::protection_reason(proc(12, "onedrive.exe", 1.0), WorkloadClass::Background, lists));
    for (auto name : sched::builtin_protected_images())
        CHECK(sched::protection_reason(proc(50, std::string(name).c_str(), 1.0), WorkloadClass::Background, lists));
    for (auto name : {"explorer.exe", "dwm.exe", "audiodg.exe", "lsass.exe", "svchost.exe", "Registry",
                      "Memory Compression", "fontdrvhost.exe", "sihost.exe", "ctfmon.exe", "winlogon.exe",
                      "wininit.exe", "smss.exe", "services.exe"})
        CHECK(sched::is_builtin_protected_image(name));
}

TEST(sched_topology_efficiency_cores) {
    sched::Topology t;
    t.logical_count = 8;
    CHECK_EQ(t.efficient_cpu_count(), 0u);
    t.efficiency_class = {0, 0, 0, 0, 0, 0, 0, 0};
    t.hybrid = true;
    CHECK_EQ(t.efficient_cpu_count(), 0u);  // homogeneous despite the flag
    t.efficiency_class = {1, 1, 0, 0, 0, 0, 1, 1};
    CHECK_EQ(t.efficient_cpu_count(), 4u);
    t.hybrid = false;
    CHECK_EQ(t.efficient_cpu_count(), 0u);
}

TEST(sched_high_cpu_without_queue_is_no_action) {
    sched::SweepScheduler s;
    sched::SchedulerAssessment a;
    for (int i = 0; i < 30; ++i) a = s.assess(contended(100.0, 2.0), topo8(), {});  // 0.25 per cpu
    CHECK(!a.contention);
    CHECK(a.recommendations.empty());
    CHECK(a.reason.find("without runnable-thread pressure") != std::string::npos);
}

TEST(sched_queue_without_cpu_is_no_action) {
    sched::SweepScheduler s;
    sched::SchedulerAssessment a;
    for (int i = 0; i < 30; ++i) a = s.assess(contended(40.0, 30.0), topo8(), {});
    CHECK(!a.contention);
    CHECK(a.recommendations.empty());
}

TEST(sched_contention_must_be_sustained) {
    sched::SweepScheduler s;
    sched::SchedulerInputs in;
    in.fg_latency_ms = 120.0;
    for (int i = 0; i < 4; ++i) {
        auto a = s.assess(contended(), topo8(), in);
        CHECK(!a.contention);
        CHECK(a.recommendations.empty());
    }
    // one quiet sample resets the counter
    s.assess(contended(10.0, 0.0), topo8(), in);
    CHECK_EQ(s.contention_ticks(), 0);
}

TEST(sched_recommends_ecoqos_for_verified_background_only) {
    sched::SweepScheduler s;
    sched::SchedulerInputs in;
    in.fg_latency_ms = 120.0;
    sched::SchedulerAssessment a;
    for (int i = 0; i < 6; ++i) a = s.assess(contended(), topo8(), in);
    CHECK(a.contention);
    CHECK(a.foreground_impacted);
    CHECK_EQ(a.recommendations.size(), 1u);
    const auto& r = a.recommendations[0];
    CHECK_EQ(r.target.pid, 200u);
    CHECK(r.kind == PolicyKind::EcoQoS);
    CHECK_EQ(r.value, 1);
    CHECK(r.workload == WorkloadClass::Background);
    CHECK_EQ(r.beneficiary_pid, 100u);
    CHECK_EQ(r.target.process_start_ms, 1200);
    CHECK(!r.evidence.empty());
    CHECK(r.area == policy::Area::Cpu);
    // the protected / unknown processes are classified but never recommended
    for (const auto& c : a.classified) {
        if (c.pid == 100) CHECK_EQ(c.protected_by, std::string("foreground"));
        if (c.pid == 300) CHECK(c.cls == WorkloadClass::Throughput);  // sustained CPU, but never a target
        if (c.pid == 400) CHECK_EQ(c.protected_by, std::string("security_software"));
    }
}

TEST(sched_foreground_responsive_means_no_action) {
    sched::SweepScheduler s;
    sched::SchedulerInputs in;
    in.fg_latency_ms = 8.0;
    sched::SchedulerAssessment a;
    for (int i = 0; i < 10; ++i) a = s.assess(contended(), topo8(), in);
    CHECK(a.contention);
    CHECK(!a.foreground_impacted);
    CHECK(a.recommendations.empty());
}

TEST(sched_no_foreground_means_no_action) {
    sched::SweepScheduler s;
    auto smp = contended();
    smp.foreground_pid = 0;
    smp.top[0].foreground = false;
    smp.top[0].name = "mystery2.exe";
    sched::SchedulerAssessment a;
    for (int i = 0; i < 10; ++i) a = s.assess(smp, topo8(), {});
    CHECK(a.contention);
    CHECK(!a.foreground_present);
    CHECK(a.recommendations.empty());
}

TEST(sched_unknown_responsiveness_requires_severe_queue) {
    sched::SweepScheduler s;
    sched::SchedulerAssessment a;
    for (int i = 0; i < 10; ++i) a = s.assess(contended(95.0, 12.0), topo8(), {});  // 1.5 per cpu
    CHECK(a.contention);
    CHECK(a.recommendations.empty());
    for (int i = 0; i < 2; ++i) a = s.assess(contended(95.0, 20.0), topo8(), {});  // 2.5 per cpu
    CHECK_EQ(a.recommendations.size(), 1u);
}

TEST(sched_escalation_order) {
    sched::SchedulerInputs in;
    in.fg_latency_ms = 120.0;
    in.opensweep_ecoqos_pids = {200};
    {
        // non-hybrid / balanced: EcoQoS already applied -> BELOW_NORMAL, never CPU sets
        sched::SweepScheduler s;
        sched::SchedulerAssessment a;
        in.allow_cpu_sets = true;
        for (int i = 0; i < 6; ++i) a = s.assess(contended(), topo8(false), in);
        CHECK_EQ(a.recommendations.size(), 1u);
        CHECK(a.recommendations[0].kind == PolicyKind::PriorityClass);
        CHECK_EQ(a.recommendations[0].value, static_cast<std::int64_t>(policy::kPriorityBelowNormal));
    }
    {
        // hybrid + advanced: CPU sets to the efficiency class before priority
        sched::SweepScheduler s;
        sched::SchedulerAssessment a;
        for (int i = 0; i < 6; ++i) a = s.assess(contended(), topo8(true), in);
        CHECK_EQ(a.recommendations.size(), 1u);
        CHECK(a.recommendations[0].kind == PolicyKind::CpuSets);
        CHECK_EQ(a.recommendations[0].value, 4);
    }
    {
        // hybrid but not advanced: skip CPU sets
        sched::SweepScheduler s;
        sched::SchedulerAssessment a;
        in.allow_cpu_sets = false;
        for (int i = 0; i < 6; ++i) a = s.assess(contended(), topo8(true), in);
        CHECK(a.recommendations[0].kind == PolicyKind::PriorityClass);
    }
    {
        // everything applied and priority already lowered -> nothing more
        sched::SweepScheduler s;
        sched::SchedulerAssessment a;
        auto smp = contended();
        smp.top[1].priority_class = policy::kPriorityBelowNormal;
        for (int i = 0; i < 6; ++i) a = s.assess(smp, topo8(false), in);
        CHECK(a.recommendations.empty());
    }
}

TEST(sched_candidate_must_be_sustained_contributor) {
    sched::SweepScheduler s;
    sched::SchedulerInputs in;
    in.fg_latency_ms = 120.0;
    auto smp = contended();
    smp.top[1].cpu_percent = 2.0;  // 0.16 of one core on 8 logical: too small to matter
    sched::SchedulerAssessment a;
    for (int i = 0; i < 8; ++i) a = s.assess(smp, topo8(), in);
    CHECK(a.contention);
    CHECK(a.recommendations.empty());
}

TEST(sched_history_bounded_and_pid_reuse) {
    sched::SchedulerConfig cfg;
    cfg.max_tracked = 16;
    sched::SweepScheduler s(cfg);
    for (std::uint32_t round = 0; round < 10; ++round) {
        telemetry::Sample smp;
        for (std::uint32_t i = 0; i < 40; ++i) smp.top.push_back(proc(1000 + round * 40 + i, "x.exe", 1.0));
        s.assess(smp, topo8(), {});
        CHECK(s.tracked() <= 16u);
    }
    sched::SweepScheduler s2;
    telemetry::Sample a;
    a.top.push_back(proc(77, "a.exe", 50.0));
    for (int i = 0; i < 5; ++i) s2.assess(a, topo8(), {});
    CHECK_EQ(s2.history(77).samples, 5);
    telemetry::Sample b;
    b.top.push_back(proc(77, "b.exe", 1.0));  // PID reused by a different image
    s2.assess(b, topo8(), {});
    CHECK_EQ(s2.history(77).samples, 1);
    s2.assess(telemetry::Sample{}, topo8(), {});
    s2.assess(b, topo8(), {});  // gap -> consecutive history restarts
    CHECK_EQ(s2.history(77).samples, 1);
}
