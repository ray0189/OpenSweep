# OpenSweep Core — master implementation prompt

Owner's prompt for turning OpenSweep into an evidence-driven Windows stability and resource-management
platform. Condensed from the original; every requirement is kept. **One change agreed with the owner:**
Part 11 no longer reverse-engineers Windows kernel binaries (licence restrictions); research uses public
Microsoft tools and documentation only.

## Mission
Build OpenSweep Core: observe system behaviour, detect developing problems, apply carefully controlled
optimisations, measure their effect and automatically revert harmful changes. Not another tweak utility.

## Non-negotiable rules
1. Inspect the repo before changing it; preserve features and user settings; no needless rewrites; document breaking changes.
2. Native C++20/23 for the performance-critical background service; keep the Electron UI.
3. Documented Windows APIs only. No undocumented kernel offsets, no disabling PatchGuard/Secure Boot/HVCI, no replacing the scheduler or memory manager.
4. No fake RAM boosters or routine cache clearing (no periodic EmptyWorkingSet, no standby-list purging).
5. Every active optimisation is measurable, reversible where possible and safety-checked; privileged operations are explicitly authorised and validated.
6. Default to **monitoring-only** until safety and benchmarking requirements are met.
7. Never invent benchmark results, tests or functionality. Never kill critical Windows processes automatically.
8. No telemetry, ads, cloud dependencies or subscriptions. Keep supported architectures (x64, arm64).
9. Automated tests wherever possible. Stability, correctness and security over feature count.
10. Work on `feature/opensweep-core`; small commits; **no push to main and no releases without explicit approval.**
11. On macOS/Linux build portable parts only; never claim Windows service, ETW, IPC or scheduling work is tested unless it ran on Windows (GitHub Actions `windows-latest` counts as Windows).

## Architecture
Electron UI (standard user) → authenticated, ACL-restricted named-pipe IPC → minimal privileged broker →
`OpenSweepCore.exe` (C++ Windows service: monitoring, topology, ETW, counters, policy, stability analysis,
incidents, recovery, secure IPC, configuration) → documented APIs, ETW, Event Log, performance counters.
A kernel driver only with documented justification and explicit approval (see `driver-justification.md`).

IPC: named pipes with explicit restrictive security descriptors, client identity validation, protocol
versioning, typed command schemas, size limits, capability-based authorisation, timeouts, cancellation,
validation, safe concurrency. Never expose arbitrary PowerShell or unrestricted privileged operations.

## Engines
- **SweepMemory** — baselines, pressure score, sustained-growth/leak detection, trend analysis, foreground protection, conservative background memory policy (memory priority via SetProcessInformation), hysteresis, cooldowns, automatic restoration. Verify with hard faults, commit, responsiveness, launch latency, paging latency; roll back when worse.
- **SweepScheduler** — topology (groups, NUMA, hybrid cores, CCD/CCX where exposed), workload classes (LatencyCritical, Interactive, Throughput, Background, Efficiency, Unknown), contention detection, prefer EcoQoS → CPU Sets → temporary conservative priority; measure; revert. No aggressive affinity; never touch protected/security/anti-cheat/real-time workloads.
- **SweepTrace** — bounded local flight recorder (CPU, memory, commit, per-process, hard faults, disk latency, driver errors, crashes, service failures, shutdowns, updates, WHEA, responsiveness, OpenSweep changes); event-driven captures; incident reports with evidence and confidence; correlation ≠ causation. No keystrokes, document/browser contents, packets, passwords or window text. Local only unless exported.
- **SweepGuard** — memory growth, commit exhaustion, CPU runaway, handle/thread exhaustion, pool pressure, driver failures, GPU TDRs, WHEA, disk deterioration, latency spikes, service crash loops, hangs, boot degradation, unexpected shutdowns, thermal throttling, DPC/ISR anomalies. EWMA, median/MAD, robust scores, slope and change-point detection, multi-signal correlation, confidence. Each incident: symptoms, evidence, possible causes, confidence, actions, risk, whether auto-recovery is allowed.
- **SweepHeal** — transactional recovery (id, trigger, evidence, preconditions, snapshot, action, authorisation, verification, rollback, cooldown, max attempts, audit). Automatic only for low-risk OpenSweep-owned components; user approval for Explorer restart, third-party services, network reset, SFC/DISM, terminating apps. Circuit breaker. Never disable security, uninstall drivers or change boot config automatically.
- **SweepIO / SweepLatency** — queue depth, disk latency, contention, background I/O, DPC/ISR and driver latency, GPU scheduling symptoms, thermal throttling, responsiveness. Evidence-based driver attribution; no automatic driver/kernel changes.
- **Adaptive control loop** — observe → baseline → detect bottleneck → classify workload → estimate risk → minimal intervention → reversible policy → measure → keep or roll back. Hysteresis, cooldowns, rate limits, conflict detection, cost scoring, journaling, fail-safe defaults, simulation and monitoring-only modes. Must be able to decide nothing is needed; never optimise just because usage is high.

