// WinActuator: the Windows implementation of telemetry::Actuator (documented APIs only).
//
//   EcoQoS          SetProcessInformation(ProcessPowerThrottling, EXECUTION_SPEED);
//                   original read with GetProcessInformation(ProcessPowerThrottling)
//                   value: 1 = on, 0 = explicitly off, -1 = system default (ControlMask 0)
//   MemoryPriority  Get/SetProcessInformation(ProcessMemoryPriority), 1..5
//   PriorityClass   GetPriorityClass / SetPriorityClass
//   CpuSets         GetProcessDefaultCpuSets / SetProcessDefaultCpuSets (+ GetSystemCpuSetInformation),
//                   resolved from kernel32 at run time (not declared by every SDK/MinGW header).
//                   value = number of efficiency-class CPU sets to confine the process to (hybrid
//                   CPUs only); `original` = number of default CPU sets before (0 = none); the
//                   previous id list is kept per (pid, start time) inside the actuator for restore.
//
// Access rights: PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SET_INFORMATION (EcoQoS, memory
// priority, priority class) or | PROCESS_SET_LIMITED_INFORMATION (CPU sets). If a query needs
// more, PROCESS_QUERY_INFORMATION is requested for that read only.
//
// Defence in depth (the engine/control layer checks these too): apply() refuses pid 0/4, our own
// pid, an unknown start time (process_start_ms == 0), a start-time mismatch (PID reuse), names on
// the built-in protection / anti-cheat / security lists (both the caller-supplied name and the
// real image name), a name mismatch, protected processes (any protection level), critical
// processes (IsProcessCritical) and session-0 processes (unless Options::allow_session0, which
// exists for tests on CI runners whose processes live in session 0).
// apply() only moves in the "less aggressive" direction allowed by docs/core/phases-2-8.md:
//   EcoQoS value 1 only; MemoryPriority 2..4 and lower than the current value; PriorityClass
//   BELOW_NORMAL only and only from NORMAL; CpuSets only on hybrid CPUs and only when the process
//   has no default CPU sets of its own.
// restore() checks pid / start time and value ranges but not the name lists, so rollback is
// never blocked. Every method is safe on an exited process (returns ok=false / false).
#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "telemetry.hpp"

namespace opensweep::win {

class WinActuator final : public telemetry::Actuator {
public:
    struct Options {
        bool allow_session0 = false;  // tests only
    };

    WinActuator();
    explicit WinActuator(Options options);

    telemetry::ApplyResult apply(telemetry::PolicyKind kind, const telemetry::PolicyTarget& target,
                                 std::int64_t value) override;
    telemetry::ApplyResult restore(telemetry::PolicyKind kind, const telemetry::PolicyTarget& target,
                                   std::int64_t original) override;
    bool alive(const telemetry::PolicyTarget& target) override;

    // Reads the current value (verification windows, tests). Applies the pid / start-time guard.
    bool read(telemetry::PolicyKind kind, const telemetry::PolicyTarget& target, std::int64_t& value,
              std::string* error = nullptr);

    // CPU set ids of the least-performant efficiency class; empty when the CPU is not hybrid
    // (a single efficiency class) or the API is unavailable.
    static std::vector<std::uint32_t> efficiency_cpu_set_ids();

private:
    Options opt_;
    std::uint32_t self_pid_ = 0;
    std::mutex mu_;
    std::map<std::pair<std::uint32_t, std::int64_t>, std::vector<std::uint32_t>> saved_cpu_sets_;
};

}  // namespace opensweep::win
