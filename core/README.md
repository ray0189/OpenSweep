# OpenSweep Core (Phase 1)

`OpenSweepCore.exe` is a native C++20 Windows service. Phase 1 only **monitors**: it serves
read-only system metrics and CPU topology to the OpenSweep UI over a local, ACL-restricted
named pipe. It never changes Windows settings, never trims working sets or "frees RAM", and
has no network, telemetry or cloud dependencies.

The wire protocol is specified in [`docs/core/ipc-protocol.md`](../docs/core/ipc-protocol.md)
(binding). The Node client is `src/core-client.js`; the Windows integration test is
`test/core-ipc.js`.

## Layout

| Path | Contents |
|---|---|
| `src/portable/` | Platform-independent code, built into the `opensweep_portable` static library: JSON parser/serializer (`json.*`), length-prefixed framing (`framing.*`), request validation, responses and the capability-checking `Dispatcher` (`protocol.*`), Phase 1 command result builders over an abstract `SystemProvider` (`commands.*`), a bounded `RingBuffer`, and statistics helpers (`stats.*`: EWMA, median, MAD, robust z-score) for later phases. |
| `src/win/` | Windows-only code for the `OpenSweepCore` executable: SCM integration (`service.*`), overlapped named-pipe server (`pipe_server.*`), metrics and the 1 s CPU sampler (`metrics.*`), topology (`topology.*`), bounded logging (`log.*`), wiring (`app.*`) and the CLI (`main.cpp`). |
| `tests/` | `core_tests`: unit tests for everything in `src/portable/` (self-contained runner, no dependencies), registered with CTest. |

## Building

Requirements: CMake ≥ 3.20 and a C++20 compiler. No third-party dependencies.

### Windows (MSVC, Visual Studio 2022)

```bat
cmake -S core -B core/build -A x64
cmake --build core/build --config Release
ctest --test-dir core/build -C Release --output-on-failure
```

The executable is `core\build\Release\OpenSweepCore.exe`. It links the static MSVC runtime
(`/MT`), so no VC++ redistributable is needed. Compiler flags: `/W4 /permissive- /EHsc /utf-8
/guard:cf`; linker: `/guard:cf /DYNAMICBASE /NXCOMPAT`. `WIN32_LEAN_AND_MEAN`, `NOMINMAX`,
`UNICODE`, `_UNICODE` and `_WIN32_WINNT=0x0A00` are defined. Links `advapi32`, `psapi`,
`shell32`, `ole32`, `uuid`.

### Linux / macOS (portable part only)

```sh
cmake -S core -B core/build
cmake --build core/build
ctest --test-dir core/build --output-on-failure
```

On non-Windows platforms only `opensweep_portable` and `core_tests` are built (GCC/Clang with
`-Wall -Wextra`).

## CLI

```
OpenSweepCore.exe                          run as the Windows service (started by the SCM)
OpenSweepCore.exe --console [--verbose]    run the pipe server in the foreground until Ctrl+C
OpenSweepCore.exe --install                create the service (elevated prompt)
OpenSweepCore.exe --uninstall              stop and delete the service (elevated prompt)
OpenSweepCore.exe --version                print "OpenSweepCore 0.1.0 (protocol 1)"
```

All commands exit 0 on success (`--uninstall` also exits 0 if the service is not installed).

`--install` creates service `OpenSweepCore` ("OpenSweep Core"): own process, automatic start,
LocalSystem, quoted binary path, a description, failure actions *restart after 5 s, restart
after 30 s, then none* (counter reset after 24 h, also applied to non-crash failures), service
SID type *unrestricted*, and a reduced required-privileges list (`SeChangeNotifyPrivilege`,
`SeImpersonatePrivilege`, `SeCreateGlobalPrivilege`; every other LocalSystem privilege is
removed from the service token). It does not start the service: `sc start OpenSweepCore`.

