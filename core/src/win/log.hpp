// Bounded logging for OpenSweep Core (Windows).
//  * File: %ProgramData%\OpenSweep\core\core.log, rotated to core.log.1 at 1 MiB (so at most
//    ~2 MiB on disk). The directories get a protected DACL (SYSTEM + Administrators full
//    control, Users read) when the process is allowed to set it; if the directory is owned by
//    anyone else or is a reparse point, file logging is disabled (only OutputDebugString).
//  * Every line also goes to OutputDebugStringW, and to stderr in console mode.
//  * Callers must never pass request contents; at most the (registered) command name.
#pragma once

#include <string>
#include <string_view>

namespace opensweep::log {

enum class Level { Debug, Info, Warn, Error };

// Initialises logging. `to_stderr` mirrors lines to the console; `verbose` enables Debug.
void init(bool to_stderr, bool verbose);
void shutdown();

void write(Level level, std::string_view message);
inline void debug(std::string_view m) { write(Level::Debug, m); }
inline void info(std::string_view m) { write(Level::Info, m); }
inline void warn(std::string_view m) { write(Level::Warn, m); }
inline void error(std::string_view m) { write(Level::Error, m); }

// Converts a UTF-16 string to UTF-8 (invalid sequences replaced).
std::string narrow(std::wstring_view w);
// Formats a Win32 error code as "<code> (<message>)".
std::string win32_error(unsigned long code);

}  // namespace opensweep::log
