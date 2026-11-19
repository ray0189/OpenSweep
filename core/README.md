# OpenSweep Core

`OpenSweepCore.exe` is a native C++20 Windows service. It observes the machine (metrics, process
list, stability events), keeps bounded local history and machine-specific baselines, turns
sustained anomalies into evidence-backed incidents, and produces resource recommendations. By
default it runs in **Monitor** mode: every decision is simulated and nothing in Windows is changed.
Reversible process policies (EcoQoS, lower memory priority, BELOW_NORMAL; CPU Sets in Advanced)
are applied only after an administrator switches to Balanced/Advanced *and* opts in to the action
kind; each is journaled before it is applied, verified, rolled back automatically if it does not
help, and rolled back when the service stops or restarts after a crash. It never trims working
sets, purges the standby list, pins affinity or kills processes, and has no network, telemetry or
cloud dependencies. Contract: [`docs/core/phases-2-8.md`](../docs/core/phases-2-8.md); kernel tracing:
[`docs/core/etw.md`](../docs/core/etw.md).

The wire protocol is specified in [`docs/core/ipc-protocol.md`](../docs/core/ipc-protocol.md)
(binding). The Node client is `src/core-client.js`; the Windows integration test is
`test/core-ipc.js`.

## Layout

| Path | Contents |
|---|---|
| `src/portable/` | Platform-independent code, built into the `opensweep_portable` static library: JSON (`json.*`), framing (`framing.*`), protocol + capability-checking `Dispatcher` (`protocol.*`), Phase 1 commands (`commands.*`), shared data model (`telemetry.*`), trace + baselines (`trace.*`, `baseline.*`), guard + incidents (`guard.*`, `incident.*`), memory/scheduler policies (`memory_policy.*`, `scheduler_policy.*`), control engine + journal (`control.*`, `journal.*`), self-healing (`heal.*`), **the engine that wires them together and owns persistence (`engine.*`)**, **the Phase 2–6 IPC commands (`commands2.*`)**, and the kernel-tracing aggregator + detectors (`kernel_stats.*`, `kernel_guard.*`). |
| `src/win/` | Windows-only code: SCM integration (`service.*`), named-pipe server (`pipe_server.*`), Phase 1 metrics + 1 s CPU sampler (`metrics.*`), topology + CPU sets (`topology.*`), logging (`log.*`), collectors (`collectors.*`, `events.*`, `process_info.*`), actuator (`actuator.*`), secure data directory + durable file I/O (`storage.*`), recovery executor (`heal_executor.*`), private kernel trace session (`etw.*`), wiring (`app.*`), CLI (`main.cpp`). |
| `tests/` | `core_tests` (portable, CTest): every portable module incl. `test_engine.cpp` (loop, monitor never acts, balanced apply → verify → rollback, crash recovery, persistence round-trip, synthetic leak → incident, bounded memory, tick cost) and `test_commands2.cpp` (every contract command's shape, argument validation, capabilities), `test_kernel_stats.cpp` (address → driver mapping, attribution, windows / bounds, histograms, self-protection) and `test_kernel_guard.cpp` (driver_latency / disk_hog / paging_hog incl. Monitor never acts). `tests/win/`: `core_win_tests` (collectors, events, actuator, ETW kernel session; Windows only, elevated). |

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
`UNICODE`, `_UNICODE` and `_WIN32_WINNT=0x0A00` are defined. Links `advapi32` (also ETW:
`StartTraceW`, `OpenTraceW`, `ProcessTrace`, `ControlTraceW`, `CloseTrace`), `psapi`, `shell32`,
`ole32`, `uuid`, `pdh`, `wevtapi`. TDH is not used (fixed, size-checked event layouts).

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
`SeImpersonatePrivilege`, `SeCreateGlobalPrivilege`, `SeSystemProfilePrivilege` for the kernel
trace session; every other LocalSystem privilege is removed from the service token; no
`SeDebugPrivilege`). It does not start the service: `sc start OpenSweepCore`. The privilege list
is applied at install time: an installation made before kernel tracing existed must be
re-installed (`--uninstall`, `--install`) to gain `SeSystemProfilePrivilege`.

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
- **Capabilities.** Every client allowed by the pipe ACL gets `read`. `admin` (setMode, setEtw,
  rollbackAll, disablePolicy, approve, exportDiagnostics) is granted only when the client's
  token (read while impersonating) has the Administrators group *enabled* (`CheckTokenMembership`
  ignores the deny-only group of a UAC-filtered token) **and** `TokenElevation` is set. The
  client's session id is taken from the same token (`TokenSessionId`) and used by
  `reportForeground`; a client whose token cannot be read gets `read` and an unknown session.
