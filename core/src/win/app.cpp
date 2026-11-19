#include "app.hpp"

#include <chrono>
#include <string>

#include "commands.hpp"
#include "log.hpp"
#include "metrics.hpp"
#include "pipe_server.hpp"
#include "protocol.hpp"
#include "topology.hpp"

namespace opensweep::win {

int run_core(HANDLE stop_event, const std::function<void()>& on_ready) {
    commands::ServiceInfo info;
    info.started_at_ms = commands::now_unix_ms();
    info.started_steady = std::chrono::steady_clock::now();

    log::info(std::string("OpenSweepCore ") + commands::kVersion + " starting (mode=" + commands::kMode +
              ", protocol=1, pid=" + std::to_string(GetCurrentProcessId()) + ")");

    commands::TopologyInfo topo;
    if (!detect_topology(topo)) log::warn("topology detection failed; the topology command will report internal");

    CpuSampler cpu;
    cpu.start();

    WinSystemProvider provider(cpu, topo);
    commands::ServerCounters counters;
    protocol::Dispatcher dispatcher;
    commands::register_phase1(dispatcher, provider, info, counters);

    int exit_code = 0;
    {
        PipeServer server(dispatcher, counters);
        if (!server.open()) {
            log::error("failed to open the IPC pipe; exiting");
            exit_code = 2;
        } else {
            if (on_ready) on_ready();
            log::info("ready");
            if (!server.serve(stop_event)) exit_code = 3;
        }
    }
    cpu.stop();
    log::info("stopped (exit code " + std::to_string(exit_code) + ")");
    return exit_code;
}

}  // namespace opensweep::win
