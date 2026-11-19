#include "service.hpp"

#include <windows.h>

#include <cstdio>
#include <mutex>
#include <string>

#include "app.hpp"
#include "log.hpp"

namespace opensweep::win {

namespace {

constexpr DWORD kStopWaitHintMs = 5000;  // the core stops well within this (pipe cancel budget: 2 s)
constexpr DWORD kStartWaitHintMs = 5000;

std::mutex g_status_mu;
SERVICE_STATUS_HANDLE g_status_handle = nullptr;
SERVICE_STATUS g_status{};
DWORD g_checkpoint = 1;
HANDLE g_stop_event = nullptr;
int g_exit_code = 0;

void report_status(DWORD state, DWORD win32_exit, DWORD specific_exit, DWORD wait_hint) {
    std::lock_guard<std::mutex> lock(g_status_mu);
    if (!g_status_handle) return;
    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCurrentState = state;
    g_status.dwWin32ExitCode = win32_exit;
    g_status.dwServiceSpecificExitCode = specific_exit;
    g_status.dwWaitHint = wait_hint;
    // Accept stop/shutdown only while running (MS guidance: none while start is pending).
    g_status.dwControlsAccepted = (state == SERVICE_RUNNING) ? (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN) : 0;
    g_status.dwCheckPoint = (state == SERVICE_RUNNING || state == SERVICE_STOPPED) ? 0 : g_checkpoint++;
    if (!SetServiceStatus(g_status_handle, &g_status))
        log::error("SetServiceStatus failed: " + log::win32_error(GetLastError()));
}

DWORD WINAPI control_handler(DWORD control, DWORD /*event_type*/, LPVOID /*event_data*/, LPVOID /*context*/) {
    switch (control) {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            log::info(control == SERVICE_CONTROL_STOP ? "stop requested" : "system shutdown");
            report_status(SERVICE_STOP_PENDING, NO_ERROR, 0, kStopWaitHintMs);
            if (g_stop_event) SetEvent(g_stop_event);
            return NO_ERROR;
        case SERVICE_CONTROL_INTERROGATE:
            return NO_ERROR;
        default:
            return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

VOID WINAPI service_main(DWORD /*argc*/, LPWSTR* /*argv*/) {
    g_status_handle = RegisterServiceCtrlHandlerExW(kServiceName, control_handler, nullptr);
    if (!g_status_handle) {
        log::error("RegisterServiceCtrlHandlerEx failed: " + log::win32_error(GetLastError()));
        g_exit_code = 1;
        return;
    }
    report_status(SERVICE_START_PENDING, NO_ERROR, 0, kStartWaitHintMs);

    // Never closed: the control handler may run concurrently until the process exits.
    g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_stop_event) {
        const DWORD err = GetLastError();
        log::error("CreateEvent failed: " + log::win32_error(err));
        g_exit_code = 1;
        report_status(SERVICE_STOPPED, err, 0, 0);
        return;
    }

    const int code = run_core(g_stop_event, [] { report_status(SERVICE_RUNNING, NO_ERROR, 0, 0); });
    g_exit_code = code;
    if (code == 0) {
        report_status(SERVICE_STOPPED, NO_ERROR, 0, 0);
    } else {
        // A service-specific exit code lets the SCM apply the failure actions (restart).
        report_status(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR, static_cast<DWORD>(code), 0);
    }
}

struct ScHandle {
    SC_HANDLE h = nullptr;
    explicit ScHandle(SC_HANDLE handle) : h(handle) {}
    ~ScHandle() {
        if (h) CloseServiceHandle(h);
    }
    ScHandle(const ScHandle&) = delete;
    ScHandle& operator=(const ScHandle&) = delete;
    explicit operator bool() const { return h != nullptr; }
};

void print_error(const char* what, DWORD err) {
    std::fprintf(stderr, "error: %s failed: %s\n", what, log::win32_error(err).c_str());
    if (err == ERROR_ACCESS_DENIED) std::fprintf(stderr, "hint: run from an elevated (Administrator) prompt\n");
}

bool config2(SC_HANDLE svc, DWORD level, void* info, const char* what) {
    if (ChangeServiceConfig2W(svc, level, info)) return true;
    const DWORD err = GetLastError();
    std::fprintf(stderr, "warning: %s failed: %s\n", what, log::win32_error(err).c_str());
    return false;
}

}  // namespace

int run_service() {
    SERVICE_TABLE_ENTRYW table[] = {
        {const_cast<LPWSTR>(kServiceName), service_main},
        {nullptr, nullptr},
    };
    if (!StartServiceCtrlDispatcherW(table)) {
        const DWORD err = GetLastError();
        if (err == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
            std::fprintf(stderr,
                         "OpenSweepCore was not started by the Service Control Manager.\n"
                         "Use --console to run in the foreground, or --help for options.\n");
        } else {
            log::error("StartServiceCtrlDispatcher failed: " + log::win32_error(err));
        }
        return 1;
    }
    return g_exit_code;
}

int install_service() {
    std::wstring path(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (n == 0 || n >= path.size()) {
        print_error("GetModuleFileName", GetLastError());
        return 1;
    }
    path.resize(n);
    // Always quote the binary path (avoids the unquoted-service-path vulnerability).
    const std::wstring command = L"\"" + path + L"\"";

    ScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE));
    if (!scm) {
        print_error("OpenSCManager", GetLastError());
        return 1;
    }
    // SERVICE_START is required on this handle for SC_ACTION_RESTART failure actions.
    ScHandle svc(CreateServiceW(scm.h, kServiceName, kServiceDisplayName,
                                SERVICE_CHANGE_CONFIG | SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS | SERVICE_START,
                                SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, command.c_str(),
                                nullptr, nullptr, nullptr, nullptr /* LocalSystem */, nullptr));
    if (!svc) {
        const DWORD err = GetLastError();
        if (err == ERROR_SERVICE_EXISTS) {
            std::fprintf(stderr, "error: service OpenSweepCore already exists (run --uninstall first)\n");
        } else {
            print_error("CreateService", err);
        }
        return 1;
    }

