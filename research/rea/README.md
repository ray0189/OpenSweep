# Windows internals research (master plan Part 11)

This folder holds research notes on Windows behaviour that OpenSweep Core depends on (scheduling, memory manager,
storage and graphics stacks). It is the only place such notes may live (Part 11).

## Status and limitation (read first)

- **No reverse engineering of Windows binaries has been performed.** REA-style disassembly or decompilation of
  `ntoskrnl.exe`, `ntdll.dll`, `hal.dll`, `dxgkrnl.sys` or storage/network drivers was **not** done, and nothing in
  OpenSweep is based on it.
- **Why:** Microsoft's licence terms for Windows restrict reverse engineering of its binaries. The project owner's note
  in Part 11 of the plan says that, unless legal clearance is confirmed, this part is carried out with **public tools and
  documentation only**. No legal clearance has been recorded. An "REA" tool was also not available in the development
  environment.
- **Allowed sources** (public, documented, licence-compatible):
  - Microsoft documentation (learn.microsoft.com: Win32/WDK API reference, "Windows Internals"-style articles published
    by Microsoft, ETW provider manifests as shipped with Windows).
  - **WinDbg** with **Microsoft public symbols** (`srv*` symbol server) — for observing documented structures and
    call stacks on a test machine, *not* for building code on top of private structures.
  - **Windows Performance Recorder (WPR)** and **Windows Performance Analyzer (WPA)**, built-in profiles and custom
    `.wprp` profiles.
  - **ETW traces** (`xperf`, `tracerpt`, `logman`, the system logger) and the providers' published manifests
    (`wevtutil gp <provider>`, `logman query providers`).
  - Performance counters (PDH), Event Log, and observed behaviour of documented APIs.
- **Never** (Part 11 rules, unchanged by any finding): undocumented kernel offsets or private structures in production
  code; anything that bypasses Windows kernel protections (PatchGuard, HVCI, PPL, driver signature enforcement).

If legal clearance is confirmed in the future, record it here (who, when, scope) **before** any binary analysis, and
keep the documented / observed / hypothesis separation below.

## How to write a finding

One Markdown file per topic: `research/rea/YYYY-MM-DD-<topic>.md`, using the template below. Every statement goes into
exactly one of four buckets, so readers can tell what is guaranteed from what is merely seen:

| Bucket | Meaning | May production code rely on it? |
|---|---|---|
| **Documented** | Stated in Microsoft documentation or a published API contract. Link it. | Yes |
| **Observed** | Measured on a named machine/build with a public tool (WPA, ETW, PDH, WinDbg + public symbols). Include the trace/command so it can be repeated. | Only with a runtime check and a fallback; re-verify per Windows release |
| **Hypothesis** | An explanation of observed behaviour not confirmed by documentation. (Reverse-engineered hypotheses would go here too — none exist.) | No — design must work if it is false |
| **Unsupported assumption** | Something we currently assume without evidence. Listed so it gets tested. | No |

### Template

```markdown
# <Topic>

- Date: YYYY-MM-DD · Author: <handle>
- Question: <what OpenSweep needs to know, and which module depends on it>
- Windows: <edition> <version> build <major.minor.build.UBR> (from `winver` / `HKLM\...\CurrentVersion`)
- Machine: <CPU, cores/threads, hybrid?, RAM, storage, VM or physical>
- Binary versions involved: <file and version, e.g. ntoskrnl.exe 10.0.26100.2454 (from file properties / `Get-Item ... | % VersionInfo`)>
- Tools: <WPR profile, WPA version, WinDbg version + symbol path, ETW providers and keywords, counters>
- Method: <exact commands, workload, duration, run count>

## Documented
- <statement> — <link>

## Observed
- <statement> — <trace file name / counter values / WinDbg command output>, n = <runs>

## Hypotheses
- <statement> — <what would confirm or refute it>

## Unsupported assumptions
- <statement> — <how to test it>

## Consequences for OpenSweep Core
- <design decision, runtime check, or "none">
```

## Open questions worth researching with public tools

These are the places where OpenSweep Core currently relies on observed or assumed behaviour (see also
`docs/core/compatibility-matrix.md` items marked "verify"):

| Question | Module | Public method |
|---|---|---|
| Effect of EcoQoS (`ProcessPowerThrottling` execution speed) on a background process's CPU time and on foreground latency, per hybrid/non-hybrid CPU | control / scheduler_policy | WPA CPU Usage (Precise) + ready time, `bench/run.ps1` contention workload |
| Effect of lowered memory priority on which pages are trimmed / repurposed first under pressure | memory_policy | `Memory\` counters, WPR memory profile (ReferenceSet / ResidentSet), RAMMap (Sysinternals) |
| Whether `Processor Queue Length` is a usable contention signal on many-core and hybrid CPUs | guard / scheduler_policy | Compare with ETW CSwitch / ReadyThread in WPA |
| Latency and loss of real-time ETW consumers for the kernel providers used by collectors | win collectors | ETW session statistics (`EventsLost`, `BuffersLost`), WPR |
| CPU set selection on hybrid CPUs (efficiency class semantics) | scheduler_policy | `GetSystemCpuSetInformation` documentation + observed thread placement in WPA |

None of these has been researched yet; no finding files exist.