## Research (changed)
Use public Microsoft documentation, public symbols, WinDbg, Windows Performance Recorder/Analyzer and ETW
traces on Windows test machines. Record Windows build numbers; separate documented, observed and hypothesised
behaviour; store notes under `research/`. No reverse engineering of Windows kernel binaries.

## UI
Core dashboard: stability score, CPU, RAM, commit, storage latency, incidents, service health, active
policies, policy and recovery history, topology, measurements. Modes: **Monitor**, **Balanced** (approved
low-risk verified optimisations), **Advanced** (extra controls with warnings; never bypasses security).
Explain every intervention, instant disable/rollback, show whether Core is running, label simulated data.

## Benchmarks
Stock Windows vs monitoring-only vs active vs Process Lasso (where appropriate). Launch time, responsiveness,
CPU ready, context switches, hard faults, commit, storage latency, FPS, 1% lows, frame-time variance, idle CPU,
background RAM, crash frequency, recovery success. Median, p95/p99, confidence intervals, run counts, hardware,
Windows build, config. Targets (to validate): service RAM 15–25 MB, idle CPU < 0.1 %, no cloud, minimal writes,
no foreground regression.

## Security & testing
Full review: Electron privilege separation, pipe security, service permissions, IPC auth, validation, DLL
search paths, config validation, privilege escalation, supply chain, secure updates, signing, crash handling,
log privacy, races, memory safety. Unit (pressure, scheduling decisions, classification, anomaly scores,
hysteresis, cooldowns, rollback, journaling), integration (service lifecycle, IPC auth, concurrency, invalid
requests, telemetry, Electron, config migration), stress and security tests (fuzzing, oversized messages,
unauthorised clients, impersonation) — destructive tests only in disposable VMs.

## Phases
1. Foundation — audit, architecture, native service, secure IPC, service health in Electron, tests.
2. Observability — collectors, SweepTrace, baselines, live dashboard.
3. Stability intelligence — SweepGuard, incident reports, anomaly detection, simulation mode.
4. Resource intelligence — SweepMemory, SweepScheduler, topology, conservative recommendations.
5. Controlled optimisation — journaling, rollback, opt-in policies, verification windows.
6. Self-healing — SweepHeal, safe automatic recovery, approved actions, circuit breakers.
7. Validation — benchmarks, long-run stability, security review, resource tuning, compatibility.
8. Kernel feasibility — justification only; no driver without approval.

Each phase: inspect code, plan, implement, test, fix, security review, document, summarise. A phase is only
complete when its acceptance criteria are met.

## Acceptance (initial Core)
Existing features still work; service starts/stops reliably; Electron talks to Core securely; metrics are
accurate; storage is bounded; baselines work; anomalies produce evidence-backed incidents; memory and
scheduling policies run monitoring-only; simulation mode shows proposed interventions; journaling works; tests
pass; no kernel driver; no unsupported kernel changes; no unverified performance claims.