- **Metrics.** `GlobalMemoryStatusEx`, `GetPerformanceInfo` (page counts × page size → bytes),
  CPU % from `GetSystemTimes` deltas sampled every second by a background thread (0 until the
  first second has elapsed), `GetProcessMemoryInfo` and `GetProcessTimes` for this process.
- **Topology.** Detected once at start-up with `GetLogicalProcessorInformationEx(RelationAll)`:
  `cores` counts `RelationProcessorCore` entries, `efficiencyClasses[k]` is the number of
  *cores* with `EfficiencyClass == k`, `hybrid` means more than one class is populated,
  `logicalProcessors` sums the active processors of all groups, `l3Caches` counts level-3
  `RelationCache` entries.
- **Timestamps.** `startedAt` and `metrics.timestamp` are ISO 8601 UTC with milliseconds.
- **Start.** Topology + CPU sets are detected, the data directory is prepared, the engine loads
  its files and rolls back anything the journal says may still be applied (crash recovery), the
  pipe is opened, then the engine thread starts (`engine::Runner`, one tick every 2 s:
  `SystemCollector::collect` + `EventCollector::poll` outside the engine lock, then
  `Engine::tick`).
- **Stop.** The SCM stop/shutdown control reports `STOP_PENDING` (15 s hint) and signals an event;
  the pipe server cancels outstanding I/O (bounded 2 s wait) and closes all instances, the engine
  thread finishes its current tick and is joined, the kernel trace session is stopped
  (`ControlTraceW(STOP)` + `CloseTrace` + consumer join, bounded 5 s), `Engine::shutdown()` rolls back every active
  policy and persists trace/baselines/incidents/events, the sampler thread is joined and
  `STOPPED` is reported. A non-zero exit (e.g. the pipe name is taken) is reported as a
  service-specific error so the failure actions apply.

## Engine (Phases 2–6)

`engine::Engine` owns Trace, BaselineSet, IncidentStore, Guard, SweepMemory, SweepScheduler,
Journal, ControlEngine and SweepHeal; the platform injects the `Actuator`, the heal `Executor`
and the `FileIO`. Per tick (contract order): `Trace.append` (+ events) → `Baselines.update` →
`Guard.evaluate` (newly opened incidents get a 5 min trace window captured, ≤ 8 kept; a
crash-looping third-party service becomes a *pending approval* to restart it) → `SweepMemory`
(fed the hard-fault baseline and the guard's suspected-leak pids) and `SweepScheduler` (fed the
pids OpenSweep currently throttles; CPU Sets only in Advanced) → `ControlEngine.step` (gates,
write-ahead journal, verification against `verification_metrics(sample)`; no foreground
responsiveness signal exists in session 0, so the scheduler requires *severe* queue pressure)
→ `SweepHeal.tick` → persistence. Tick cost is measured (µs) and reported by `status`.
`GuardConfig.logical_processors` comes from the topology.

| File (`%ProgramData%\OpenSweep\core`) | When written |
|---|---|
| `trace.bin` | every 5 min + at stop |
| `baselines.json` | every 15 min + at stop |
| `events.jsonl` | when new events arrive (append; compacted at 2× bound) + at stop |
| `incidents.jsonl` | when an incident opens / re-opens / resolves; touch-only updates at most every 5 min |
| `journal.jsonl` | write-ahead before every action (append + `FlushFileBuffers`) |
| `config.json` | on every successful `setMode` / `setEtw` (`{version:1, mode, simulation, optIn:[...], etw}`; `etw` optional, default `true`; invalid → monitor defaults + log) |
| `diagnostics.json` | on `exportDiagnostics` (overwritten; bounded) |

