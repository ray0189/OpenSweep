#include "memory_policy.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace opensweep::memory {

using telemetry::PolicyKind;
using telemetry::WorkloadClass;

namespace {

double clamp01(double v) noexcept {
    if (!std::isfinite(v)) return 0.0;
    return std::clamp(v, 0.0, 1.0);
}

double ramp(double v, double lo, double hi) noexcept { return clamp01((v - lo) / (hi - lo)); }

std::string fmt(double v, int decimals = 1) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
    return buf;
}

std::string mib(std::uint64_t b) { return std::to_string(b / (1024ull * 1024ull)) + " MiB"; }

bool contains(const std::vector<std::uint32_t>& v, std::uint32_t pid) {
    return std::find(v.begin(), v.end(), pid) != v.end();
}

constexpr double kMinFaults = 50.0;  // hard faults/s below this never count as pressure

}  // namespace

const char* to_string(PressureLevel l) noexcept {
    switch (l) {
        case PressureLevel::Low: return "low";
        case PressureLevel::Elevated: return "elevated";
        case PressureLevel::Significant: return "significant";
        case PressureLevel::Critical: return "critical";
    }
    return "low";
}

PressureScore pressure_score(const telemetry::Sample& s, const MemoryBaseline& b, const MemoryConfig& cfg) {
    PressureScore r;
    auto& c = r.c;
    if (s.mem_total > 0) {
        c.available_percent = static_cast<double>(s.mem_avail) * 100.0 / static_cast<double>(s.mem_total);
        c.compressed_percent = static_cast<double>(s.compressed_bytes) * 100.0 / static_cast<double>(s.mem_total);
    }
    if (s.commit_limit > 0)
        c.commit_percent = static_cast<double>(s.commit_total) * 100.0 / static_cast<double>(s.commit_limit);
    c.hard_faults = std::isfinite(s.hard_faults_per_sec) ? std::max(0.0, s.hard_faults_per_sec) : 0.0;
    c.paging_latency_ms = std::isfinite(s.disk_read_latency_ms) ? std::max(0.0, s.disk_read_latency_ms) : 0.0;

    c.s_available = s.mem_total > 0 ? ramp(25.0 - c.available_percent, 0.0, 20.0) : 0.0;  // 25% -> 0, 5% -> 1
    c.s_commit = ramp(c.commit_percent, 70.0, 95.0);
    if (c.hard_faults >= kMinFaults) {
        if (b.ready) {
            c.hard_fault_ratio = c.hard_faults / std::max(b.hard_faults_median, 20.0);
            c.s_faults = ramp(c.hard_fault_ratio, 2.0, 10.0);  // 2x baseline -> 0, 10x -> 1
        } else {
            c.s_faults = ramp(c.hard_faults, 200.0, 2000.0);
        }
        c.s_latency = ramp(c.paging_latency_ms, 15.0, 50.0);  // only meaningful while paging
    }
    c.s_compression = ramp(c.compressed_percent, 5.0, 20.0);

    r.score = cfg.w_available * c.s_available + cfg.w_commit * c.s_commit + cfg.w_faults * c.s_faults +
              cfg.w_compression * c.s_compression + cfg.w_latency * c.s_latency;
    r.score = std::clamp(r.score, 0.0, 100.0);
    return r;
}

PressureLevel level_for(double score, const MemoryConfig& cfg) noexcept {
    if (score >= cfg.critical) return PressureLevel::Critical;
    if (score >= cfg.significant) return PressureLevel::Significant;
    if (score >= cfg.elevated) return PressureLevel::Elevated;
    return PressureLevel::Low;
}

SweepMemory::SweepMemory(MemoryConfig cfg) : cfg_(std::move(cfg)) {}

PressureLevel SweepMemory::step_level(double score) {
    const PressureLevel raw = level_for(score, cfg_);
    if (raw > level_) {
        down_ticks_ = 0;
        if (++up_ticks_ >= cfg_.enter_ticks) {
            level_ = raw;
            up_ticks_ = 0;
        }
        return level_;
    }
    up_ticks_ = 0;
    const PressureLevel down_target = level_for(score + cfg_.hysteresis_margin, cfg_);
    if (down_target < level_) {
        if (++down_ticks_ >= cfg_.exit_ticks) {
            level_ = down_target;
            down_ticks_ = 0;
        }
    } else {
        down_ticks_ = 0;
    }
    return level_;
}

