#include "heal_executor.hpp"

#include <cstddef>
#include <string_view>
#include <vector>

#include "log.hpp"
#include "storage.hpp"

namespace opensweep::win {

namespace {

struct ScHandle {
    SC_HANDLE h = nullptr;
    explicit ScHandle(SC_HANDLE x) : h(x) {}
    ~ScHandle() {
        if (h) CloseServiceHandle(h);
    }
    ScHandle(const ScHandle&) = delete;
    ScHandle& operator=(const ScHandle&) = delete;
    explicit operator bool() const { return h != nullptr; }
};

std::wstring system32() {
    wchar_t buf[MAX_PATH];
    const UINT n = GetSystemDirectoryW(buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return std::wstring(buf, n);
}

// Starts %SystemRoot%\System32\<exe> with fixed arguments. Returns the process handle or nullptr.
HANDLE launch(const wchar_t* exe, const wchar_t* args, std::string& error) {
    const std::wstring dir = system32();
    if (dir.empty()) {
        error = "system directory not found";
        return nullptr;
    }
    const std::wstring path = dir + L"\\" + exe;
    std::wstring cmd = L"\"" + path + L"\" " + args;  // fixed text only
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(path.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, dir.c_str(), &si,
                        &pi)) {
        error = "CreateProcess failed: " + log::win32_error(GetLastError());
        return nullptr;
    }
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

bool wait_state(SC_HANDLE svc, DWORD wanted, DWORD timeout_ms, DWORD& last) {
    const ULONGLONG until = GetTickCount64() + timeout_ms;
    for (;;) {
        SERVICE_STATUS_PROCESS sp{};
        DWORD needed = 0;
        if (!QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&sp), sizeof(sp), &needed))
            return false;
        last = sp.dwCurrentState;
        if (sp.dwCurrentState == wanted) return true;
        if (GetTickCount64() >= until) return false;
        Sleep(200);
    }
}

// Event-log service names: printable, no path separators or quotes, bounded.
bool plausible_service_name(std::string_view s) {
    if (s.empty() || s.size() > 256) return false;
    for (char c : s) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7F || c == '\\' || c == '/' || c == '"') return false;
    }
    return true;
}

// Resolves a service key name from a key or display name (SCM events report the display name).
bool resolve_key_name(SC_HANDLE scm, const std::wstring& name, std::wstring& key) {
    ScHandle probe(OpenServiceW(scm, name.c_str(), SERVICE_QUERY_STATUS));
    if (probe) {
        key = name;
        return true;
    }
    wchar_t buf[257];
    DWORD len = 257;
    if (!GetServiceKeyNameW(scm, name.c_str(), buf, &len)) return false;
    key.assign(buf, len);
    return true;
}

}  // namespace

WinHealExecutor::WinHealExecutor(CollectorSlots& slots) : slots_(slots) {}

WinHealExecutor::~WinHealExecutor() {
    std::lock_guard<std::mutex> lock(mu_);
    if (long_job_) CloseHandle(long_job_);  // the child keeps running; we only drop our handle
}

heal::ExecResult WinHealExecutor::restart_service(const std::string& name) {
    if (!plausible_service_name(name)) return {false, "invalid service name"};
    ScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm) return {false, "OpenSCManager failed: " + log::win32_error(GetLastError())};
    std::wstring key;
    if (!resolve_key_name(scm.h, widen(name), key)) return {false, "service not found"};
    if (heal::is_protected_target(heal::Action::RestartService, log::narrow(key)))
        return {false, "protected system or security service"};
    ScHandle svc(OpenServiceW(scm.h, key.c_str(), SERVICE_STOP | SERVICE_START | SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG));
    if (!svc) return {false, "OpenService failed: " + log::win32_error(GetLastError())};

    DWORD needed = 0;
    QueryServiceConfigW(svc.h, nullptr, 0, &needed);
    if (needed == 0 || needed > 64 * 1024) return {false, "service configuration unavailable"};
    std::vector<std::max_align_t> buf(needed / sizeof(std::max_align_t) + 1);
    auto* cfg = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buf.data());
    if (!QueryServiceConfigW(svc.h, cfg, needed, &needed)) return {false, "QueryServiceConfig failed"};
    if (!(cfg->dwServiceType & (SERVICE_WIN32_OWN_PROCESS | SERVICE_WIN32_SHARE_PROCESS)))
        return {false, "not a Win32 service (drivers are never touched)"};
    if (cfg->dwStartType == SERVICE_DISABLED) return {false, "service is disabled"};

    SERVICE_STATUS st{};
    DWORD last = 0;
    if (!ControlService(svc.h, SERVICE_CONTROL_STOP, &st)) {
        const DWORD err = GetLastError();
        if (err != ERROR_SERVICE_NOT_ACTIVE) return {false, "stop failed: " + log::win32_error(err)};
    }
    if (!wait_state(svc.h, SERVICE_STOPPED, 8000, last)) return {false, "service did not stop within 8 s"};
    if (!StartServiceW(svc.h, 0, nullptr)) return {false, "start failed: " + log::win32_error(GetLastError())};
    if (!wait_state(svc.h, SERVICE_RUNNING, 5000, last) && last != SERVICE_START_PENDING)
        return {false, "service did not reach RUNNING"};
    return {true, "service restarted via the SCM"};
}

