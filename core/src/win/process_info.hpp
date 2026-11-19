// Shared helpers for reading process identity and classifying processes (Windows only).
// Documented APIs only: OpenProcess, GetProcessTimes, QueryFullProcessImageNameW,
// GetProcessInformation(ProcessProtectionLevelInfo), IsProcessCritical (resolved at run time
// because some SDK/MinGW headers lack it), ProcessIdToSessionId.
// Never reads command lines, window titles or process memory.
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace opensweep::win::procinfo {

// Documented structures/constants mirrored locally. With _WIN32_WINNT=0x0A00 the SDK derives
// NTDDI_VERSION = NTDDI_WIN10 (base), and some of these are declared only for later NTDDI
// levels (and MinGW lacks some); the layouts below are the documented ABI.
namespace abi {
struct PowerThrottlingState {  // PROCESS_POWER_THROTTLING_STATE
    ULONG Version;
    ULONG ControlMask;
    ULONG StateMask;
};
constexpr ULONG kPowerThrottlingCurrentVersion = 1;  // PROCESS_POWER_THROTTLING_CURRENT_VERSION
constexpr ULONG kPowerThrottlingExecutionSpeed = 0x1;  // PROCESS_POWER_THROTTLING_EXECUTION_SPEED
struct MemoryPriorityInformation {  // MEMORY_PRIORITY_INFORMATION
    ULONG MemoryPriority;
};
constexpr ULONG kMemoryPriorityVeryLow = 1;
constexpr ULONG kMemoryPriorityLow = 2;
constexpr ULONG kMemoryPriorityBelowNormal = 4;
constexpr ULONG kMemoryPriorityNormal = 5;
struct ProtectionLevelInformation {  // PROCESS_PROTECTION_LEVEL_INFORMATION
    DWORD ProtectionLevel;
};
struct CpuSetEntry {  // SYSTEM_CPU_SET_INFORMATION with Type == CpuSetInformation
    DWORD Size;
    DWORD Type;  // 0 = CpuSetInformation
    DWORD Id;
    WORD Group;
    BYTE LogicalProcessorIndex;
    BYTE CoreIndex;
    BYTE LastLevelCacheIndex;
    BYTE NumaNodeIndex;
    BYTE EfficiencyClass;
    BYTE AllFlags;
    DWORD Reserved;
    DWORD64 AllocationTag;
};
static_assert(sizeof(CpuSetEntry) == 32, "SYSTEM_CPU_SET_INFORMATION layout");
constexpr BYTE kCpuSetAllocated = 0x2;  // SYSTEM_CPU_SET_INFORMATION_ALLOCATED
constexpr BYTE kCpuSetRealTime = 0x8;   // SYSTEM_CPU_SET_INFORMATION_REALTIME
constexpr DWORD kProcessSetLimitedInformation = 0x2000;  // PROCESS_SET_LIMITED_INFORMATION
}  // namespace abi

// 100 ns units since 1601-01-01 (FILETIME) -> Unix epoch milliseconds.
std::int64_t filetime_to_unix_ms(std::uint64_t ft100ns) noexcept;
std::uint64_t filetime_u64(const FILETIME& ft) noexcept;

// Process creation time (Unix ms) from a handle with PROCESS_QUERY_LIMITED_INFORMATION.
bool creation_time_ms(HANDLE process, std::int64_t& out_ms) noexcept;
bool creation_time_100ns(HANDLE process, std::uint64_t& out) noexcept;

// True if the process has not exited (GetExitCodeProcess != STILL_ACTIVE is treated as exited;
// a process that legitimately returned 259 is a documented corner case we accept).
bool is_running(HANDLE process) noexcept;

// Full Win32 image path (QueryFullProcessImageNameW). Empty on failure.
bool image_path(HANDLE process, std::wstring& out);
// File-name part of a path ("C:\\x\\a.exe" -> "a.exe").
std::wstring_view file_name_of(std::wstring_view path) noexcept;

// Name normalisation for list checks: ASCII lower-case, trailing ".exe" removed.
std::string normalize_name(std::string_view image_name);

// Built-in protection list from docs/core/phases-2-8.md ("Never act on ...") plus known
// anti-cheat images. Matching is case-insensitive on the file name, ".exe" optional.
bool is_protected_name(std::string_view image_name);
bool is_anti_cheat_name(std::string_view image_name);
// Known AV / EDR images (not exhaustive; protected-light antimalware processes are detected
// separately through the protection level).
bool is_security_software_name(std::string_view image_name);

// %SystemRoot% (GetSystemWindowsDirectoryW), with a trailing backslash; cached.
const std::wstring& system_root();
// Case-insensitive (ordinal) prefix check against system_root().
bool is_under_system_root(std::wstring_view path);

// Protection level via GetProcessInformation(ProcessProtectionLevelInfo).
// Values are the documented PROTECTION_LEVEL_* constants.
constexpr unsigned long kProtectionLevelWinTcbLight = 0;
constexpr unsigned long kProtectionLevelWindows = 1;
constexpr unsigned long kProtectionLevelWindowsLight = 2;
constexpr unsigned long kProtectionLevelAntimalwareLight = 3;
constexpr unsigned long kProtectionLevelLsaLight = 4;
constexpr unsigned long kProtectionLevelWinTcb = 5;
constexpr unsigned long kProtectionLevelCodeGenLight = 6;
constexpr unsigned long kProtectionLevelAuthenticode = 7;
constexpr unsigned long kProtectionLevelPplApp = 8;
constexpr unsigned long kProtectionLevelNone = 0xFFFFFFFEUL;
bool protection_level(HANDLE process, unsigned long& level) noexcept;
// Windows-component protection (WinTcb / Windows / Lsa / CodeGen levels).
bool is_windows_protection(unsigned long level) noexcept;

// IsProcessCritical (kernel32, Windows 8.1+). Returns false if unavailable or the call failed.
bool is_critical(HANDLE process, bool& critical) noexcept;

// Session of a process id; false if it cannot be determined.
bool session_of(std::uint32_t pid, std::uint32_t& session) noexcept;

// RAII process handle.
class ProcessHandle {
public:
    ProcessHandle() = default;
    explicit ProcessHandle(HANDLE h) noexcept : h_(h) {}
    ~ProcessHandle() { reset(); }
    ProcessHandle(const ProcessHandle&) = delete;
    ProcessHandle& operator=(const ProcessHandle&) = delete;
    ProcessHandle(ProcessHandle&& o) noexcept : h_(o.h_) { o.h_ = nullptr; }
    ProcessHandle& operator=(ProcessHandle&& o) noexcept {
        if (this != &o) {
            reset();
            h_ = o.h_;
            o.h_ = nullptr;
        }
        return *this;
    }
    void reset(HANDLE h = nullptr) noexcept {
        if (h_) CloseHandle(h_);
        h_ = h;
    }
    HANDLE get() const noexcept { return h_; }
    explicit operator bool() const noexcept { return h_ != nullptr; }

private:
    HANDLE h_ = nullptr;
};

ProcessHandle open_process(std::uint32_t pid, DWORD access) noexcept;

}  // namespace opensweep::win::procinfo