MemoryAssessment SweepMemory::assess(const telemetry::Sample& s, const MemoryInputs& in) {
    MemoryAssessment a;
    const PressureScore ps = pressure_score(s, in.baseline, cfg_);
    a.score = ps.score;
    a.components = ps.c;
    a.raw_level = level_for(ps.score, cfg_);
    a.level = step_level(ps.score);

    if (a.level == PressureLevel::Low) {
        a.reason = "memory pressure low; nothing to do";
        return a;
    }

    // Identify contributors (bounded), largest private bytes first.
    const std::uint64_t min_bytes = std::max<std::uint64_t>(
        cfg_.contributor_min_bytes,
        static_cast<std::uint64_t>(static_cast<double>(s.mem_total) * cfg_.contributor_min_fraction));
    std::vector<const telemetry::ProcessSample*> procs;
    procs.reserve(s.top.size());
    for (const auto& p : s.top)
        if (p.private_bytes >= min_bytes || contains(in.suspected_leak_pids, p.pid)) procs.push_back(&p);
    std::sort(procs.begin(), procs.end(),
              [](const auto* x, const auto* y) { return x->private_bytes > y->private_bytes; });
    if (procs.size() > cfg_.max_contributors) procs.resize(cfg_.max_contributors);
    for (const auto* p : procs) {
        telemetry::ProcessSample q = *p;
        if (s.foreground_pid != 0 && q.pid == s.foreground_pid) q.foreground = true;
        Contributor c;
        c.pid = q.pid;
        c.name = q.name;
        c.private_bytes = q.private_bytes;
        c.working_set = q.working_set;
        c.cls = sched::classify(q, sched::HistoryView{}, cfg_.lists, contains(in.self_ecoqos_pids, q.pid));
        c.suspected_leak = contains(in.suspected_leak_pids, q.pid);
        if (auto why = sched::protection_reason(q, c.cls, cfg_.lists)) c.protected_by = *why;
        if (c.protected_by.empty() && c.cls != WorkloadClass::Background) c.protected_by = "not_background";
        a.contributors.push_back(std::move(c));
    }

    std::vector<std::string> evidence;
    evidence.push_back("pressure score " + fmt(ps.score) + " (" + to_string(a.level) + ")");
    evidence.push_back("available " + fmt(ps.c.available_percent) + "%");
    evidence.push_back("commit " + fmt(ps.c.commit_percent) + "%");
    if (ps.c.hard_faults >= kMinFaults) {
        std::string e = "hard faults " + fmt(ps.c.hard_faults, 0) + "/s";
        if (in.baseline.ready) e += " (" + fmt(ps.c.hard_fault_ratio) + "x baseline)";
        evidence.push_back(std::move(e));
        evidence.push_back("paging read latency " + fmt(ps.c.paging_latency_ms) + " ms");
    }
    if (ps.c.compressed_percent > 0.0) evidence.push_back("compressed " + fmt(ps.c.compressed_percent) + "% of RAM");

    if (a.level == PressureLevel::Elevated) {
        a.reason = "memory pressure elevated; contributors identified, no intervention justified yet";
        return a;
    }

    // Significant or critical: conservative background recommendations only.
    std::size_t taken = 0;
    for (const auto& c : a.contributors) {
        if (taken >= cfg_.max_candidates) break;
        if (!c.protected_by.empty()) continue;
        ++taken;
        const telemetry::ProcessSample* src = nullptr;
        for (const auto& p : s.top)
            if (p.pid == c.pid) src = &p;
        auto make = [&](PolicyKind kind, std::int64_t value, const std::string& effect) {
            policy::Recommendation r;
            r.area = policy::Area::Memory;
            r.target.pid = c.pid;
            r.target.name = c.name;
            r.target.process_start_ms = src ? src->start_ms : 0;
            r.workload = c.cls;
            r.kind = kind;
            r.value = value;
            r.beneficiary_pid = s.foreground_pid;
            r.risk = 1;
            r.id = std::string("memory:") + telemetry::to_string(kind) + ":" + std::to_string(c.pid);
            r.reason = "memory pressure " + std::string(to_string(a.level)) + "; " + c.name +
                       " is a verified background contributor (" + mib(c.private_bytes) + " private)" +
                       (c.suspected_leak ? ", suspected leak" : "");
            r.evidence = evidence;
            r.expected_effect = effect;
            a.recommendations.push_back(std::move(r));
        };
        make(PolicyKind::MemoryPriority, policy::kMemoryPriorityLow,
             "the memory manager trims and repurposes this process's pages before the foreground's");
        if (cfg_.recommend_ecoqos && !contains(in.self_ecoqos_pids, c.pid))
            make(PolicyKind::EcoQoS, 1, "background allocation/CPU rate is reduced while pressure lasts");
    }

    if (a.level == PressureLevel::Critical) {
        Diagnostic d;
        d.summary = "Critical memory pressure (score " + fmt(ps.score) + "). OpenSweep will not terminate "
                    "processes, trim working sets or purge the standby list; the actions below are for you to decide.";
        d.evidence = evidence;
        for (const auto& c : a.contributors) {
            std::string line = c.name + " uses " + mib(c.private_bytes) + " private";
            if (c.suspected_leak) line += " and is growing steadily (suspected leak)";
            d.evidence.push_back(std::move(line));
            if (c.suspected_leak) d.recommended_actions.push_back("Save your work in " + c.name + " and restart it");
        }
        if (ps.c.commit_percent >= 90.0)
            d.recommended_actions.push_back("Commit charge is near the limit: close unused applications or let "
                                            "Windows manage the page file size");
        d.recommended_actions.push_back("Close applications you are not using");
        d.recommended_actions.push_back("If this happens regularly, review startup apps or consider more RAM");
        a.diagnostic = std::move(d);
    }

    a.reason = a.recommendations.empty()
                   ? std::string("memory pressure ") + to_string(a.level) + "; no verified background contributor to act on"
                   : std::string("memory pressure ") + to_string(a.level) + "; recommending background memory priority";
    return a;
}

}  // namespace opensweep::memory