bool WinHealExecutor::service_running(const std::string& name) {
    ScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm) return false;
    std::wstring key;
    if (!resolve_key_name(scm.h, widen(name), key)) return false;
    ScHandle svc(OpenServiceW(scm.h, key.c_str(), SERVICE_QUERY_STATUS));
    DWORD last = 0;
    return svc && wait_state(svc.h, SERVICE_RUNNING, 3000, last);
}

heal::ExecResult WinHealExecutor::start_long_job(const wchar_t* exe, const wchar_t* args) {
    std::lock_guard<std::mutex> lock(mu_);
    if (long_job_) {
        if (WaitForSingleObject(long_job_, 0) == WAIT_TIMEOUT) return {false, "another repair job is still running"};
        CloseHandle(long_job_);
        long_job_ = nullptr;
    }
    std::string err;
    long_job_ = launch(exe, args, err);
    if (!long_job_) return {false, err};
    return {true, "started (runs in the background; results are written to the Windows CBS/DISM logs)"};
}

bool WinHealExecutor::long_job_ok() {
    std::lock_guard<std::mutex> lock(mu_);
    if (!long_job_) return false;
    if (WaitForSingleObject(long_job_, 0) == WAIT_TIMEOUT) return true;  // still running
    DWORD code = 1;
    return GetExitCodeProcess(long_job_, &code) && code == 0;
}

heal::ExecResult WinHealExecutor::execute(heal::Action a, const std::string& target) {
    switch (a) {
        case heal::Action::RestartCollector: {
            std::lock_guard<std::mutex> lock(slots_.mu);
            slots_.sys = std::make_unique<SystemCollector>();
            return {true, "system collector recreated"};
        }
        case heal::Action::ReconnectEventSubscription: {
            std::lock_guard<std::mutex> lock(slots_.mu);
            if (!slots_.events) slots_.events = std::make_unique<EventCollector>();
            return {true, "event subscription retries with back-off (bookmark kept)"};
        }
        case heal::Action::RestartService: return restart_service(target);
        case heal::Action::ResetNetworkStack: {
            std::string err1, err2;
            HANDLE p1 = launch(L"ipconfig.exe", L"/flushdns", err1);
            HANDLE p2 = launch(L"netsh.exe", L"winsock reset", err2);
            DWORD c1 = 1, c2 = 1;
            if (p1) {
                WaitForSingleObject(p1, 5000);
                GetExitCodeProcess(p1, &c1);
                CloseHandle(p1);
            }
            if (p2) {
                WaitForSingleObject(p2, 5000);
                GetExitCodeProcess(p2, &c2);
                CloseHandle(p2);
            }
            const bool ok = p1 && p2 && c1 == 0 && c2 == 0;
            return {ok, std::string("ipconfig /flushdns: ") + (p1 ? "exit " + std::to_string(c1) : err1) +
                            "; netsh winsock reset: " + (p2 ? "exit " + std::to_string(c2) : err2) +
                            (ok ? "; a reboot is required for the winsock reset" : "")};
        }
        case heal::Action::RunSfc: return start_long_job(L"sfc.exe", L"/scannow");
        case heal::Action::RunDismRestoreHealth:
            return start_long_job(L"Dism.exe", L"/Online /Cleanup-Image /RestoreHealth");
        case heal::Action::RestartExplorer:
        case heal::Action::TerminateApplication:
            return {false, "must be run from the OpenSweep app in the user's session"};
        case heal::Action::RestartWorker: return {false, "no separate worker to restart in this build"};
        case heal::Action::ClearInternalCache:
        case heal::Action::RestoreOwnedPolicies: return {false, "handled by the engine"};
    }
    return {false, "unknown action"};
}

bool WinHealExecutor::verify(heal::Action a, const std::string& target) {
    switch (a) {
        case heal::Action::RestartCollector: {
            std::lock_guard<std::mutex> lock(slots_.mu);
            telemetry::Sample s;
            return slots_.sys && slots_.sys->collect(s) && s.mem_total > 0;
        }
        case heal::Action::ReconnectEventSubscription: {
            std::lock_guard<std::mutex> lock(slots_.mu);
            return slots_.events && slots_.events->subscribed();
        }
        case heal::Action::RestartService: return service_running(target);
        case heal::Action::ResetNetworkStack: return true;  // exit codes were checked in execute()
        case heal::Action::RunSfc:
        case heal::Action::RunDismRestoreHealth: return long_job_ok();
        default: return false;
    }
}

}  // namespace opensweep::win