Install the binary in a directory that standard users cannot write to (e.g. under
`C:\Program Files\OpenSweep\`), since the service runs it as LocalSystem.

`--console` needs an elevated prompt to get all 8 pipe instances: creating additional instances
requires `FILE_CREATE_PIPE_INSTANCE`, which the pipe ACL grants only to SYSTEM and
Administrators. A non-elevated console run serves a single instance and logs a warning.

## Design notes

- **One I/O thread, overlapped I/O.** Up to 8 pipe instances, each with a manual-reset event
  and a small state machine (listen → read → process → write → …). `WaitForMultipleObjects`
  waits on the stop event plus the event of every instance with pending I/O; after each wake all
  instances are polled with `HasOverlappedIoCompleted`, so no instance starves. Requests on one
  connection are processed in order, one at a time; reads pause while parsed frames are queued
  (back-pressure).
- **Deadlines.** A request must be complete within 5 s of its first byte, else the server sends
  `timeout` (id 0) and closes. Idle connections close after 60 s. A response write that does not
  complete within 5 s closes the connection.
- **Error-then-close.** After `too_large`, `bad_frame` or `timeout` the server writes the error,
  then keeps reading (and discarding) for up to 1 s before `DisconnectNamedPipe`, because
  disconnecting immediately would discard the unread response in the pipe buffer. A client that
  closes first ends this earlier.
- **Validation order.** JSON parse (size ≤ 64 KiB, depth ≤ 32, strict UTF-8, no duplicate keys)
  → must be an object → `v` must be 1 (`bad_version`, id echoed if valid) → `id` must be an
  integer with |id| ≤ 2^53−1 (else `bad_json`, id 0) → `cmd` must be a non-empty string
  (`bad_json`) of at most 64 bytes (`unknown_cmd`) → `args`, if present, must be an object
  (`bad_args`) → unknown command (`unknown_cmd`) → capability (`forbidden`) → handler.
  Handler exceptions become `internal` without exposing their text. A response that would exceed
  1 MiB is replaced by an `internal` error.
- **Capabilities.** Every client allowed by the pipe ACL gets `read`. `admin` is granted only
  when the client's token has the Administrators group enabled (elevated); no Phase 1 command
  requires it.
- **Metrics.** `GlobalMemoryStatusEx`, `GetPerformanceInfo` (page counts × page size → bytes),
  CPU % from `GetSystemTimes` deltas sampled every second by a background thread (0 until the
  first second has elapsed), `GetProcessMemoryInfo` and `GetProcessTimes` for this process.
- **Topology.** Detected once at start-up with `GetLogicalProcessorInformationEx(RelationAll)`:
  `cores` counts `RelationProcessorCore` entries, `efficiencyClasses[k]` is the number of
  *cores* with `EfficiencyClass == k`, `hybrid` means more than one class is populated,
  `logicalProcessors` sums the active processors of all groups, `l3Caches` counts level-3
  `RelationCache` entries.
- **Timestamps.** `startedAt` and `metrics.timestamp` are ISO 8601 UTC with milliseconds.
- **Stop.** The SCM stop/shutdown control reports `STOP_PENDING` (5 s hint) and signals an event;
  the pipe server cancels outstanding I/O (bounded 2 s wait), closes all instances, the sampler
  thread is joined and `STOPPED` is reported. A non-zero exit (e.g. the pipe name is taken) is
  reported as a service-specific error so the failure actions apply.

## Security notes

Pipe `\\.\pipe\OpenSweepCore.v1`:

| Setting | Why |
|---|---|
| SDDL `D:P(D;;GA;;;NU)(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)` via `ConvertStringSecurityDescriptorToSecurityDescriptorW` | Protected DACL (no inherited ACEs). Network logons are denied; SYSTEM and Administrators get full control; interactive users may read/write (connect and talk) but not create pipe instances, so they cannot add a rogue server instance to our pipe. Services, batch jobs and anonymous callers have no access. |
| `FILE_FLAG_FIRST_PIPE_INSTANCE` on the first instance | If another process already created the name (pipe squatting, to impersonate the service to the UI), creation fails and the service refuses to start instead of silently joining it. When an instance must be recreated while no other instance is alive, the flag is used again. |
| `PIPE_REJECT_REMOTE_CLIENTS` | Rejects connections arriving over SMB, independent of the ACL. |
| Byte mode, 64 KiB kernel buffers, 8 instances | Framing is the protocol's length prefix; the instance limit bounds concurrent clients and memory. |
| Request limit 64 KiB checked from the 4-byte header | Oversized frames are rejected before any payload is buffered. |
| `GetNamedPipeClientProcessId` + `ImpersonateNamedPipeClient` (token query only) | The client PID and user SID are written to the audit log. Impersonation is reverted immediately; if `RevertToSelf` fails the process terminates rather than continue under the client's identity. |

Process hardening at start-up: `HeapEnableTerminationOnCorruption`,
`SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32)` and `SetDllDirectory(L"")` (DLL
planting), image-load mitigation (no remote or low-integrity images) and extension-point
disabling (AppInit DLLs, legacy hooks).

Logging: `%ProgramData%\OpenSweep\core\core.log`, rotated to `core.log.1` at 1 MiB (≤ ~2 MiB
total), plus `OutputDebugString` (and stderr in console mode). Directories are created with a
protected DACL (SYSTEM and Administrators only). An existing directory is used only if it is
not a reparse point and is owned by SYSTEM or Administrators (its DACL is then reset); the log
file is opened without following reparse points and rejected if it has more than one hard link
or an untrusted owner. Otherwise file logging is disabled. Non-elevated console runs never
create the directory. Log lines contain the registered command name, error codes, PIDs and
SIDs only — never request contents; control characters are replaced to prevent log injection.

Still open for later phases: the service runs as LocalSystem (a dedicated virtual account
would be tighter); the binary is not code-signed; the `admin` capability is unused.

## What is and isn't validated

- **Validated on Linux (CI and locally):** all portable code via `core_tests` (JSON valid/invalid
  documents, depth and size limits, strict UTF-8, escaping round-trip, big numbers; framing with
  split chunks, exact limit, over limit, zero length; every protocol error path and the
  dispatcher capability check; ring buffer; statistics; Phase 1 result shapes against a fake
  provider). Also run under AddressSanitizer/UBSan and a short mutation fuzz of the parser and
  dispatcher during development.
- **Windows code:** compiled and linked with MinGW-w64 GCC during development (not the target
  toolchain; `EfficiencyClass` is missing from MinGW headers and was shimmed for that check
  only). It has **not** been compiled with MSVC or run on Windows here. `test/core-ipc.js`
  exercises the real executable on Windows (console mode, protocol errors, 64 KiB limits,
  8-client limit, 5 s stall cut-off, service install/start/stop/uninstall).
- **Not tested automatically:** the 60 s idle timeout, log rotation and directory-ownership
  checks, behaviour under memory pressure, sleep/resume, hybrid/multi-group machines.
