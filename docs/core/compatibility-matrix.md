# OpenSweep Core compatibility matrix

Targets: Windows 10 22H2 and Windows 11 23H2 / 24H2 / 25H2, on x64 and arm64.

Sources: Microsoft Learn API reference pages ("Minimum supported client" lines) as remembered at the time of writing.
**Nothing here has been run on these Windows versions for this document.** Entries marked **verify** must be checked
against the current Microsoft page and on a real machine/VM of that version before code depends on them. Each Core
feature must check availability at runtime (`GetProcAddress` for newer functions, error codes such as
`ERROR_INVALID_PARAMETER` for newer information classes) and degrade to "not available on this Windows version".

## 1. Platforms

| Platform | Build | Servicing status (Nov 2026, **verify**) | Notes |
|---|---|---|---|
| Windows 10 22H2 x64 | 19045 | Mainstream support ended 14 Oct 2025; consumer ESU to 13 Oct 2026; commercial ESU longer | Still a large installed base. Keep supported for monitoring; avoid Windows 11-only policies. |
| Windows 10 22H2 arm64 | 19045 | as above | Emulates **x86 only, not x64**. Today's x64-only release (`release.yml`) does not run here. Needs an arm64 Electron build and an arm64 service. |
| Windows 11 23H2 x64/arm64 | 22631 | Home/Pro servicing ended Nov 2025; Enterprise/Education Nov 2026 | |
| Windows 11 24H2 x64/arm64 | 26100 | In service | New servicing baseline (germanium). arm64 x64 emulation is "Prism". |
| Windows 11 25H2 x64/arm64 | 26200 | In service | Enablement package on the 24H2 code base (**verify**); same APIs as 24H2 expected. |

Electron 45 (current `devDependencies`) supports Windows 10 and later on x64 and arm64.

Architecture decision for the service: build **native x64 and native arm64** binaries. An x64 service under emulation on
Windows 11 arm64 would mostly work, but emulation is not available on Windows 10 arm64, adds overhead to a component with a
<0.1 % idle CPU target (Part 14), and ETW/PDH data about native processes should be read natively (**verify** any
emulation-specific limits if an x64 fallback is ever shipped).

## 2. API matrix

Legend: ✓ available per docs · ✓¹ available with limitations in Notes · — not available · ? verify.

### Memory (SweepMemory, `metrics` command)

| API | Min client (MS docs) | W10 22H2 | W11 23H2 | W11 24H2 | W11 25H2 | arm64 | Notes |
|---|---|---|---|---|---|---|---|
| `GetPerformanceInfo` | Windows XP (`K32GetPerformanceInfo` in kernel32 from Windows 7) | ✓ | ✓ | ✓ | ✓ | ✓ | Commit total/limit/peak, physical total/available, system cache, kernel paged/nonpaged, handle/process/thread counts. Values in pages × `PageSize`. |
| `GlobalMemoryStatusEx` | Windows 2000 | ✓ | ✓ | ✓ | ✓ | ✓ | `ullTotalPageFile` is the commit limit despite the name. |
| `GetProcessMemoryInfo` + `PROCESS_MEMORY_COUNTERS_EX` | Windows XP (EX: XP SP2 / Vista) | ✓ | ✓ | ✓ | ✓ | ✓ | `PrivateUsage` = private commit. Handle needs `PROCESS_QUERY_LIMITED_INFORMATION` (plus `PROCESS_VM_READ` on old systems — **verify** current rule). Protected processes: limited info only. |
| `PROCESS_MEMORY_COUNTERS_EX2` | ? | ? | ? | ✓? | ✓? | ? | Adds private working set and shared commit. Minimum version **verify**; do not depend on it. |
| `QueryWorkingSetEx` | Windows Vista (`K32QueryWorkingSetEx` from Windows 7) | ✓ | ✓ | ✓ | ✓ | ✓ | Per-page query; expensive. Diagnostics on demand only, never periodic over all processes. Access right: `PROCESS_QUERY_INFORMATION` (**verify** whether limited is enough). |
| `SetProcessInformation(ProcessMemoryPriority)` | Windows 8 | ✓ | ✓ | ✓ | ✓ | ✓ | `MEMORY_PRIORITY_VERY_LOW`..`NORMAL`. Affects standby-list priority of the process's pages. Needs `PROCESS_SET_INFORMATION`. Phase 5 opt-in only. |
| `CreateMemoryResourceNotification` | Windows XP | ✓ | ✓ | ✓ | ✓ | ✓ | Low/high memory signal from the memory manager; useful event-driven trigger. |
| PDH `\Memory\*` (Available Bytes, Committed Bytes, Commit Limit, Pages Input/sec, Page Reads/sec, Pool Paged/Nonpaged Bytes) | Vista (PDH API) | ✓ | ✓ | ✓ | ✓ | ✓ | Use `PdhAddEnglishCounter` (Vista+) to avoid localised names. Counters can be disabled or corrupted (`lodctr /R`); handle `PDH_CSTATUS_NO_OBJECT`. |
| Memory compression indicators | no documented API | ? | ? | ? | ? | ? | The "Memory Compression" process working set is visible via process APIs; no documented counter for compressed store size is known. Treat as **verify**; do not use undocumented `NtQuerySystemInformation` classes (Part 1 rule 8). |

