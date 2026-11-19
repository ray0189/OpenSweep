// Wires the Phase 1 core together (logging is initialised by the caller):
// topology detection, CPU sampler, command dispatcher and the named-pipe server.
#pragma once

#include <windows.h>

#include <functional>

namespace opensweep::win {

// Runs the core until `stop_event` is signalled. `on_ready` is invoked once the pipe is
// listening (the service uses it to report SERVICE_RUNNING). Returns a process exit code
// (0 on clean stop).
int run_core(HANDLE stop_event, const std::function<void()>& on_ready);

}  // namespace opensweep::win