Atomic writes: `<file>.tmp` → `FlushFileBuffers` → `MoveFileExW(REPLACE_EXISTING|WRITE_THROUGH)`.
Opens never follow reparse points and refuse files with more than one hard link. The directory
gets a protected DACL (SYSTEM + Administrators full, Users read) and is refused if it is a reparse
point or not owned by SYSTEM/Administrators; without a trusted directory the engine runs in memory
(`NullFileIO`) and — because the journal cannot be written — never applies anything.

Thread safety: every `Engine` method takes one mutex; IPC handlers copy data out and serialise
after releasing it. Collectors are shared with the IPC thread (foreground hints) and the heal
executor through `CollectorSlots` (own mutex; lock order engine → slots, never the reverse).

## Kernel tracing (ETW)

Contract: [`docs/core/etw.md`](../docs/core/etw.md). `win::KernelSession` (`src/win/etw.*`) runs a
**private** SystemTraceProvider session — `StartTraceW` with `EVENT_TRACE_SYSTEM_LOGGER_MODE |
EVENT_TRACE_REAL_TIME_MODE`, name `OpenSweepCoreKernel`, `Wnode.Guid = SystemTraceControlGuid`
(a private session GUID is tried if that is rejected with `ERROR_INVALID_PARAMETER`),
`ClientContext = 1` (QPC), 64 KB × 4–16 buffers, `FlushTimer` 1 s, flags `PROCESS | THREAD |
IMAGE_LOAD | DPC | INTERRUPT | DISK_IO | HARD_FAULTS`. It never touches the "NT Kernel Logger".
A stale session with the same name is stopped by name at start (crash recovery); the running session
is stopped by its own handle. `OpenTraceW` (`REAL_TIME | EVENT_RECORD | RAW_TIMESTAMP`) +
`ProcessTrace` run on a dedicated thread.

Decoding (classic MOF events, by class GUID + opcode, fixed documented layouts, every field
bounds-checked, pointer size from `EVENT_HEADER_FLAG_32_BIT_HEADER` / `_64_BIT_HEADER`; malformed
events are counted and skipped):

| Class / opcodes | Fields used |
|---|---|
| PerfInfo 66 ThreadedDPC, 68 DPC, 69 TimerDPC, 67 ISR, 50 ISR-MSI | `InitialTime` (u64), `Routine` (ptr) |
| Image 10 Load, 3 DCStart, 2 Unload (v2+) | `ImageBase`, `ImageSize`, `ProcessId`, `FileName` (file name only; kernel images = pid 0 or kernel address) |
| Thread 1 Start, 3 DCStart, 2 End | `ProcessId`, `TThreadId` |
| Process 1 Start, 3 DCStart, 2 End (v3+) | `ProcessId`, `ImageFileName` (after skipping `UserSID`; the command line is never read). Unknown layout → name from `QueryFullProcessImageNameW` |
| DiskIo 10 Read, 11 Write (v2+) | `TransferSize`, `HighResResponseTime` (QPC ticks), `IssuingThreadId` (v3) |
| PageFault 32 HardFault | `TThreadId` |

**DPC / ISR duration** = `EventHeader.TimeStamp − InitialTime`: these events are written when the
routine returns and `InitialTime` is its entry time on the same (QPC) clock, which is why the
consumer asks for raw timestamps. Values outside 0–1 s are rejected as malformed.

Aggregation (`kstats::KernelStats`, portable): routine address → driver via sorted image ranges
(unknown → `"unknown"`); disk I/O and hard faults attributed issuing/faulting thread → process;
1-minute windows, 60 kept, top 64 keys per table; histograms `<50 … ≥4000 µs`; system DPC/ISR %;
per-sample (2 s) fields appended to `telemetry::Sample` (`kernel`, `kernelDrivers`,
`kernelProcesses`; ETW per-process hard faults also fill `ProcessSample::hard_faults_per_sec`).
All maps are bounded (threads 65 536, processes 8 192, images 4 096). If no kernel image rundown
arrives within 5 s, driver ranges are seeded from `EnumDeviceDrivers` (approximate sizes).