### CPU topology and scheduling (SweepScheduler, `topology` command)

| API | Min client (MS docs) | W10 22H2 | W11 23H2 | W11 24H2 | W11 25H2 | arm64 | Notes |
|---|---|---|---|---|---|---|---|
| `GetLogicalProcessorInformationEx` (`RelationProcessorCore`, `RelationProcessorPackage`, `RelationNumaNode`, `RelationCache`, `RelationGroup`) | Windows 7 | ✓ | ✓ | ✓ | ✓ | ✓ | Group-aware. |
| `PROCESSOR_RELATIONSHIP.EfficiencyClass` | Windows 10 (**verify** build) | ✓¹ | ✓ | ✓ | ✓ | ✓¹ | 0 on homogeneous CPUs. Higher value = higher performance class. On hybrid Intel (12th gen+) and some Arm SoCs it distinguishes core types. |
| `RelationProcessorDie`, `RelationProcessorModule`, `RelationNumaNodeEx` | Windows 11 / Server 2022 (**verify**) | — | ✓? | ✓? | ✓? | ✓? | Die/module info could help with AMD CCD awareness (Part 5 "where reliably exposed"). Treat as optional. |
| `GetSystemCpuSetInformation` | Windows 10 (1507) | ✓ | ✓ | ✓ | ✓ | ✓ | Per-CPU-set `EfficiencyClass`, `SchedulingClass` (**verify** min build for `SchedulingClass`), parked/allocated flags. |
| `SetProcessDefaultCpuSets`, `SetThreadSelectedCpuSets` | Windows 10 (1507) | ✓ | ✓ | ✓ | ✓ | ✓ | Soft preference; hard affinity overrides it. Needs `PROCESS_SET_LIMITED_INFORMATION`. Phase 5 opt-in only; avoid on games, audio, anti-cheat (Part 5). |
| `SetProcessDefaultCpuSetMasks`, `SetThreadSelectedCpuSetMasks` | Windows 11 / Server 2022 | — | ✓ | ✓ | ✓ | ✓ | Mask-based variants. Use the ID-based functions for Windows 10 compatibility. |
| `GetNumaNodeProcessorMaskEx` | Windows 7 | ✓ | ✓ | ✓ | ✓ | ✓ | Single group per call. |
| `GetNumaNodeProcessorMask2` | Windows 11 / Server 2022 | — | ✓ | ✓ | ✓ | ✓ | Multi-group nodes. |
| Processor-group default behaviour | — | old | new | new | new | — | From Windows 11 / Server 2022, processes span all groups by default; on Windows 10, a process starts in one group. Matters only above 64 logical processors. |
| `SetPriorityClass`, `SetThreadPriority` | Windows XP / 2000 | ✓ | ✓ | ✓ | ✓ | ✓ | Never `REALTIME_PRIORITY_CLASS` (today's `win-procs.js` already excludes it). |
| `SetProcessInformation(ProcessPowerThrottling)` with `PROCESS_POWER_THROTTLING_EXECUTION_SPEED` (EcoQoS) | Windows 10 1709 (build 16299); function itself Windows 8 | ✓¹ | ✓ | ✓ | ✓ | ✓ | On Windows 11 this is EcoQoS (lower frequency, efficiency cores preferred). On Windows 10 the effect is "power throttling" and depends on power source/mode — may do nothing on AC desktops (**verify**). Needs `PROCESS_SET_INFORMATION`. Must be journaled and reverted (clear the control mask). Task Manager "Efficiency mode" on Windows 11 combines this with low priority. |
| `PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION` | Windows 11 (22000) | — | ✓ | ✓ | ✓ | ✓ | |
| `SetThreadInformation(ThreadPowerThrottling)` | Function: Windows 8. Class: **verify** | ? | ✓? | ✓? | ✓? | ? | Only for threads of processes we own, or with appropriate access. Not planned before Phase 5. |
| Hybrid scheduling hints (Intel Thread Director) | OS feature | — | ✓ | ✓ | ✓ | n/a | Windows 10 does not use Thread Director; EcoQoS/E-core placement is mainly a Windows 11 behaviour. SweepScheduler must not assume Windows 11 placement semantics on Windows 10. |
| `IsProcessCritical` | Windows 8.1 (**verify**) | ✓ | ✓ | ✓ | ✓ | ✓ | Part of the "never touch critical processes" check, with the protected-process check (`PROCESS_PROTECTION_LEVEL_INFORMATION` via `GetProcessInformation(ProcessProtectionLevelInfo)`, Windows 8 — **verify**). |
| `GetSystemTimes`, `GetProcessTimes`, `GetProcessIoCounters` | XP SP1 / XP | ✓ | ✓ | ✓ | ✓ | ✓ | System CPU % for `metrics`; per-process CPU and I/O deltas (replaces the `win-procs.js` PowerShell loop). |

### Tracing, events, counters (SweepTrace, SweepGuard, SweepIO/SweepLatency)

| API | Min client (MS docs) | W10 22H2 | W11 23H2 | W11 24H2 | W11 25H2 | arm64 | Notes |
|---|---|---|---|---|---|---|---|
| ETW real-time consumer (`StartTrace`, `EnableTraceEx2`, `OpenTrace`, `ProcessTrace`, TDH) | Vista (EnableTraceEx2: Windows 7) | ✓ | ✓ | ✓ | ✓ | ✓ | Sessions are a global resource (64 max). Name sessions uniquely, stop them on service stop and on crash recovery. |
| Kernel events via "NT Kernel Logger" | XP | ✓¹ | ✓¹ | ✓¹ | ✓¹ | ✓¹ | Single global session shared with WPR/xperf/ProcMon — do not use. |
| System logger sessions (`EVENT_TRACE_SYSTEM_LOGGER_MODE`) | Windows 8 | ✓ | ✓ | ✓ | ✓ | ✓ | Up to 8 private kernel sessions. Needed for DPC/ISR (`EVENT_TRACE_FLAG_DPC`, `_INTERRUPT`), context switch, disk I/O flags. Requires admin-level rights (**verify** exact privilege). High volume: on-demand, time-bounded captures only. |
| Manifest kernel providers (Microsoft-Windows-Kernel-Process, -Kernel-Disk, -Kernel-File, -Kernel-Memory, -Kernel-Power) | Windows 7/8 (per provider, **verify**) | ✓ | ✓ | ✓ | ✓ | ✓ | Event IDs and fields vary by build; parse with TDH using the manifest, never fixed offsets. |
| Microsoft-Windows-DxgKrnl (GPU timeouts/scheduling) | Windows 8+ (**verify**) | ✓? | ✓? | ✓? | ✓? | ✓? | Phase 3+ diagnostics. Events differ with WDDM version. |
| Event Log (`EvtSubscribe`, `EvtQuery`, `EvtRender`, `EvtFormatMessage`) | Vista | ✓ | ✓ | ✓ | ✓ | ✓ | Push subscriptions replace today's hourly `Get-WinEvent` poll in `stabilityCheck()`. Same providers: WER-SystemErrorReporting 1001, Kernel-Power 41, WHEA-Logger, Display 4101, disk 7/51/153, Ntfs 55. Provider presence varies by hardware. |
| PDH (`PdhOpenQuery`, `PdhAddEnglishCounter`, `PdhCollectQueryData`) | Vista | ✓ | ✓ | ✓ | ✓ | ✓ | |
| `\Processor Information(_Total)\% Processor Utility` | Windows 8 (**verify**) | ✓ | ✓ | ✓ | ✓ | ✓ | Closer to Task Manager than `% Processor Time` on frequency-scaling CPUs. |
| `\Processor Information(*)\% Processor Performance` | Windows 8 (**verify**) | ✓ | ✓ | ✓ | ✓ | ✓? | Frequency vs base; a throttling indicator, not a temperature. |
| `\PhysicalDisk(*)\Avg. Disk sec/Read|Write`, `Current Disk Queue Length` | Vista | ✓ | ✓ | ✓ | ✓ | ✓ | Storage latency and queue depth (SweepIO). Per-process attribution needs ETW. |
| `\Thermal Zone Information(*)\Temperature` | **verify** | ? | ? | ? | ? | ? | Depends on ACPI firmware; often missing or static on desktops. Treat thermal data as optional. |
| Reliability WMI (`Win32_ReliabilityRecords`, `Win32_ReliabilityStabilityMetrics`) | Vista/7 | ✓ | ✓ | ✓ | ✓ | ✓ | Used today in `win-reliability.js`. Depends on the RAC task; can be disabled. |

### IPC and service

| API | Min client (MS docs) | W10 22H2 | W11 (all) | arm64 | Notes |
|---|---|---|---|---|---|
| `CreateNamedPipeW` with explicit `SECURITY_ATTRIBUTES` | 2000 | ✓ | ✓ | ✓ | Protocol in `ipc-protocol.md`. |
| `PIPE_REJECT_REMOTE_CLIENTS` | Vista | ✓ | ✓ | ✓ | |
| `FILE_FLAG_FIRST_PIPE_INSTANCE` | 2000 SP2 | ✓ | ✓ | ✓ | Client must still verify the server (see `security-audit.md` §5). |
| `ConvertStringSecurityDescriptorToSecurityDescriptorW` (SDDL) | 2000 | ✓ | ✓ | ✓ | |
| `GetNamedPipeClientProcessId`, `GetNamedPipeServerProcessId` | Vista | ✓ | ✓ | ✓ | PIDs are for logging and server verification, not for authorisation. |
| `ImpersonateNamedPipeClient` + `GetTokenInformation(TokenElevation)` | XP / Vista | ✓ | ✓ | ✓ | Basis for the `admin` capability. |
| Service APIs (`StartServiceCtrlDispatcherW`, `RegisterServiceCtrlHandlerExW`, `ChangeServiceConfig2W` with `SERVICE_CONFIG_SERVICE_SID_INFO`, `SERVICE_CONFIG_REQUIRED_PRIVILEGES_INFO`, `SERVICE_CONFIG_FAILURE_ACTIONS`, delayed auto-start) | XP / Vista | ✓ | ✓ | ✓ | |
| `SetDefaultDllDirectories` | Windows 8 (Windows 7 with KB2533623) | ✓ | ✓ | ✓ | DLL search-path hardening. |
| `SetProcessMitigationPolicy` | Windows 8 | ✓ | ✓ | ✓ | Choose policies per test results (**verify** none break ETW/PDH). |
| `RegNotifyChangeKeyValue` | 2000 | ✓ | ✓ | ✓ | Settings guard in the service. |

## 3. Things today's app already depends on

| Dependency | Where | Compatibility note |
|---|---|---|
| Windows PowerShell 5.1 at `%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe` | `pscore.js` | Present on all targets. Which PowerShell binary (native arm64 or emulated) an emulated x64 app gets on arm64 is **verify**. Could be removed as an optional feature in future Windows versions (**verify** announcements). |
| `Get-PhysicalDisk`, `Get-MpComputerStatus`, `Get-NetFirewallProfile`, `Get-SmbServerConfiguration`, `Get-WindowsDriver`, `Get-WindowsOptionalFeature` | `windows.js`, `win-clean.js`, `win-uninstall.js` | Inbox modules; some fail when a third-party AV replaces Defender. |
| `winget.exe` | `win-uninstall.js` | Only when App Installer is installed. |
| Explorer `StartupApproved` binary format | `setStartup()` | Undocumented format shared with Task Manager; keep the read-back verification. |
| Windows Update UX\Settings pause values, `TargetReleaseVersion` policy | `updatesAction()` | `TargetReleaseVersionInfo` must match the product (Windows 10 vs 11); `^\d{2}H\d$` check in `main.js`. |

## 4. Test coverage needed

| Platform | How | Status |
|---|---|---|
| Windows Server (GitHub `windows-latest`) x64 | existing `windows-test.yml` smoke test | runs today for the PowerShell backend; not a client SKU |
| Windows 10 22H2 x64 | VM | not set up |
| Windows 11 24H2 / 25H2 x64 | VM | not set up |
| Windows 11 arm64 | hardware or Azure arm64 VM; GitHub hosted `windows-11-arm` runner (**verify** availability) | not set up |
| Windows 10 arm64 | hardware | not set up; low priority |
| Hybrid CPU (Intel 12th gen+) | hardware | needed for `EfficiencyClass`/EcoQoS checks |
| > 64 logical processors | VM with many vCPUs | needed for processor-group handling |
