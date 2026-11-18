#include "log.hpp"

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>

#include <cstdio>
#include <mutex>

#include "commands.hpp"

namespace opensweep::log {

namespace {

constexpr ULONGLONG kMaxLogBytes = 1024 * 1024;  // rotate at 1 MiB
constexpr std::size_t kMaxMessageChars = 1024;
// Log directories: SYSTEM and Administrators only, inherited by files created inside.
constexpr const wchar_t* kDirSddl = L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)";

struct State {
    std::mutex mu;
    bool initialized = false;
    bool to_stderr = false;
    bool verbose = false;
    HANDLE file = INVALID_HANDLE_VALUE;
    ULONGLONG size = 0;
    std::wstring path;      // ...\core.log
    std::wstring rotated;   // ...\core.log.1
};

State& state() {
    static State s;
    return s;
}

std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

const char* level_name(Level l) {
    switch (l) {
        case Level::Debug: return "DEBUG";
        case Level::Info: return "INFO ";
        case Level::Warn: return "WARN ";
        case Level::Error: return "ERROR";
    }
    return "INFO ";
}

bool owner_is_trusted(HANDLE h) {
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (GetSecurityInfo(h, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr, &sd) !=
        ERROR_SUCCESS)
        return false;
    const bool ok = owner && (IsWellKnownSid(owner, WinLocalSystemSid) || IsWellKnownSid(owner, WinBuiltinAdministratorsSid));
    LocalFree(sd);
    return ok;
}

// Only elevated administrators or SYSTEM create/secure the log directory. A non-elevated
// console run must not leave a user-owned directory behind (the service would refuse it).
bool process_is_admin_or_system() {
    BYTE sid_buf[SECURITY_MAX_SID_SIZE];
    DWORD sid_size = sizeof(sid_buf);
    if (!CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, sid_buf, &sid_size)) return false;
    BOOL member = FALSE;
    if (!CheckTokenMembership(nullptr, sid_buf, &member)) return false;
    return member != FALSE;
}

// Creates `dir` with a protected DACL, or validates an existing one (not a reparse point,
// owned by SYSTEM/Administrators) and re-applies the protected DACL.
bool ensure_secure_directory(const std::wstring& dir) {
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(kDirSddl, SDDL_REVISION_1, &sd, nullptr)) return false;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = sd;
    sa.bInheritHandle = FALSE;
    bool ok = false;
    if (CreateDirectoryW(dir.c_str(), &sa)) {
        ok = true;
    } else if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HANDLE h = CreateFileW(dir.c_str(), READ_CONTROL | WRITE_DAC | FILE_READ_ATTRIBUTES,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            BY_HANDLE_FILE_INFORMATION fi{};
            if (GetFileInformationByHandle(h, &fi) && (fi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                !(fi.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && owner_is_trusted(h)) {
                BOOL present = FALSE, defaulted = FALSE;
                PACL dacl = nullptr;
                if (GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted) && present) {
                    ok = SetSecurityInfo(h, SE_FILE_OBJECT,
                                         DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr,
                                         nullptr, dacl, nullptr) == ERROR_SUCCESS;
                }
            }
            CloseHandle(h);
        }
    }
    LocalFree(sd);
    return ok;
}

// Opens the log file for appending without following reparse points; rejects hard links
// and files not owned by SYSTEM/Administrators.
HANDLE open_log_file(const std::wstring& path, ULONGLONG& size) {
    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA | FILE_READ_ATTRIBUTES | READ_CONTROL, FILE_SHARE_READ,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) return h;
    BY_HANDLE_FILE_INFORMATION fi{};
    if (!GetFileInformationByHandle(h, &fi) || (fi.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        (fi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fi.nNumberOfLinks != 1 || !owner_is_trusted(h)) {
        CloseHandle(h);
        return INVALID_HANDLE_VALUE;
    }
    size = (static_cast<ULONGLONG>(fi.nFileSizeHigh) << 32) | fi.nFileSizeLow;
    return h;
}

void rotate_locked(State& s) {
    if (s.file != INVALID_HANDLE_VALUE) {
        CloseHandle(s.file);
        s.file = INVALID_HANDLE_VALUE;
    }
    // Replace core.log.1 with the current file; on failure, truncate by deleting.
    if (!MoveFileExW(s.path.c_str(), s.rotated.c_str(), MOVEFILE_REPLACE_EXISTING)) DeleteFileW(s.path.c_str());
    s.size = 0;
    s.file = open_log_file(s.path, s.size);
}

std::string sanitize(std::string_view m) {
    std::string out;
    out.reserve(m.size() < kMaxMessageChars ? m.size() : kMaxMessageChars);
    for (char c : m) {
        if (out.size() >= kMaxMessageChars) {
            out.append("...");
            break;
        }
        const auto u = static_cast<unsigned char>(c);
        out.push_back(u < 0x20 || u == 0x7F ? '?' : c);  // no CR/LF injection into the log
    }
    return out;
}

}  // namespace

