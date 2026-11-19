// Shared fakes for the engine / commands2 tests: actuator, heal executor, scenario samples.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "engine.hpp"
#include "observe_test_util.hpp"
#include "policy_types.hpp"

namespace eng_test {

using namespace opensweep;

struct FakeActuator : telemetry::Actuator {
    int applies = 0;
    int restores = 0;
    std::map<std::uint32_t, std::int64_t> start;  // live processes: pid -> start ms
    std::vector<std::pair<telemetry::PolicyKind, std::uint32_t>> applied;

    telemetry::ApplyResult apply(telemetry::PolicyKind k, const telemetry::PolicyTarget& t, std::int64_t) override {
        ++applies;
        applied.emplace_back(k, t.pid);
        telemetry::ApplyResult r;
        r.ok = true;
        r.original = 0;
        return r;
    }
    telemetry::ApplyResult restore(telemetry::PolicyKind, const telemetry::PolicyTarget&, std::int64_t) override {
        ++restores;
        telemetry::ApplyResult r;
        r.ok = true;
        return r;
    }
    bool alive(const telemetry::PolicyTarget& t) override {
        auto it = start.find(t.pid);
        return it != start.end() && (t.process_start_ms == 0 || it->second == t.process_start_ms);
    }
};

struct FakeExecutor : heal::Executor {
    std::vector<std::pair<heal::Action, std::string>> calls;
    bool ok = true;
    heal::ExecResult execute(heal::Action a, const std::string& target) override {
        calls.emplace_back(a, target);
        return {ok, ok ? "done" : "failed"};
    }
    bool verify(heal::Action, const std::string&) override { return ok; }
};

inline constexpr std::int64_t kT0 = 1'780'000'000'000;  // 2026-05-28, a whole minute
inline constexpr std::int64_t kTick = 2000;

inline sched::Topology topo8() {
    sched::Topology t;
    t.logical_count = 8;
    t.efficiency_class.assign(8, 0);
    return t;
}

inline telemetry::ProcessSample proc(std::uint32_t pid, const char* name, double cpu, double priv_mb,
                                     std::uint32_t session = 1) {
    telemetry::ProcessSample p = obs_test::proc(pid, name, cpu, priv_mb);
    p.session_id = session;
    p.priority_class = policy::kPriorityNormal;
    p.start_ms = 1000 + pid;
    return p;
}

// A healthy machine (memory, commit and disk comfortably normal).
inline telemetry::Sample quiet(std::int64_t ts) {
    telemetry::Sample s;
    s.ts_ms = ts;
    s.cpu_percent = 12.0;
    s.cpu_queue_length = 1.0;
    s.mem_total = 16 * obs_test::kGBu;
    s.mem_avail = 9 * obs_test::kGBu;
    s.mem_load_percent = 45;
    s.commit_total = 10 * obs_test::kGBu;
    s.commit_limit = 24 * obs_test::kGBu;
    s.kernel_paged = 500 * obs_test::kMBu;
    s.kernel_nonpaged = 300 * obs_test::kMBu;
    s.hard_faults_per_sec = 2.0;
    s.disk_read_latency_ms = 1.0;
    s.disk_write_latency_ms = 1.0;
    s.processes = 200;
    s.threads = 3000;
    s.handles = 100000;
    auto fg = proc(100, "word.exe", 5.0, 300);
    fg.foreground = true;
    s.foreground_pid = 100;
    s.top.push_back(fg);
    s.top.push_back(proc(200, "SearchProtocolHost.exe", 1.0, 50));
    return s;
}

// Sustained, attributed CPU contention: a verified background indexer eats CPU while the
// foreground app waits (queue 2.5 per logical CPU, so it is "severe" without a latency signal).
inline telemetry::Sample contended(std::int64_t ts, double queue = 20.0) {
    telemetry::Sample s = quiet(ts);
    s.cpu_percent = 96.0;
    s.cpu_queue_length = queue;
    s.top[0].cpu_percent = 20.0;
    s.top[1].cpu_percent = 40.0;
    return s;
}

struct Rig {
    obs_test::MemFileIO io;
    FakeActuator act;
    FakeExecutor exec;
    std::int64_t now = kT0;
    std::unique_ptr<engine::Engine> eng;
    std::vector<std::string> logs;

    explicit Rig(bool start = true) {
        act.start[100] = 1100;
        act.start[200] = 1200;
        make(start);
    }
    engine::EngineOptions options() {
        engine::EngineOptions o;
        o.data_dir = "/data";
        o.topology = topo8();
        o.clock = [this] { return now; };
        o.log = [this](std::string_view m) {
            if (logs.size() < 1000) logs.emplace_back(m);
        };
        return o;
    }
    void make(bool start = true) {
        eng.reset();
        eng = std::make_unique<engine::Engine>(act, exec, io, options());
        if (start) eng->start();
    }
    void tick(const telemetry::Sample& s, std::span<const telemetry::EventRecord> ev = {}) {
        now = s.ts_ms;
        eng->tick(s, ev);
    }
    // Runs `n` ticks of `gen(ts)` starting at the current time + 2 s.
    template <typename G>
    void run(int n, G&& gen) {
        for (int i = 0; i < n; ++i) tick(gen(now + kTick));
    }
};

}  // namespace eng_test
