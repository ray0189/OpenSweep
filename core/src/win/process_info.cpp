#include "process_info.hpp"

#include <algorithm>
#include <iterator>
#include <mutex>

namespace opensweep::win::procinfo {

namespace {

constexpr std::uint64_t kUnixEpochAs100ns = 116444736000000000ULL;  // 1970-01-01 in FILETIME units

// Normalised (lower-case, no ".exe") names. Keep sorted only for readability; lookup is linear
// over a few dozen short strings, which is cheaper than building a hash set per call.
constexpr std::string_view kProtected[] = {
    // Contract list (docs/core/phases-2-8.md, "Safety rules every module enforces").
    "csrss", "wininit", "winlogon", "lsass", "services", "smss", "svchost", "dwm", "audiodg", "msmpeng",
    "explorer", "system", "registry", "memory compression", "fontdrvhost", "sihost", "ctfmon",
    // Same class of component, added as defence in depth.
    "lsaiso", "secure system", "system idle process", "opensweepcore", "wudfhost",
};

constexpr std::string_view kAntiCheat[] = {
    "easyanticheat", "easyanticheat_eos", "eaanticheat.gameservice", "eaanticheat.installer", "beservice",
    "beservice_x64", "vgc", "vgtray", "faceit", "faceitservice", "faceitclient", "gamemon", "gamemon64",
    "pnkbstra", "pnkbstrb", "sguard64", "sguardsvc64", "ace-tray", "xigncode", "nprotect",
};

constexpr std::string_view kSecurity[] = {
    // Microsoft Defender / Windows Security / Defender for Endpoint
    "msmpeng", "nissrv", "mpdefendercoreservice", "securityhealthservice", "securityhealthsystray", "smartscreen",
    "mssense", "senseir", "sensecncproxy", "sensendr",
    // Third-party AV / EDR (well-known service images)
    "csfalconservice", "csfalconcontainer", "sentinelagent", "sentinelservicehost", "sentinelstaticengine",
    "sentinelhelperservice", "cb", "repmgr", "cylancesvc", "avp", "avpui", "ekrn", "egui", "bdagent",
    "bdservicehost", "vsserv", "avastsvc", "avastui", "aswidsagent", "avgsvc", "avgui", "mcshield", "mfemms",
    "mfevtps", "mfefire", "ccsvchost", "nortonsecurity", "sophoshealth", "savservice", "sophosfilescanner",
    "mbamservice", "mbamtray", "wrsa", "xagt", "taniumclient", "elastic-endpoint", "cyserver", "cytray",
    "tmbmsrv", "ntrtscan",
};

template <std::size_t N>
bool contains(const std::string_view (&list)[N], std::string_view normalized) {
    if (normalized.empty()) return false;
    return std::find(std::begin(list), std::end(list), normalized) != std::end(list);
}

using IsProcessCriticalFn = BOOL(WINAPI*)(HANDLE, PBOOL);

IsProcessCriticalFn resolve_is_process_critical() {
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (!k32) return nullptr;
    // Cast through a generic function pointer type (documented pattern; avoids -Wcast-function-type).
    return reinterpret_cast<IsProcessCriticalFn>(
        reinterpret_cast<void (*)()>(GetProcAddress(k32, "IsProcessCritical")));
}

}  // namespace

std::uint64_t filetime_u64(const FILETIME& ft) noexcept {
    return (static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

std::int64_t filetime_to_unix_ms(std::uint64_t ft100ns) noexcept {
    if (ft100ns < kUnixEpochAs100ns) return 0;
    return static_cast<std::int64_t>((ft100ns - kUnixEpochAs100ns) / 10000ULL);
}

bool creation_time_100ns(HANDLE process, std::uint64_t& out) noexcept {
    FILETIME create{}, exit_time{}, kernel{}, user{};
    if (!process || !GetProcessTimes(process, &create, &exit_time, &kernel, &user)) return false;
    out = filetime_u64(create);
    return out != 0;
}

bool creation_time_ms(HANDLE process, std::int64_t& out_ms) noexcept {
    std::uint64_t ft = 0;
    if (!creation_time_100ns(process, ft)) return false;
    out_ms = filetime_to_unix_ms(ft);
    return out_ms != 0;
}

bool is_running(HANDLE process) noexcept {
    DWORD code = 0;
    if (!process || !GetExitCodeProcess(process, &code)) return false;
    return code == STILL_ACTIVE;
}

bool image_path(HANDLE process, std::wstring& out) {
    out.clear();
    if (!process) return false;
    wchar_t buf[MAX_PATH * 2];
    DWORD size = static_cast<DWORD>(sizeof(buf) / sizeof(buf[0]));
    if (QueryFullProcessImageNameW(process, 0, buf, &size) && size > 0) {
        out.assign(buf, size);
        return true;
    }
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) return false;
    std::wstring big(32768, L'\0');
    size = static_cast<DWORD>(big.size());
    if (!QueryFullProcessImageNameW(process, 0, big.data(), &size) || size == 0) return false;
    big.resize(size);
    out = std::move(big);
    return true;
}

std::wstring_view file_name_of(std::wstring_view path) noexcept {
    const auto pos = path.find_last_of(L"\\/");
    return pos == std::wstring_view::npos ? path : path.substr(pos + 1);
}

std::string normalize_name(std::string_view image_name) {
    // Strip any directory part defensively.
    const auto slash = image_name.find_last_of("\\/");
    if (slash != std::string_view::npos) image_name = image_name.substr(slash + 1);
    std::string s(image_name);
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    if (s.size() > 4 && s.compare(s.size() - 4, 4, ".exe") == 0) s.resize(s.size() - 4);
    return s;
}

bool is_protected_name(std::string_view image_name) {
    const std::string n = normalize_name(image_name);
    return contains(kProtected, n) || contains(kAntiCheat, n);
}

bool is_anti_cheat_name(std::string_view image_name) { return contains(kAntiCheat, normalize_name(image_name)); }

bool is_security_software_name(std::string_view image_name) {
    return contains(kSecurity, normalize_name(image_name));
}

const std::wstring& system_root() {
    static const std::wstring root = [] {
        wchar_t buf[MAX_PATH];
        const UINT n = GetSystemWindowsDirectoryW(buf, MAX_PATH);
        std::wstring r = (n > 0 && n < MAX_PATH) ? std::wstring(buf, n) : std::wstring(L"C:\\Windows");
        if (r.empty() || r.back() != L'\\') r.push_back(L'\\');
        return r;
    }();
    return root;
}

bool is_under_system_root(std::wstring_view path) {
    const std::wstring& root = system_root();
    if (path.size() < root.size()) return false;
    return CompareStringOrdinal(path.data(), static_cast<int>(root.size()), root.data(), static_cast<int>(root.size()),
                                TRUE) == CSTR_EQUAL;
}

bool protection_level(HANDLE process, unsigned long& level) noexcept {
    abi::ProtectionLevelInformation info{};
    if (!process || !GetProcessInformation(process, ProcessProtectionLevelInfo, &info, sizeof(info))) return false;
    level = info.ProtectionLevel;
    return true;
}

bool is_windows_protection(unsigned long level) noexcept {
    switch (level) {
        case kProtectionLevelWinTcbLight:
        case kProtectionLevelWindows:
        case kProtectionLevelWindowsLight:
        case kProtectionLevelLsaLight:
        case kProtectionLevelWinTcb:
        case kProtectionLevelCodeGenLight:
            return true;
        default:
            return false;
    }
}

bool is_critical(HANDLE process, bool& critical) noexcept {
    static const IsProcessCriticalFn fn = resolve_is_process_critical();
    if (!fn || !process) return false;
    BOOL c = FALSE;
    if (!fn(process, &c)) return false;
    critical = c != FALSE;
    return true;
}

bool session_of(std::uint32_t pid, std::uint32_t& session) noexcept {
    DWORD s = 0;
    if (!ProcessIdToSessionId(pid, &s)) return false;
    session = s;
    return true;
}

ProcessHandle open_process(std::uint32_t pid, DWORD access) noexcept {
    return ProcessHandle(OpenProcess(access, FALSE, pid));
}

}  // namespace opensweep::win::procinfo
