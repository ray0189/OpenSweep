#include "app.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "actuator.hpp"
#include "collectors.hpp"
#include "commands.hpp"
#include "commands2.hpp"
#include "engine.hpp"
#include "etw.hpp"
#include "events.hpp"
#include "heal_executor.hpp"
#include "log.hpp"
#include "metrics.hpp"
#include "pipe_server.hpp"
#include "process_info.hpp"
#include "protocol.hpp"
#include "storage.hpp"
#include "topology.hpp"

namespace opensweep::win {

namespace {

constexpr int kCollectFailuresBeforeHeal = 3;    // 6 s without a usable sample
constexpr int kEventFailuresBeforeHeal = 30;     // 1 min with the event log unavailable

sched::Topology engine_topology(const commands::TopologyInfo& t, const std::vector<commands2::CpuSetInfo>& sets) {
    sched::Topology out;
    out.logical_count = t.logical_processors > 0 ? t.logical_processors : 1;
    out.hybrid = t.hybrid();
    for (const auto& c : sets) out.efficiency_class.push_back(static_cast<std::uint8_t>(c.efficiency_class));
    return out;
}

// reportForeground: the pid must exist and be in the caller's *token* session. Session 0 has no
// foreground window: a session-0 caller's own session-0 pid is accepted but not recorded.
commands2::ForegroundVerdict check_foreground(CollectorSlots& slots, std::uint32_t pid, std::uint32_t caller_session) {
    procinfo::ProcessHandle h = procinfo::open_process(pid, PROCESS_QUERY_LIMITED_INFORMATION);
    if (!h || !procinfo::is_running(h.get())) return commands2::ForegroundVerdict::NoSuchProcess;
    std::uint32_t session = 0;
    if (!procinfo::session_of(pid, session)) return commands2::ForegroundVerdict::NoSuchProcess;
    if (session != caller_session) return commands2::ForegroundVerdict::WrongSession;
    if (session == 0) return commands2::ForegroundVerdict::Ignored;
    std::lock_guard<std::mutex> lock(slots.mu);
    if (!slots.sys) return commands2::ForegroundVerdict::Unavailable;
    return slots.sys->report_foreground(pid, caller_session) ? commands2::ForegroundVerdict::Accepted
                                                              : commands2::ForegroundVerdict::NoSuchProcess;
}

}  // namespace

int run_core(HANDLE stop_event, const std::function<void()>& on_ready) {
    commands::ServiceInfo info;
    info.started_at_ms = commands::now_unix_ms();
    info.started_steady = std::chrono::steady_clock::now();

    log::info(std::string("OpenSweepCore ") + commands::kVersion + " starting (protocol=1, pid=" +
              std::to_string(GetCurrentProcessId()) + ")");

    commands::TopologyInfo topo;
    if (!detect_topology(topo)) log::warn("topology detection failed; the topology command will report internal");
    std::vector<commands2::CpuSetInfo> cpu_sets;
    detect_cpu_sets(cpu_sets);

    // ---- storage --------------------------------------------------------------------------------
    std::string why;
    const std::wstring data_dir = prepare_data_dir(why);
    WinFileIO file_io;
    NullFileIO null_io;
    trace::FileIO& io = data_dir.empty() ? static_cast<trace::FileIO&>(null_io) : static_cast<trace::FileIO&>(file_io);
    if (data_dir.empty())
        log::warn("persistence disabled: " + why + " (the engine runs in memory; policies cannot be journaled, so "
                  "nothing is ever applied)");
    else
        log::info("data directory: " + log::narrow(data_dir));

    // ---- engine ---------------------------------------------------------------------------------
    CollectorSlots slots;
    slots.sys = std::make_unique<SystemCollector>();
    slots.events = std::make_unique<EventCollector>();
    WinActuator actuator;
    WinHealExecutor executor(slots);

    engine::EngineOptions opts;
    opts.data_dir = log::narrow(data_dir);
    opts.topology = engine_topology(topo, cpu_sets);
    opts.log = [](std::string_view m) { log::info(std::string("engine: ") + std::string(m)); };
    engine::Engine engine(actuator, executor, io, std::move(opts));
    engine.start();  // loads state and rolls back anything a crash left applied

    // ---- kernel tracing (docs/core/etw.md) --------------------------------------------------------
    KernelSession::Options etw_opts;
    etw_opts.logical_processors = topo.logical_processors > 0 ? topo.logical_processors : 1;
    KernelSession etw(etw_opts);
    if (engine.etw_enabled()) {
        if (!etw.start()) log::warn("kernel tracing unavailable: " + etw.status().reason);
    } else {
        etw.set_enabled(false);
        log::info("kernel tracing disabled by config.json (etw:false)");
    }

    // ---- IPC ------------------------------------------------------------------------------------
    CpuSampler cpu;  // Phase 1 `metrics` (GetSystemTimes; no PDH, so no duplicate counter query)
    cpu.start();
    WinSystemProvider provider(cpu, topo);
    commands::ServerCounters counters;
    protocol::Dispatcher dispatcher;
    commands::register_phase1(dispatcher, provider, info, counters, commands2::engine_mode_provider(engine));
    commands2::Platform platform;
    platform.provider = &provider;
    platform.cpu_sets = cpu_sets;
    platform.service = &info;
    platform.report_foreground = [&slots](std::uint32_t pid, std::uint32_t session) {
        return check_foreground(slots, pid, session);
    };
    platform.etw_status = [&etw] { return etw.status(); };
    platform.etw_latency = [&etw] { return etw.latency(); };
    platform.etw_disk = [&etw] { return etw.disk_activity(); };
    platform.etw_faults = [&etw] { return etw.hard_faults(); };
    platform.set_etw = [&etw](bool on) { return etw.set_enabled(on); };
    platform.diagnostics_extra = [&slots, &file_io, &data_dir, &etw]() {
        json::Value o = json::Value::object();
        o.set("etw", etw.diagnostics());
        std::lock_guard<std::mutex> lock(slots.mu);
        if (slots.sys) {
            o.set("collectLastUs", slots.sys->last_collect_us());
            o.set("collectMaxUs", slots.sys->max_collect_us());
            o.set("pdhCountersOk", slots.sys->counters_ok());
            o.set("pdhCountersMissing", slots.sys->counters_missing());
        }
        if (slots.events) {
            o.set("eventsSubscribed", slots.events->subscribed());
            o.set("eventsBackfillDone", slots.events->backfill_done());
            o.set("eventsDropped", slots.events->records_dropped());
        }
        o.set("persistence", !data_dir.empty());
        o.set("fileErrors", file_io.errors());
        return o;
    };
    commands2::register_engine_commands(dispatcher, engine, platform);

    // ---- engine loop (collect outside the engine lock) ---------------------------------------------
    std::atomic<int> collect_failures{0};
    std::atomic<int> event_failures{0};
    auto collect = [&](telemetry::Sample& s, std::vector<telemetry::EventRecord>& ev) {
        std::lock_guard<std::mutex> lock(slots.mu);
        bool ok = slots.sys && slots.sys->collect(s);
        collect_failures = ok ? 0 : collect_failures + 1;
        if (ok) etw.fill_sample(s);  // kernel fields + per-process hard faults (interval since last tick)
        if (slots.events) {
            if (slots.events->poll(ev))
                event_failures = 0;
            else
                ++event_failures;
        }
        return ok;
    };
    auto after_tick = [&]() {
        if (collect_failures >= kCollectFailuresBeforeHeal) {
            collect_failures = 0;
            heal::RecoveryRequest r;
            r.action = heal::Action::RestartCollector;
            r.target = "system_collector";
            r.trigger = "collect() failed " + std::to_string(kCollectFailuresBeforeHeal) + " times in a row";
            const auto res = engine.request_heal(r);
            log::warn(std::string("self-heal restart_collector: ") + heal::to_string(res.status));
        }
        if (event_failures >= kEventFailuresBeforeHeal) {
            event_failures = 0;
            heal::RecoveryRequest r;
            r.action = heal::Action::ReconnectEventSubscription;
            r.target = "event_log";
            r.trigger = "event log unavailable for 1 min";
            const auto res = engine.request_heal(r);
            log::warn(std::string("self-heal reconnect_event_subscription: ") + heal::to_string(res.status));
        }
    };

    int exit_code = 0;
    {
        PipeServer server(dispatcher, counters);
        if (!server.open()) {
            log::error("failed to open the IPC pipe; exiting");
            exit_code = 2;
        } else {
            // Engine thread starts only once the pipe is open; it is stopped before the engine shuts down.
            engine::Runner runner(engine, collect, std::chrono::milliseconds(telemetry::kSampleIntervalMs), after_tick);
            runner.start();
            if (on_ready) on_ready();
            log::info(std::string("ready (mode=") + engine.mode_name() + ")");
            if (!server.serve(stop_event)) exit_code = 3;
            runner.stop();
        }
    }
    etw.stop();  // ControlTrace(STOP) + CloseTrace + consumer join (bounded), before the engine shuts down
    const auto rb = engine.shutdown();  // rolls back every OpenSweep policy, persists state
    cpu.stop();
    log::info("stopped (exit code " + std::to_string(exit_code) + ", policies restored " + std::to_string(rb.restored) +
              ", failed " + std::to_string(rb.failed) + ")");
    return exit_code;
}

}  // namespace opensweep::win
