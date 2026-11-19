#include "scheduler_policy.hpp"

#include <algorithm>
#include <cstdio>

namespace opensweep::sched {

using telemetry::PolicyKind;

namespace {

constexpr std::string_view kProtected[] = {
    "csrss", "wininit", "winlogon", "lsass", "lsaiso", "services", "smss", "svchost", "dwm", "audiodg",
    "msmpeng", "nissrv", "mssense", "securityhealthservice", "securityhealthsystray", "smartscreen",
    "explorer", "system", "registry", "memory compression", "fontdrvhost", "sihost", "ctfmon", "idle",
    "secure system", "logonui", "userinit",
};

constexpr std::string_view kAntiCheat[] = {
    "easyanticheat", "easyanticheat_eos", "easyanticheat_launcher", "beservice", "beservice_x64",
    "bedaisy", "vgc", "vgtray", "faceit", "faceitservice", "faceitclient", "eaanticheat.gameservice",
    "gamemon", "gamemon64", "xigncode*", "xcoronahost", "pnkbstra", "pnkbstrb", "ricochet*",
    "mhyprot*", "atvi-randgrid_sr", "esportal*", "nprotect*", "gameguard*", "zakynthos*",
};

constexpr std::string_view kLatencyCritical[] = {
    // games (illustrative; the anti-cheat and foreground signals cover the rest)
    "cs2", "csgo", "valorant-win64-shipping", "fortniteclient-win64-shipping", "r5apex", "r5apex_dx12",
    "eldenring", "cyberpunk2077", "gta5", "gta5_enhanced", "rdr2", "overwatch", "dota2", "league of legends",
    "rocketleague", "destiny2", "witcher3", "starfield", "bf2042", "cod", "minecraft.windows",
    // media playback / capture
    "vlc", "mpc-hc64", "mpc-hc", "mpc-be64", "potplayermini64", "wmplayer", "video.ui", "microsoft.media.player",
    "spotify", "obs64", "obs32", "streamlabs obs", "zoom", "ms-teams", "teams",
    // DAWs / pro audio
    "ableton live*", "reaper", "fl64", "fl", "cubase*", "studio one", "bitwig studio", "protools",
    "audacity", "voicemeeter*", "nuendo*",
};

constexpr std::string_view kBackground[] = {
    "searchprotocolhost", "searchfilterhost", "searchindexer", "onedrive", "onedrivestandaloneupdater",
    "dropbox", "googledrivefs", "compattelrunner", "backgroundtaskhost", "mousocoreworker", "tiworker",
    "microsoftedgeupdate", "googleupdate", "officeclicktorun", "adobearm", "bzserv", "crashplanservice",
    "steamservice", "nvdisplay.container", "wsappx", "musnotification", "devicecensus",
};

constexpr std::string_view kThroughput[] = {
    "cl", "link", "msbuild", "clang", "clang++", "clang-cl", "gcc", "g++", "cc1plus", "rustc", "cargo",
    "javac", "ffmpeg", "handbrake", "handbrakecli", "x264", "x265", "7z", "7zg", "winrar", "rar",
    "blender", "ninja", "node", "dotnet",
};

char lower(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool matches(std::string_view name, std::string_view entry) noexcept {
    if (!entry.empty() && entry.back() == '*') {
        const std::string_view prefix = entry.substr(0, entry.size() - 1);
        return name.substr(0, prefix.size()) == prefix;
    }
    return name == entry;
}

bool in_builtin(std::string_view norm, std::span<const std::string_view> list) noexcept {
    for (const auto& e : list)
        if (matches(norm, e)) return true;
    return false;
}

bool in_extra(std::string_view norm, const std::vector<std::string>& list) {
    for (const auto& e : list) {
        const std::string n = normalize_image(e);
        // normalize_image strips a trailing ".exe" but keeps '*'
        if (matches(norm, n)) return true;
    }
    return false;
}

bool contains(const std::vector<std::uint32_t>& v, std::uint32_t pid) {
    return std::find(v.begin(), v.end(), pid) != v.end();
}

std::string fmt(double v, int decimals = 1) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
    return buf;
}

}  // namespace

std::string normalize_image(std::string_view name) {
    const auto slash = name.find_last_of("\\/");
    if (slash != std::string_view::npos) name = name.substr(slash + 1);
    std::string out;
    out.reserve(name.size());
    for (char c : name) out.push_back(lower(c));
    if (out.size() > 4 && out.compare(out.size() - 4, 4, ".exe") == 0) out.resize(out.size() - 4);
    return out;
}

std::span<const std::string_view> builtin_protected_images() noexcept { return kProtected; }
std::span<const std::string_view> anti_cheat_images() noexcept { return kAntiCheat; }
std::span<const std::string_view> latency_critical_images() noexcept { return kLatencyCritical; }
std::span<const std::string_view> background_images() noexcept { return kBackground; }
std::span<const std::string_view> throughput_images() noexcept { return kThroughput; }

bool is_builtin_protected_image(std::string_view name) { return in_builtin(normalize_image(name), kProtected); }
bool is_anti_cheat_image(std::string_view name) { return in_builtin(normalize_image(name), kAntiCheat); }

WorkloadClass classify(const ProcessSample& p, const HistoryView& h, const ClassifierLists& lists,
                       bool self_ecoqos, double sustained_cpu_floor, int min_samples) {
    const std::string n = normalize_image(p.name);
    const bool latency_image = in_builtin(n, kLatencyCritical) || in_builtin(n, kAntiCheat) ||
                               in_extra(n, lists.extra_latency_critical);
    if (p.foreground) return latency_image ? WorkloadClass::LatencyCritical : WorkloadClass::Interactive;
    if (p.has_audio) return WorkloadClass::LatencyCritical;
    if (latency_image) return WorkloadClass::LatencyCritical;
    if (self_ecoqos || p.priority_class == policy::kPriorityIdle) return WorkloadClass::Efficiency;
    if (in_builtin(n, kThroughput) || in_extra(n, lists.extra_throughput)) return WorkloadClass::Throughput;
    if (in_builtin(n, kBackground) || in_extra(n, lists.extra_background)) return WorkloadClass::Background;
    if (p.is_service && !p.is_system_critical && !p.is_security_software) return WorkloadClass::Background;
    if (h.present && h.samples >= min_samples && h.min_cpu >= sustained_cpu_floor) return WorkloadClass::Throughput;
    return WorkloadClass::Unknown;
}

std::optional<std::string> protection_reason(const ProcessSample& p, WorkloadClass cls, const ClassifierLists& lists) {
    if (p.pid == 0 || p.pid == 4) return "reserved_pid";
    if (p.foreground) return "foreground";
    if (p.is_system_critical) return "system_critical";
    if (p.is_security_software) return "security_software";
    if (p.has_audio) return "audio";
    const std::string n = normalize_image(p.name);
    if (n.empty()) return "unnamed_process";
    if (in_builtin(n, kAntiCheat)) return "anti_cheat";
    if (in_builtin(n, kProtected) || in_extra(n, lists.extra_protected)) return "protected_image";
    if (p.session_id == 0 && !in_extra(n, lists.owned_images)) return "session0_not_owned";
    switch (cls) {
        case WorkloadClass::Unknown: return "unknown_workload";
        case WorkloadClass::LatencyCritical: return "latency_critical";
        case WorkloadClass::Interactive: return "interactive";
        case WorkloadClass::Efficiency: return "already_efficient";
        case WorkloadClass::Throughput:
        case WorkloadClass::Background: break;
    }
    if (p.priority_class == policy::kPriorityHigh || p.priority_class == policy::kPriorityRealtime ||
        p.priority_class == policy::kPriorityAboveNormal)
        return "elevated_priority";  // someone raised it on purpose; leave it alone
    return std::nullopt;
}

std::uint32_t Topology::efficient_cpu_count() const noexcept {
    if (!hybrid || efficiency_class.empty()) return 0;
    const auto [mn, mx] = std::minmax_element(efficiency_class.begin(), efficiency_class.end());
    if (*mn == *mx) return 0;  // not actually heterogeneous
    return static_cast<std::uint32_t>(std::count(efficiency_class.begin(), efficiency_class.end(), *mn));
}

SweepScheduler::SweepScheduler(SchedulerConfig cfg) : cfg_(std::move(cfg)) {}

void SweepScheduler::record(const ProcessSample& p) {
    auto& h = hist_[p.pid];
    const bool reused = !h.name.empty() &&
                        (h.name != p.name || (h.start_ms != 0 && p.start_ms != 0 && h.start_ms != p.start_ms));
    const bool gap = h.count > 0 && h.last_tick + 1 != tick_;
    if (reused || gap || h.name.empty()) {
        h = History{};
        h.name = p.name;
        h.start_ms = p.start_ms;
    }
    if (h.last_tick == tick_ && h.count > 0) return;  // duplicate pid in one sample
    h.cpu[h.next] = static_cast<float>(p.cpu_percent);
    h.next = static_cast<std::uint8_t>((h.next + 1) % kHistoryLen);
    if (h.count < kHistoryLen) ++h.count;
    h.last_tick = tick_;
}

void SweepScheduler::prune() {
    for (auto it = hist_.begin(); it != hist_.end();) {
        if (tick_ - it->second.last_tick > cfg_.forget_after_ticks)
            it = hist_.erase(it);
        else
            ++it;
    }
    while (hist_.size() > cfg_.max_tracked) {
        auto oldest = hist_.begin();
        for (auto it = hist_.begin(); it != hist_.end(); ++it)
            if (it->second.last_tick < oldest->second.last_tick) oldest = it;
        hist_.erase(oldest);
    }
}

HistoryView SweepScheduler::view(const History& h) const {
    HistoryView v;
    v.samples = h.count;
    v.present = h.last_tick == tick_ && h.count > 0;
    if (h.count == 0) return v;
    double sum = 0.0;
    double mn = 1e300;
    for (std::size_t i = 0; i < h.count; ++i) {
        const double c = h.cpu[(h.next + kHistoryLen - 1 - i) % kHistoryLen];
        sum += c;
        mn = std::min(mn, c);
    }
    v.avg_cpu = sum / h.count;
    v.min_cpu = mn;
    return v;
}

HistoryView SweepScheduler::history(std::uint32_t pid) const {
    const auto it = hist_.find(pid);
    return it == hist_.end() ? HistoryView{} : view(it->second);
}

SchedulerAssessment SweepScheduler::assess(const telemetry::Sample& s, const Topology& topo, const SchedulerInputs& in) {
    ++tick_;
    for (const auto& p : s.top) record(p);
    prune();

    SchedulerAssessment a;
    const double logical = static_cast<double>(std::max<std::uint32_t>(1, topo.logical_count));
    a.cpu_percent = s.cpu_percent;
    a.queue_per_cpu = s.cpu_queue_length / logical;
    const bool cpu_hot = s.cpu_percent >= cfg_.contention_cpu_percent;
    const bool queue_hot = a.queue_per_cpu >= cfg_.contention_queue_per_cpu;
    contention_ticks_ = (cpu_hot && queue_hot) ? contention_ticks_ + 1 : 0;
    a.contention_ticks = contention_ticks_;
    a.contention = contention_ticks_ >= cfg_.sustain_ticks;

    // Per-core share expressed as % of the whole machine.
    const double core_floor = cfg_.candidate_core_fraction * 100.0 / logical;

    std::uint32_t fg_pid = s.foreground_pid;
    a.classified.reserve(s.top.size());
    struct Cand {
        const ProcessSample* p;
        WorkloadClass cls;
        HistoryView h;
    };
    std::vector<Cand> cands;
    for (const auto& p : s.top) {
        if (p.foreground && fg_pid == 0) fg_pid = p.pid;
        ProcessSample copy_flags = p;
        if (fg_pid != 0 && p.pid == fg_pid) copy_flags.foreground = true;
        const HistoryView h = history(p.pid);
        const WorkloadClass cls = classify(copy_flags, h, cfg_.lists, contains(in.self_ecoqos_pids, p.pid),
                                           core_floor, cfg_.min_history);
        auto prot = protection_reason(copy_flags, cls, cfg_.lists);
        ProcessClassification pc;
        pc.pid = p.pid;
        pc.name = p.name;
        pc.cls = cls;
        if (prot) pc.protected_by = *prot;
        a.classified.push_back(std::move(pc));
        if (prot) continue;
        if (cls != WorkloadClass::Background) continue;  // only verified background work is a target
        if (!h.present || h.samples < cfg_.min_history || h.min_cpu < core_floor) continue;
        cands.push_back({&p, cls, h});
    }
    a.foreground_present = fg_pid != 0;

    if (!a.contention) {
        if (cpu_hot && !queue_hot)
            a.reason = "observe: high CPU without runnable-thread pressure; no action";
        else if (cpu_hot && queue_hot)
            a.reason = "observe: contention not yet sustained (" + std::to_string(contention_ticks_) + "/" +
                       std::to_string(cfg_.sustain_ticks) + ")";
        else
            a.reason = "observe: no contention";
        return a;
    }
    if (!a.foreground_present) {
        a.reason = "contention but no foreground workload to protect; no action";
        return a;
    }
    if (in.fg_latency_ms) {
        a.foreground_impacted = *in.fg_latency_ms >= cfg_.fg_latency_degraded_ms;
        if (!a.foreground_impacted) {
            a.reason = "contention but foreground responsive (" + fmt(*in.fg_latency_ms) + " ms); no action";
            return a;
        }
    } else {
        a.foreground_impacted = a.queue_per_cpu >= cfg_.severe_queue_per_cpu;
        if (!a.foreground_impacted) {
            a.reason = "contention; foreground responsiveness unknown and queue not severe; no action";
            return a;
        }
    }
    if (cands.empty()) {
        a.reason = "contention affects foreground but no verified background workload to act on; no action";
        return a;
    }

    std::sort(cands.begin(), cands.end(), [](const Cand& x, const Cand& y) { return x.h.avg_cpu > y.h.avg_cpu; });
    if (cands.size() > cfg_.max_candidates) cands.resize(cfg_.max_candidates);

    const std::uint32_t eff = topo.efficient_cpu_count();
    for (const auto& c : cands) {
        const ProcessSample& p = *c.p;
        policy::Recommendation r;
        r.area = policy::Area::Cpu;
        r.target.pid = p.pid;
        r.target.name = p.name;
        r.target.process_start_ms = p.start_ms;
        r.workload = c.cls;
        r.beneficiary_pid = fg_pid;
        if (!contains(in.opensweep_ecoqos_pids, p.pid)) {
            r.kind = PolicyKind::EcoQoS;
            r.value = 1;
            r.risk = 1;
            r.expected_effect = "background work runs at efficient clocks, freeing performance cores for the foreground";
        } else if (in.allow_cpu_sets && eff > 0 && !contains(in.opensweep_cpusets_pids, p.pid)) {
            r.kind = PolicyKind::CpuSets;
            r.value = eff;
            r.risk = 2;
            r.expected_effect = "background threads prefer the " + std::to_string(eff) + " efficiency-class CPU sets";
        } else if (p.priority_class == policy::kPriorityNormal) {
            r.kind = PolicyKind::PriorityClass;
            r.value = policy::kPriorityBelowNormal;
            r.risk = 2;
            r.expected_effect = "foreground threads win ties against this background process";
        } else {
            continue;  // escalation exhausted; observe only
        }
        r.id = std::string("cpu:") + telemetry::to_string(r.kind) + ":" + std::to_string(p.pid);
        r.reason = "sustained CPU contention is affecting the foreground; " + p.name + " is verified " +
                   telemetry::to_string(c.cls) + " work";
        r.evidence.push_back("system cpu " + fmt(s.cpu_percent) + "% >= " + fmt(cfg_.contention_cpu_percent) + "% for " +
                             std::to_string(contention_ticks_) + " samples");
        r.evidence.push_back("processor queue " + fmt(a.queue_per_cpu, 2) + " per logical cpu >= " +
                             fmt(cfg_.contention_queue_per_cpu, 2));
        if (in.fg_latency_ms)
            r.evidence.push_back("foreground responsiveness " + fmt(*in.fg_latency_ms) + " ms >= " +
                                 fmt(cfg_.fg_latency_degraded_ms) + " ms");
        r.evidence.push_back(p.name + " avg cpu " + fmt(c.h.avg_cpu) + "% (min " + fmt(c.h.min_cpu) + "%) over " +
                             std::to_string(c.h.samples) + " samples");
        a.recommendations.push_back(std::move(r));
    }
    a.reason = a.recommendations.empty() ? "contention, but every candidate already has all allowed policies"
                                         : "contention affecting foreground; recommending minimal background policy";
    return a;
}

}  // namespace opensweep::sched
