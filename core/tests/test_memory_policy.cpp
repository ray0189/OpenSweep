#include <cmath>
#include <string>

#include "memory_policy.hpp"
#include "test_framework.hpp"

using namespace opensweep;
using memory::PressureLevel;
using telemetry::PolicyKind;

namespace {

constexpr std::uint64_t GiB = 1024ull * 1024 * 1024;
constexpr std::uint64_t MiB = 1024ull * 1024;

telemetry::ProcessSample proc(std::uint32_t pid, const char* name, std::uint64_t priv) {
    telemetry::ProcessSample p;
    p.pid = pid;
    p.name = name;
    p.private_bytes = priv;
    p.session_id = 1;
    p.priority_class = policy::kPriorityNormal;
    p.start_ms = 5000 + pid;
    return p;
}

// avail_pct of 16 GiB, commit_pct of a 32 GiB limit
telemetry::Sample mem(double avail_pct, double commit_pct, double hard_faults = 0.0, double compressed_pct = 0.0,
                      double latency_ms = 0.0) {
    telemetry::Sample s;
    s.mem_total = 16 * GiB;
    s.mem_avail = static_cast<std::uint64_t>(static_cast<double>(s.mem_total) * avail_pct / 100.0);
    s.commit_limit = 32 * GiB;
    s.commit_total = static_cast<std::uint64_t>(static_cast<double>(s.commit_limit) * commit_pct / 100.0);
    s.hard_faults_per_sec = hard_faults;
    s.compressed_bytes = static_cast<std::uint64_t>(static_cast<double>(s.mem_total) * compressed_pct / 100.0);
    s.disk_read_latency_ms = latency_ms;
    s.foreground_pid = 100;
    auto fg = proc(100, "chrome.exe", 4 * GiB);
    fg.foreground = true;
    s.top.push_back(fg);
    s.top.push_back(proc(200, "OneDrive.exe", 1 * GiB));  // verified background contributor
    s.top.push_back(proc(300, "mystery.exe", 2 * GiB));   // unknown -> never acted on
    auto dwm = proc(400, "dwm.exe", 900 * MiB);
    s.top.push_back(dwm);
    s.top.push_back(proc(500, "smallbg.exe", 10 * MiB));
    return s;
}

}  // namespace

TEST(mem_score_components) {
    const memory::MemoryConfig cfg;
    const memory::MemoryBaseline nb;
    // plenty of memory -> 0
    CHECK(memory::pressure_score(mem(60, 40), nb, cfg).score < 1.0);
    // available 5% -> full availability component
    auto s1 = memory::pressure_score(mem(5, 40), nb, cfg);
    CHECK(std::fabs(s1.score - cfg.w_available) < 0.01);
    CHECK(std::fabs(s1.c.available_percent - 5.0) < 0.01);
    // commit 95% -> full commit component
    auto s2 = memory::pressure_score(mem(60, 95), nb, cfg);
    CHECK(std::fabs(s2.score - cfg.w_commit) < 0.01);
    // hard faults vs baseline: 10x median saturates; latency counts only while paging
    memory::MemoryBaseline b;
    b.ready = true;
    b.hard_faults_median = 100.0;
    auto s3 = memory::pressure_score(mem(60, 40, 1000.0, 0, 60.0), b, cfg);
    CHECK(std::fabs(s3.c.hard_fault_ratio - 10.0) < 1e-9);
    CHECK(std::fabs(s3.score - (cfg.w_faults + cfg.w_latency)) < 0.01);
    auto s4 = memory::pressure_score(mem(60, 40, 10.0, 0, 60.0), b, cfg);  // tiny fault rate: latency ignored
    CHECK(s4.score < 0.01);
    // baseline-relative: same absolute faults on a machine that always pages that much is not pressure
    memory::MemoryBaseline heavy;
    heavy.ready = true;
    heavy.hard_faults_median = 600.0;
    CHECK(memory::pressure_score(mem(60, 40, 1000.0), heavy, cfg).c.s_faults < 0.01);
    // compression 20% -> full compression component
    auto s5 = memory::pressure_score(mem(60, 40, 0, 20.0), nb, cfg);
    CHECK(std::fabs(s5.score - cfg.w_compression) < 0.01);
    // everything maxed -> 100, clamped
    auto s6 = memory::pressure_score(mem(1, 99, 5000.0, 30.0, 200.0), nb, cfg);
    CHECK(std::fabs(s6.score - 100.0) < 0.01);
    // zero/garbage input is safe
    telemetry::Sample empty;
    empty.hard_faults_per_sec = std::nan("");
    CHECK(memory::pressure_score(empty, nb, cfg).score == 0.0);
}

TEST(mem_level_thresholds) {
    const memory::MemoryConfig cfg;
    CHECK(memory::level_for(10, cfg) == PressureLevel::Low);
    CHECK(memory::level_for(35, cfg) == PressureLevel::Elevated);
    CHECK(memory::level_for(60, cfg) == PressureLevel::Significant);
    CHECK(memory::level_for(80, cfg) == PressureLevel::Critical);
    CHECK_EQ(std::string(memory::to_string(PressureLevel::Critical)), std::string("critical"));
}

TEST(mem_low_does_nothing) {
    memory::SweepMemory m;
    memory::MemoryAssessment a;
    for (int i = 0; i < 5; ++i) a = m.assess(mem(60, 40), {});
    CHECK(a.level == PressureLevel::Low);
    CHECK(a.contributors.empty());
    CHECK(a.recommendations.empty());
    CHECK(!a.diagnostic);
}

