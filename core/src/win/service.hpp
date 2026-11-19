// Windows service integration (SCM) for OpenSweep Core.
#pragma once

namespace opensweep::win {

inline constexpr const wchar_t* kServiceName = L"OpenSweepCore";
inline constexpr const wchar_t* kServiceDisplayName = L"OpenSweep Core";
inline constexpr const wchar_t* kServiceDescription =
    L"OpenSweep Core: local stability and resource monitoring over an ACL-restricted named pipe. Runs in "
    L"Monitor mode by default (observes only); reversible process policies are applied only after an "
    L"administrator opts in, and are rolled back when the service stops.";

// Connects to the SCM via StartServiceCtrlDispatcherW and runs until the service stops.
// Returns a process exit code. If the process was not started by the SCM, prints a hint.
int run_service();

// Creates the service (auto start, LocalSystem, restart on failure x2, unrestricted service SID,
// reduced privilege set). Returns 0 on success.
int install_service();

// Stops (waiting up to ~15 s) and deletes the service. Returns 0 on success.
int uninstall_service();

}  // namespace opensweep::win
