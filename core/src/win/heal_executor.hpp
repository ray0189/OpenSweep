// WinHealExecutor: the Windows side of SweepHeal (heal::Executor). Windows only.
//
// OpenSweep-owned, automatic:
//   restart_collector              recreate the SystemCollector (PDH query + process tracking)
//   reconnect_event_subscription   the EventCollector already retries with back-off (5 s .. 5 min) and
//                                  keeps its bookmark; recreating it would replay the 24 h backfill, so
//                                  execute() only reports and verify() checks subscribed()
//   clear_internal_cache, restore_owned_policies   executed by the portable engine, never reach here
// Approval-required (admin approve{id}; targets never come from IPC clients):
//   restart_service        SCM API only (ControlService STOP, StartService) on the service named by the
//                          event log record that opened the incident; drivers, disabled services and
//                          the protected list (heal::is_protected_target, re-checked on the key name)
//                          are refused. Waits at most 8 s for STOPPED and 5 s for RUNNING.
//   reset_network_stack    %SystemRoot%\System32\ipconfig.exe /flushdns and
//                          %SystemRoot%\System32\netsh.exe winsock reset (fixed arguments, waits <= 10 s;
//                          a reboot is needed for the winsock reset to take effect)
//   run_sfc                %SystemRoot%\System32\sfc.exe /scannow                (started, not awaited)
//   run_dism_restorehealth %SystemRoot%\System32\Dism.exe /Online /Cleanup-Image /RestoreHealth (started)
//                          Only one of these long jobs runs at a time. verify() reports "started / still
//                          running / exit code 0"; the actual repair result is in CBS.log / dism.log.
//   restart_explorer, terminate_application   refused: a session-0 service cannot do this safely in
//                          the user's session; the OpenSweep app must offer them.
// No command line ever contains a string that came from a client or from event data.
#pragma once

#include <windows.h>

#include <memory>
#include <mutex>
#include <string>

#include "collectors.hpp"
#include "events.hpp"
#include "heal.hpp"

namespace opensweep::win {

// Collectors shared by the engine loop (collect), the IPC thread (foreground hints) and the heal
// executor (restart). `mu` guards the pointers and every call on the objects.
struct CollectorSlots {
    std::mutex mu;
    std::unique_ptr<SystemCollector> sys;
    std::unique_ptr<EventCollector> events;
};

class WinHealExecutor final : public heal::Executor {
public:
    explicit WinHealExecutor(CollectorSlots& slots);
    ~WinHealExecutor() override;
    WinHealExecutor(const WinHealExecutor&) = delete;
    WinHealExecutor& operator=(const WinHealExecutor&) = delete;

    heal::ExecResult execute(heal::Action a, const std::string& target) override;
    bool verify(heal::Action a, const std::string& target) override;

private:
    heal::ExecResult restart_service(const std::string& name);
    bool service_running(const std::string& name);
    heal::ExecResult start_long_job(const wchar_t* exe, const wchar_t* args);
    bool long_job_ok();

    CollectorSlots& slots_;
    std::mutex mu_;
    HANDLE long_job_ = nullptr;  // sfc / DISM process handle
};

}  // namespace opensweep::win