TEST(mem_hysteresis) {
    memory::SweepMemory m;  // enter 2 ticks, exit 3 ticks, margin 10
    // avail 4% + commit 96% -> score 60 -> significant
    auto a = m.assess(mem(4, 96), {});
    CHECK(a.raw_level == PressureLevel::Significant);
    CHECK(a.level == PressureLevel::Low);  // not yet: needs 2 consecutive
    a = m.assess(mem(4, 96), {});
    CHECK(a.level == PressureLevel::Significant);
    // dip slightly below the threshold (but within the margin) -> stays significant
    for (int i = 0; i < 6; ++i) a = m.assess(mem(7, 93), {});  // ~54
    CHECK(a.score < 60.0);
    CHECK(a.level == PressureLevel::Significant);
    // well below: needs 3 consecutive samples
    a = m.assess(mem(60, 40), {});
    CHECK(a.level == PressureLevel::Significant);
    a = m.assess(mem(60, 40), {});
    CHECK(a.level == PressureLevel::Significant);
    a = m.assess(mem(60, 40), {});
    CHECK(a.level == PressureLevel::Low);
    // a single spike does not raise the level; an interruption resets the count
    a = m.assess(mem(4, 96), {});
    a = m.assess(mem(60, 40), {});
    a = m.assess(mem(4, 96), {});
    CHECK(a.level == PressureLevel::Low);
}

TEST(mem_elevated_identifies_contributors_only) {
    memory::SweepMemory m;
    memory::MemoryAssessment a;
    for (int i = 0; i < 3; ++i) a = m.assess(mem(6, 72, 0, 10.0), {});  // ~38.6
    CHECK(a.level == PressureLevel::Elevated);
    CHECK(!a.contributors.empty());
    CHECK(a.recommendations.empty());
    CHECK(!a.diagnostic);
    // sorted by private bytes, small processes excluded
    CHECK_EQ(a.contributors[0].pid, 100u);
    for (const auto& c : a.contributors) CHECK(c.pid != 500u);
}

TEST(mem_significant_recommends_background_only) {
    memory::SweepMemory m;
    memory::MemoryAssessment a;
    for (int i = 0; i < 3; ++i) a = m.assess(mem(4, 96, 0, 5.0), {});
    CHECK(a.level == PressureLevel::Significant);
    CHECK(!a.recommendations.empty());
    bool memprio = false;
    for (const auto& r : a.recommendations) {
        CHECK_EQ(r.target.pid, 200u);  // only OneDrive: fg, unknown and dwm are protected
        CHECK(r.area == policy::Area::Memory);
        CHECK(r.workload == telemetry::WorkloadClass::Background);
        CHECK_EQ(r.target.process_start_ms, 5200);
        if (r.kind == PolicyKind::MemoryPriority) {
            memprio = true;
            CHECK_EQ(r.value, policy::kMemoryPriorityLow);
        } else {
            CHECK(r.kind == PolicyKind::EcoQoS);
        }
    }
    CHECK(memprio);
    for (const auto& c : a.contributors) {
        if (c.pid == 100) CHECK_EQ(c.protected_by, std::string("foreground"));
        if (c.pid == 300) CHECK_EQ(c.protected_by, std::string("unknown_workload"));
        if (c.pid == 400) CHECK_EQ(c.protected_by, std::string("protected_image"));
    }
    CHECK(!a.diagnostic);
}

TEST(mem_critical_diagnostic_never_kills) {
    memory::SweepMemory m;
    memory::MemoryInputs in;
    in.suspected_leak_pids = {300};
    memory::MemoryAssessment a;
    for (int i = 0; i < 3; ++i) a = m.assess(mem(2, 98, 3000.0, 15.0, 40.0), in);
    CHECK(a.level == PressureLevel::Critical);
    CHECK(a.diagnostic.has_value());
    CHECK(!a.diagnostic->recommended_actions.empty());
    CHECK(a.diagnostic->summary.find("will not terminate") != std::string::npos);
    bool leak_mentioned = false;
    for (const auto& e : a.diagnostic->evidence)
        if (e.find("mystery.exe") != std::string::npos && e.find("leak") != std::string::npos) leak_mentioned = true;
    CHECK(leak_mentioned);
    // the suspected leak is unknown workload -> still never a policy target
    for (const auto& r : a.recommendations) CHECK(r.target.pid != 300u);
    // only reversible allow-listed kinds are ever produced
    for (const auto& r : a.recommendations)
        CHECK(r.kind == PolicyKind::MemoryPriority || r.kind == PolicyKind::EcoQoS);
}

TEST(mem_leak_listed_small_process_is_contributor) {
    memory::SweepMemory m;
    memory::MemoryInputs in;
    in.suspected_leak_pids = {500};
    memory::MemoryAssessment a;
    for (int i = 0; i < 3; ++i) a = m.assess(mem(4, 96), in);
    bool found = false;
    for (const auto& c : a.contributors)
        if (c.pid == 500) found = c.suspected_leak;
    CHECK(found);
}

TEST(mem_verification_metrics) {
    auto s = mem(50, 50, 120.0);
    s.cpu_queue_length = 8.0;
    auto m = policy::verification_metrics(s, 4, 33.0);
    CHECK(std::fabs(m.commit_percent - 50.0) < 0.01);
    CHECK(std::fabs(m.hard_faults - 120.0) < 1e-9);
    CHECK(std::fabs(m.cpu_queue - 2.0) < 1e-9);
    CHECK(std::fabs(m.fg_latency_ms - 33.0) < 1e-9);
    auto u = policy::verification_metrics(telemetry::Sample{}, 0, std::nullopt);
    CHECK(u.fg_latency_ms < 0.0);
    CHECK(u.commit_percent < 0.0);
}