Self-protection: a supervisor thread (1 s) reads the session's lost events (`EventsLost +
RealTimeBuffersLost`) and the consumer thread's CPU time (`GetThreadTimes`); more than 2 % of one
CPU over 30 s, or loss in every check for 30 s, restarts the session with fewer flags (DPC/ISR
dropped first, then disk, then hard faults; after that tracing stays off) and `etw.reason` says
why. The cost covers the consumer thread (decode + aggregation), not the kernel's own logging
overhead. Off switch: `config.json` `etw:false` or IPC `setEtw` (admin).

Detectors (`kguard::KernelGuard`, after SweepMemory in the tick): `driver_latency` (recommendations
only, never acts), `disk_hog` (candidates in area `disk`, verified by disk latency) and
`paging_hog` (contributor in open memory incidents, candidates in area `memory`). Candidates go
through the normal control engine: verified background workloads only, protected list and
foreground excluded, journaled, verified, rolled back; Monitor mode only simulates.

Self-healing (`WinHealExecutor`): automatic, OpenSweep-owned: recreate the system collector after 3
failed collections; event-subscription reconnect is reported after 1 min of failures (the
collector already retries with back-off and keeps its bookmark); clearing the incident-window
cache and restoring owned policies run inside the engine. Approval-required (admin `approve`):
restart a third-party service via the SCM API (protected list re-checked, drivers/disabled
services refused, ≤ 8 s stop + 5 s start), `ipconfig /flushdns` + `netsh winsock reset`, `sfc
/scannow`, `DISM /Online /Cleanup-Image /RestoreHealth` — always `%SystemRoot%\System32\<exe>`
with fixed arguments (no client or event text on any command line); sfc/DISM are started, not
awaited. Restart Explorer / terminate an application: refused ("must be run from the app").

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
protected DACL (SYSTEM and Administrators full control, Users read — the same DACL as the engine
data files). An existing directory is used only if it is
not a reparse point and is owned by SYSTEM or Administrators (its DACL is then reset); the log
file is opened without following reparse points and rejected if it has more than one hard link
or an untrusted owner. Otherwise file logging is disabled. Non-elevated console runs never
create the directory. Log lines contain the registered command name, error codes, PIDs and
SIDs only — never request contents; control characters are replaced to prevent log injection.

Still open: the service runs as LocalSystem (a dedicated virtual account would be tighter); the
binary is not code-signed; child processes started by approved recoveries (sfc, DISM, netsh)
inherit the service's reduced privilege set and may fail for lack of e.g. backup/restore
privileges — to be validated on Windows.

## What is and isn't validated

- **Validated on Linux (CI and locally):** all portable code via `core_tests` (JSON valid/invalid
  documents, depth and size limits, strict UTF-8, escaping round-trip, big numbers; framing with
  split chunks, exact limit, over limit, zero length; every protocol error path and the
  dispatcher capability check; ring buffer; statistics; Phase 1 result shapes against a fake
  provider). Also run under AddressSanitizer/UBSan and a short mutation fuzz of the parser and
  dispatcher during development.
- **Kernel tracing:** the aggregator and the three detectors are unit-tested on Linux with synthetic
  events. The ETW session (`etw.cpp`, `test_win_etw.cpp`) was only cross-compiled with MinGW here;
  it counts as tested once the `windows` CI job runs it (session starts, DPC/ISR seen, disk I/O of
  the test attributed to its own pid, clean stop, no orphan session). Finding a specific bad driver
  on a real PC is only known after a user runs it.
- **Windows code:** compiled and linked with MinGW-w64 GCC during development (not the target
  toolchain; `topology.cpp` reads `EfficiencyClass` as the byte after `Flags` so both MinGW and
  MSVC headers work). It has **not** been compiled with MSVC or run on Windows here. `test/core-ipc.js`
  exercises the real executable on Windows (console mode, protocol errors, 64 KiB limits,
  8-client limit, 5 s stall cut-off, service install/start/stop/uninstall).
- **Not tested automatically:** the 60 s idle timeout, log rotation and directory-ownership
  checks, behaviour under memory pressure, sleep/resume, hybrid/multi-group machines.