    bool all_ok = true;
    SERVICE_DESCRIPTIONW desc{};
    desc.lpDescription = const_cast<LPWSTR>(kServiceDescription);
    if (!config2(svc.h, SERVICE_CONFIG_DESCRIPTION, &desc, "setting the description")) all_ok = false;

    // Restart after 5 s on the first failure, after 30 s on the second, then do nothing.
    // The failure counter resets after one day without failures.
    SC_ACTION actions[3] = {
        {SC_ACTION_RESTART, 5000},
        {SC_ACTION_RESTART, 30000},
        {SC_ACTION_NONE, 0},
    };
    SERVICE_FAILURE_ACTIONSW failure{};
    failure.dwResetPeriod = 24 * 60 * 60;
    failure.lpRebootMsg = nullptr;
    failure.lpCommand = nullptr;
    failure.cActions = 3;
    failure.lpsaActions = actions;
    if (!config2(svc.h, SERVICE_CONFIG_FAILURE_ACTIONS, &failure, "setting failure actions")) all_ok = false;

    // Also apply failure actions when the service stops itself with a non-zero exit code.
    SERVICE_FAILURE_ACTIONS_FLAG failure_flag{};
    failure_flag.fFailureActionsOnNonCrashFailures = TRUE;
    if (!config2(svc.h, SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, &failure_flag, "setting the failure-actions flag")) all_ok = false;

    // Per-service SID (NT SERVICE\OpenSweepCore) so resources can later be ACLed to this service.
    SERVICE_SID_INFO sid_info{};
    sid_info.dwServiceSidType = SERVICE_SID_TYPE_UNRESTRICTED;
    if (!config2(svc.h, SERVICE_CONFIG_SERVICE_SID_INFO, &sid_info, "setting the service SID type")) all_ok = false;

    // Least privilege: strip every LocalSystem privilege the service does not need.
    //   SeChangeNotifyPrivilege  - traverse checking (always required)
    //   SeImpersonatePrivilege   - ImpersonateNamedPipeClient, to read the client's SID for the audit log
    //   SeCreateGlobalPrivilege  - kept for future global named objects; harmless
    static wchar_t privileges[] = L"SeChangeNotifyPrivilege\0SeImpersonatePrivilege\0SeCreateGlobalPrivilege\0";
    SERVICE_REQUIRED_PRIVILEGES_INFOW priv_info{};
    priv_info.pmszRequiredPrivileges = privileges;
    if (!config2(svc.h, SERVICE_CONFIG_REQUIRED_PRIVILEGES_INFO, &priv_info, "setting required privileges")) all_ok = false;

    std::printf("Service OpenSweepCore installed (auto start, LocalSystem with a reduced privilege set).\n"
                "Binary: %s\n"
                "Start it with: sc start OpenSweepCore\n",
                log::narrow(path).c_str());
    if (!all_ok) std::fprintf(stderr, "warning: some optional settings could not be applied (see above)\n");
    log::info("service installed");
    return 0;
}

int uninstall_service() {
    ScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm) {
        print_error("OpenSCManager", GetLastError());
        return 1;
    }
    ScHandle svc(OpenServiceW(scm.h, kServiceName, SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE));
    if (!svc) {
        const DWORD err = GetLastError();
        if (err == ERROR_SERVICE_DOES_NOT_EXIST) {
            std::printf("Service OpenSweepCore is not installed.\n");
            return 0;
        }
        print_error("OpenService", err);
        return 1;
    }

    SERVICE_STATUS status{};
    if (ControlService(svc.h, SERVICE_CONTROL_STOP, &status)) {
        std::printf("Stopping OpenSweepCore...\n");
        const ULONGLONG deadline = GetTickCount64() + 15000;
        for (;;) {
            SERVICE_STATUS_PROCESS sp{};
            DWORD needed = 0;
            if (!QueryServiceStatusEx(svc.h, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&sp), sizeof(sp),
                                      &needed))
                break;
            if (sp.dwCurrentState == SERVICE_STOPPED) break;
            if (GetTickCount64() > deadline) {
                std::fprintf(stderr, "warning: service did not stop within 15 s; deleting anyway\n");
                break;
            }
            Sleep(250);
        }
    } else {
        const DWORD err = GetLastError();
        if (err != ERROR_SERVICE_NOT_ACTIVE && err != ERROR_SERVICE_CANNOT_ACCEPT_CTRL)
            std::fprintf(stderr, "warning: stopping the service failed: %s\n", log::win32_error(err).c_str());
    }

    if (!DeleteService(svc.h)) {
        const DWORD err = GetLastError();
        if (err == ERROR_SERVICE_MARKED_FOR_DELETE) {
            std::printf("Service OpenSweepCore is already marked for deletion.\n");
            return 0;
        }
        print_error("DeleteService", err);
        return 1;
    }
    std::printf("Service OpenSweepCore removed.\n");
    log::info("service uninstalled");
    return 0;
}

}  // namespace opensweep::win