std::string narrow(std::wstring_view w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string win32_error(unsigned long code) {
    wchar_t buf[512];
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_MAX_WIDTH_MASK,
                                   nullptr, code, 0, buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])), nullptr);
    std::wstring msg(buf, n);
    while (!msg.empty() && (msg.back() == L' ' || msg.back() == L'\r' || msg.back() == L'\n')) msg.pop_back();
    return std::to_string(code) + (msg.empty() ? std::string() : " (" + narrow(msg) + ")");
}

void init(bool to_stderr, bool verbose) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mu);
    if (s.initialized) return;
    s.initialized = true;
    s.to_stderr = to_stderr;
    s.verbose = verbose;

    if (!process_is_admin_or_system()) {
        OutputDebugStringW(L"OpenSweepCore: not elevated; file logging disabled\n");
        return;
    }
    PWSTR program_data = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, KF_FLAG_DEFAULT, nullptr, &program_data)) || !program_data) {
        CoTaskMemFree(program_data);
        OutputDebugStringW(L"OpenSweepCore: ProgramData not found; file logging disabled\n");
        return;
    }
    const std::wstring base(program_data);
    CoTaskMemFree(program_data);

    const std::wstring dir1 = base + L"\\OpenSweep";
    const std::wstring dir2 = dir1 + L"\\core";
    if (!ensure_secure_directory(dir1) || !ensure_secure_directory(dir2)) {
        OutputDebugStringW(L"OpenSweepCore: log directory missing, untrusted or not securable; file logging disabled\n");
        if (to_stderr) std::fputs("warning: log directory is untrusted or not securable; file logging disabled\n", stderr);
        return;
    }
    s.path = dir2 + L"\\core.log";
    s.rotated = dir2 + L"\\core.log.1";
    s.file = open_log_file(s.path, s.size);
    if (s.file == INVALID_HANDLE_VALUE) {
        OutputDebugStringW(L"OpenSweepCore: cannot open core.log safely; file logging disabled\n");
        return;
    }
    if (s.size >= kMaxLogBytes) rotate_locked(s);
}

void shutdown() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mu);
    if (s.file != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(s.file);
        CloseHandle(s.file);
        s.file = INVALID_HANDLE_VALUE;
    }
}

void write(Level level, std::string_view message) {
    State& s = state();
    if (level == Level::Debug && !s.verbose) return;
    std::string line = commands::format_iso8601_utc(commands::now_unix_ms());
    line += ' ';
    line += level_name(level);
    line += " [";
    line += std::to_string(GetCurrentThreadId());
    line += "] ";
    line += sanitize(message);
    line += "\r\n";

    const std::wstring wline = widen("OpenSweepCore: " + line);
    OutputDebugStringW(wline.c_str());

    std::lock_guard<std::mutex> lock(s.mu);
    if (s.to_stderr) {
        std::fwrite(line.data(), 1, line.size() - 2, stderr);  // without CRLF
        std::fputc('\n', stderr);
        std::fflush(stderr);
    }
    if (s.file == INVALID_HANDLE_VALUE) return;
    if (s.size + line.size() > kMaxLogBytes) rotate_locked(s);
    if (s.file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    if (WriteFile(s.file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr)) s.size += written;
}

}  // namespace opensweep::log
