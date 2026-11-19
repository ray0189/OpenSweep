#include "storage.hpp"

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>

#include <algorithm>
#include <memory>

namespace opensweep::win {

namespace {

bool owner_is_trusted(HANDLE h) {
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (GetSecurityInfo(h, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr, &sd) !=
        ERROR_SUCCESS)
        return false;
    const bool ok =
        owner && (IsWellKnownSid(owner, WinLocalSystemSid) || IsWellKnownSid(owner, WinBuiltinAdministratorsSid));
    LocalFree(sd);
    return ok;
}

struct Handle {
    HANDLE h = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE x) : h(x) {}
    ~Handle() {
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    bool ok() const { return h != INVALID_HANDLE_VALUE; }
};

// Plain file, not a reparse point, exactly one link.
bool regular_single_link(HANDLE h, ULONGLONG* size) {
    BY_HANDLE_FILE_INFORMATION fi{};
    if (!GetFileInformationByHandle(h, &fi)) return false;
    if (fi.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) return false;
    if (fi.nNumberOfLinks != 1) return false;
    if (size) *size = (static_cast<ULONGLONG>(fi.nFileSizeHigh) << 32) | fi.nFileSizeLow;
    return true;
}

bool write_all(HANDLE h, std::string_view data) {
    std::size_t off = 0;
    while (off < data.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(data.size() - off, 1u << 20));
        DWORD written = 0;
        if (!WriteFile(h, data.data() + off, chunk, &written, nullptr) || written == 0) return false;
        off += written;
    }
    return true;
}

}  // namespace

std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

bool process_is_admin_or_system() {
    BYTE sid_buf[SECURITY_MAX_SID_SIZE];
    DWORD sid_size = sizeof(sid_buf);
    if (!CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, sid_buf, &sid_size)) return false;
    BOOL member = FALSE;
    if (!CheckTokenMembership(nullptr, sid_buf, &member)) return false;
    return member != FALSE;  // SYSTEM's token also has Administrators enabled
}

bool ensure_secure_directory(const std::wstring& dir, const wchar_t* sddl) {
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sd, nullptr)) return false;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = sd;
    sa.bInheritHandle = FALSE;
    bool ok = false;
    if (CreateDirectoryW(dir.c_str(), &sa)) {
        ok = true;
    } else if (GetLastError() == ERROR_ALREADY_EXISTS) {
        Handle h(CreateFileW(dir.c_str(), READ_CONTROL | WRITE_DAC | FILE_READ_ATTRIBUTES,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                             FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (h.ok()) {
            BY_HANDLE_FILE_INFORMATION fi{};
            if (GetFileInformationByHandle(h.h, &fi) && (fi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                !(fi.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && owner_is_trusted(h.h)) {
                BOOL present = FALSE, defaulted = FALSE;
                PACL dacl = nullptr;
                if (GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted) && present) {
                    ok = SetSecurityInfo(h.h, SE_FILE_OBJECT,
                                         DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr,
                                         nullptr, dacl, nullptr) == ERROR_SUCCESS;
                }
            }
        }
    }
    LocalFree(sd);
    return ok;
}

std::wstring prepare_data_dir(std::string& why) {
    if (!process_is_admin_or_system()) {
        why = "not running as SYSTEM or an elevated administrator";
        return {};
    }
    PWSTR program_data = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, KF_FLAG_DEFAULT, nullptr, &program_data)) || !program_data) {
        CoTaskMemFree(program_data);
        why = "%ProgramData% not found";
        return {};
    }
    const std::wstring base(program_data);
    CoTaskMemFree(program_data);
    const std::wstring dir1 = base + L"\\OpenSweep";
    const std::wstring dir2 = dir1 + L"\\core";
    if (!ensure_secure_directory(dir1, kDataDirSddl) || !ensure_secure_directory(dir2, kDataDirSddl)) {
        why = "data directory is untrusted (owner not SYSTEM/Administrators, or a reparse point) or not securable";
        return {};
    }
    return dir2;
}

bool WinFileIO::read_all(const std::string& path, std::string& out, std::size_t max_bytes) {
    Handle h(CreateFileW(widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!h.ok()) return false;  // missing is not an error
    ULONGLONG size = 0;
    if (!regular_single_link(h.h, &size) || size > max_bytes) {
        errors_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    out.assign(static_cast<std::size_t>(size), '\0');
    std::size_t off = 0;
    while (off < out.size()) {
        DWORD got = 0;
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(out.size() - off, 1u << 20));
        if (!ReadFile(h.h, out.data() + off, chunk, &got, nullptr)) {
            errors_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (got == 0) break;
        off += got;
    }
    out.resize(off);
    return true;
}

bool WinFileIO::write_atomic(const std::string& path, std::string_view data) {
    const std::wstring target = widen(path);
    const std::wstring tmp = target + L".tmp";
    {
        Handle h(CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!h.ok() || !regular_single_link(h.h, nullptr) || !write_all(h.h, data) || !FlushFileBuffers(h.h)) {
            errors_.fetch_add(1, std::memory_order_relaxed);
            if (h.ok()) {
                CloseHandle(h.h);
                h.h = INVALID_HANDLE_VALUE;
                DeleteFileW(tmp.c_str());
            }
            return false;
        }
    }
    if (!MoveFileExW(tmp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        errors_.fetch_add(1, std::memory_order_relaxed);
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

bool WinFileIO::append(const std::string& path, std::string_view data) {
    Handle h(CreateFileW(widen(path).c_str(), FILE_APPEND_DATA | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr,
                         OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!h.ok() || !regular_single_link(h.h, nullptr) || !write_all(h.h, data) || !FlushFileBuffers(h.h)) {
        errors_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

}  // namespace opensweep::win
