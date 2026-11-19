// Secure data directory + durable file I/O for the engine (Windows only).
//
// Data directory: %ProgramData%\OpenSweep\core (shared with core.log). Each level is created with a
// protected DACL (kDataDirSddl: SYSTEM and Administrators full control, Users read, inherited by
// files). An existing directory is used only if it is a real directory (not a reparse point) owned
// by SYSTEM or Administrators; its protected DACL is then re-applied. Otherwise it is refused and the
// engine runs without persistence (NullFileIO). Only an elevated administrator / SYSTEM process
// creates or secures the directory.
//
// WinFileIO (trace::FileIO): every path is built by the engine from a fixed file name (no client
// input). Opens never follow reparse points; files with more than one hard link are refused.
//   write_atomic: CREATE_ALWAYS "<path>.tmp", WriteFile, FlushFileBuffers, MoveFileExW(REPLACE_EXISTING
//                 | WRITE_THROUGH)
//   append:       FILE_APPEND_DATA, WriteFile, FlushFileBuffers (journal write-ahead durability)
#pragma once

#include <atomic>
#include <cstddef>
#include <string>
#include <string_view>

#include "trace.hpp"

namespace opensweep::win {

inline constexpr const wchar_t* kDataDirSddl = L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FR;;;BU)";

// Creates `dir` with `sddl` or validates + re-secures an existing one (see header comment).
bool ensure_secure_directory(const std::wstring& dir, const wchar_t* sddl);
// True if this process runs as SYSTEM or with an enabled Administrators group (elevated).
bool process_is_admin_or_system();
// %ProgramData%\OpenSweep\core, created / validated. Empty on failure (reason in `why`).
std::wstring prepare_data_dir(std::string& why);

std::wstring widen(std::string_view s);

class WinFileIO final : public trace::FileIO {
public:
    bool read_all(const std::string& path, std::string& out, std::size_t max_bytes) override;
    bool write_atomic(const std::string& path, std::string_view data) override;
    bool append(const std::string& path, std::string_view data) override;
    std::uint64_t errors() const noexcept { return errors_.load(std::memory_order_relaxed); }

private:
    std::atomic<std::uint64_t> errors_{0};
};

// Persistence disabled (no trusted data directory): reads find nothing, writes fail.
class NullFileIO final : public trace::FileIO {
public:
    bool read_all(const std::string&, std::string&, std::size_t) override { return false; }
    bool write_atomic(const std::string&, std::string_view) override { return false; }
    bool append(const std::string&, std::string_view) override { return false; }
};

}  // namespace opensweep::win
