# OpenSweep Core — kernel tracing (ETW) contract

Goal: kernel-level visibility without a driver, so Core can name the driver or app behind stutter, disk
thrashing and paging, and turn that into incidents and reversible fixes.

## Session
- A **private** kernel session using documented APIs only: `StartTraceW` with
  `EVENT_TRACE_SYSTEM_LOGGER_MODE | EVENT_TRACE_REAL_TIME_MODE` (Windows 8+), session name
  `OpenSweepCoreKernel`, then `TraceSetInformation(TraceSystemTraceEnableFlagsInfo)` (or
  `EnableFlags` in the properties) and `OpenTraceW`/`ProcessTrace` on a dedicated thread.
  Never uses or stops the global "NT Kernel Logger" (other tools such as LatencyMon may own it).
- Flags: `PROCESS`, `THREAD`, `IMAGE_LOAD`, `DPC`, `INTERRUPT`, `DISK_IO`, `HARD_FAULTS`.
  No context-switch, no file-name, no registry, no network tracing (cost and privacy).
- Bounded: small buffers (e.g. 64 KB × 4–16), `FlushTimer` 1 s. Events are aggregated in place, never
  stored raw. Stopped with `ControlTraceW(EVENT_TRACE_CONTROL_STOP)` on service stop and on start-up if a
  stale `OpenSweepCoreKernel` session exists (crash recovery).
- Self-protection: if processing costs > 2 % of one CPU over 30 s, or events are lost
  continuously, the session is stopped and restarted with fewer flags (DPC/ISR → disk → faults), and
  `etw.status` says so. Off switch: `config.json` `etw: false` and IPC `setEtw` (admin).
- Privacy: only process image names, driver file names, byte counts and timings. No file paths, no
  stacks, no payloads.

## Aggregation (portable: `core/src/portable/kernel_stats.{hpp,cpp}`, unit-tested on Linux)
Fed by decoded events through a small interface so tests can inject synthetic events:
- **Per driver (DPC + ISR):** count, total µs, max µs, histogram buckets
  (<50, <100, <250, <500, <1000, <2000, <4000, ≥4000 µs). Routine addresses are mapped to drivers
  from image-load events of kernel images (address ranges); unknown → `"unknown"`.
- **Per process (disk):** read/write bytes, I/O count, mean and max latency ms (from
  `HighResResponseTime`), attributed via issuing thread → process (thread events).
- **Per process (hard faults):** count/s, attributed via thread → process.
- **System:** DPC % and ISR % of all CPUs, worst single DPC/ISR µs, events lost.
- Windows: 1-minute windows; keeps the last 60 windows (1 h) per driver/process, bounded to top 64 keys
  per window. Feeds `telemetry::Sample` (new fields appended at the end) every tick.

## Detection (SweepGuard additions)
- `driver_latency`: a driver's max DPC/ISR ≥ 1000 µs repeatedly (≥ 3 windows in 10 min) or its
  DPC/ISR share is far above baseline → incident "Driver X correlated with latency spikes (max N µs)"
  with evidence; recommended actions: update/roll back that driver (by name, with the device class if
  known), check power settings. Never auto-acts on drivers.
- `disk_hog`: a non-foreground process doing sustained heavy I/O (e.g. ≥ 20 MB/s for ≥ 2 min) while
  disk latency is above baseline → incident + Balanced-mode candidate (EcoQoS / lower memory priority /
  BELOW_NORMAL on that verified background process, via the existing control engine and journal).
- `paging_hog`: a process responsible for most hard faults during memory pressure → contributor in
  memory incidents + candidate as above.
These reuse confidence, "correlated with" wording, hysteresis, cooldowns, journaling and rollback.

## IPC additions (read unless noted)
| cmd | result |
|---|---|
| `latency` | `{ status:{ running, flags[], eventsLost, costPercent, reason? }, system:{ dpcPercent, isrPercent, worstDpcUs, worstIsrUs }, drivers:[{ name, dpcCount, isrCount, totalUs, maxUs, histogram:[8] }] }` (last 10 min, top 20) |
| `diskActivity` | `{ processes:[{ pid, name, readBps, writeBps, ios, meanLatencyMs, maxLatencyMs }] }` (last 2 min, top 20) |
| `hardFaults` | `{ processes:[{ pid, name, faultsPerSec }] }` (last 2 min, top 20) |
| `setEtw` (admin) | `{ enabled }` → `{ enabled, running }` |
`status` gains `etw:{ running, flags[], eventsLost, costPercent }`.

## UI
A **Latency** tab on the Core page: worst drivers with histogram bars and plain explanations
("ndis.sys = network driver"), disk activity and hard-fault tables, ETW status/cost, and the related
incidents/recommendations. Simulated data in demo mode.

## Honest testing
Portable aggregation and detectors: Linux unit tests. Kernel session: `core_win_tests` + `test/core-ipc.js`
on the GitHub Windows runner (session starts, sees DPC/ISR and disk events for a generated workload,
attributes disk I/O to the test's own process, stops cleanly, no orphan session). Real-PC behaviour
(e.g. finding a specific bad driver) is only known after a user runs it.
