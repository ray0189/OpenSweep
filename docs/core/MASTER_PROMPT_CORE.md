# MASTER IMPLEMENTATION PROMPT — OPENSWEEP CORE

## ROLE AND MISSION

You are the principal engineering team responsible for developing OpenSweep Core, a next-generation Windows stability, diagnostics, resource management and autonomous optimisation platform.

Act simultaneously as:

* Principal Windows NT systems architect
* Senior Windows kernel and driver engineer
* Expert C++20/23 developer
* Windows performance and ETW specialist
* CPU scheduling and memory management researcher
* Reverse-engineering specialist
* Security architect
* Site reliability engineer
* Quality assurance and benchmarking engineer
* Senior Electron application developer

You have access to Claude Code, the local development environment and potentially REA reverse-engineering tooling.

Your mission is to transform the existing OpenSweep application into an advanced, production-quality Windows system management platform.

Repository:
https://github.com/ray0189/OpenSweep

Project name: OpenSweep Core

Primary objective: Build an intelligent Windows resource management and stability engine that observes system behaviour, detects developing problems, applies carefully controlled optimisations, measures their effectiveness and automatically reverts harmful changes.

This is a real implementation project, not a theoretical exercise.

---

## PART 1 — NON-NEGOTIABLE ENGINEERING REQUIREMENTS

1. Inspect the entire existing repository before modifying anything.
2. Preserve existing OpenSweep features and user configurations.
3. Do not rewrite the existing application unnecessarily.
4. Do not introduce breaking changes without documenting them.
5. Use native C++20/23 for the performance-critical background service.
6. Retain the existing Electron interface initially.
7. Use documented Windows APIs wherever possible.
8. Never rely on undocumented Windows kernel offsets in production.
9. Never disable PatchGuard, Secure Boot, HVCI or Windows security protections.
10. Do not attempt to replace the Windows kernel scheduler or memory manager.
11. Do not implement fake RAM boosters or unnecessary cache clearing.
12. Every active optimisation must be measurable, reversible where possible and subject to safety checks.
13. Every privileged operation must be explicitly authorised and validated.
14. Default to monitoring-only mode until safety and benchmarking requirements are satisfied.
15. Do not invent benchmark results, completed tests or implemented functionality.
16. Never automatically terminate critical Windows processes.
17. Do not introduce unnecessary telemetry, advertising, cloud dependencies or subscriptions.
18. Maintain compatibility with the project's supported Windows architectures.
19. Use automated tests wherever possible.
20. Prioritise stability, correctness and security over feature quantity.

Do not sacrifice system reliability to achieve impressive-looking performance statistics.

---

## PART 2 — REPOSITORY AUDIT

Before implementation:

Clone or inspect the current repository.

Examine:

* package.json
* src/main.js
* src/backend/catalog.js
* src/backend/windows.js
* src/backend/mock.js
* Electron security configuration
* Existing IPC handlers
* Existing PowerShell execution paths
* Current Boost functionality
* Settings Guard
* Startup optimisation
* Process management
* Existing stability monitoring
* Application packaging
* Automatic updates
* GitHub Actions workflows
* Test infrastructure

Identify architectural weaknesses, security risks, duplicated functionality and opportunities for native integration.

Particular attention must be given to the current administrator elevation model.

Produce:

* docs/core/architecture-audit.md
* docs/core/security-audit.md
* docs/core/migration-plan.md
* docs/core/compatibility-matrix.md

Do not remove existing features.

---

## PART 3 — TARGET ARCHITECTURE

Implement the following architecture:

OpenSweep UI (Electron, standard user privileges)

↓

Authenticated, ACL-restricted IPC

↓

OpenSweep Broker (minimal privileged command interface)

↓

OpenSweepCore.exe (native C++ Windows service)

↓

Modular resource management and stability engines

↓

Documented Windows APIs, ETW, Event Log and performance counters

↓

Windows NT Kernel

An optional future kernel-mode driver may be considered only when a documented technical justification proves that user-mode interfaces are insufficient.

### Required components

#### OpenSweepCore.exe

Persistent Windows service responsible for:

* Resource monitoring
* Hardware topology detection
* ETW event processing
* Performance counters
* Policy evaluation
* System stability analysis
* Incident recording
* Recovery coordination
* Secure IPC
* Configuration management

Implement clean service start, stop, pause where appropriate, recovery and shutdown behaviour.

The service must not require the Electron interface to remain open.

Use event-driven monitoring wherever practical.

#### Secure IPC

Implement:

* Windows named pipes
* Explicit restrictive security descriptors
* Client identity validation
* Protocol versioning
* Typed command schemas
* Request size limits
* Capability-based authorisation
* Timeouts
* Cancellation
* Input validation
* Safe concurrency

Never expose arbitrary PowerShell execution or unrestricted privileged operations through IPC.

Design the privileged broker with the smallest possible attack surface.

---

## PART 4 — SWEEPMEMORY

Develop an adaptive memory management subsystem.

### Objectives

Improve system responsiveness under genuine memory pressure while preserving useful Windows caching behaviour.

### Required monitoring

Collect:

* Total physical memory
* Available memory
* Commit charge
* Commit limit
* Per-process private bytes
* Working-set usage
* Hard page faults
* Memory compression indicators where supported
* Paging-related storage latency
* Process memory growth
* Kernel pool indicators
* Foreground workload sensitivity

### Algorithms

Implement:

1. Machine-specific memory baselines.
2. Memory pressure scoring.
3. Sustained memory growth detection.
4. Suspected memory leak classification.
5. Memory pressure trend analysis.
6. Foreground workload protection.
7. Conservative background memory policy selection.
8. Intervention hysteresis.
9. Cooldown periods.
10. Automatic policy restoration.

Investigate supported APIs including:

* GetPerformanceInfo
* GlobalMemoryStatusEx
* GetProcessMemoryInfo
* SetProcessInformation
* QueryWorkingSetEx
* Relevant performance counters

Validate API support and semantics against Microsoft documentation.

### Behaviour

When memory pressure is low:

Do nothing.

When pressure increases:

Identify the cause and determine whether intervention is justified.

When pressure becomes significant:

Prefer supported memory-priority and background workload policies.

When pressure becomes critical:

Generate a diagnostic incident and recommend appropriate recovery.

Never automatically kill a foreground application solely because it consumes large amounts of memory.

Do not implement periodic EmptyWorkingSet sweeps or routine standby-list purging.

### Verification

Measure:

* Hard page faults
* Commit pressure
* Foreground responsiveness
* Application launch latency
* Storage paging latency

Rollback an intervention when measurable performance worsens or the intended improvement fails to materialise.

---

## PART 5 — SWEEPSCHEDULER

Develop an intelligent CPU scheduling policy engine.

This engine must influence Windows scheduling through supported APIs rather than replace the NT scheduler.

### Required capabilities

* CPU topology detection
* Logical processor detection
* Processor group awareness
* NUMA detection
* Intel hybrid architecture detection
* AMD CCD/CCX topology awareness where reliably exposed
* Foreground workload classification
* Background workload classification
* CPU contention detection
* Runnable-thread pressure analysis
* CPU scheduling latency measurement
* Power efficiency policy management
* Process priority management
* CPU Set policy management

### Workload classifications

Implement:

* LatencyCritical
* Interactive
* Throughput
* Background
* Efficiency
* Unknown

### Scheduling policy

Prioritise the following order:

1. Observe without intervention.
2. Detect sustained contention.
3. Identify verified background workloads.
4. Prefer EcoQoS where appropriate.
5. Consider CPU Sets.
6. Consider temporary conservative priority changes.
7. Measure the outcome.
8. Revert automatically if necessary.

Avoid aggressive processor affinity changes.

Do not assume a particular CCD or processor core is always optimal.

### Relevant APIs

Research and use documented interfaces where appropriate:

* GetLogicalProcessorInformationEx
* GetSystemCpuSetInformation
* SetProcessDefaultCpuSets
* SetThreadSelectedCpuSets
* SetPriorityClass
* SetThreadPriority
* SetProcessInformation
* SetThreadInformation
* GetNumaNodeProcessorMaskEx

Account for API availability and permissions.

### Critical protection

Never modify protected system processes, security software or unknown critical workloads automatically.

Avoid interfering with real-time audio, video playback, gaming anti-cheat software or latency-critical applications.

---

## PART 6 — SWEEPTRACE

Implement a lightweight system flight recorder.

The system should maintain bounded local diagnostic history.

### Required information

* CPU utilisation
* Memory usage
* Commit pressure
* Process resource consumption
* Hard page faults
* Disk latency
* Driver-related errors
* Application crashes
* Service failures
* Unexpected shutdowns
* Windows Update events
* Hardware errors
* System responsiveness
* OpenSweep configuration changes

### Architecture

Use:

* Low-frequency continuous monitoring
* Bounded circular storage
* Event-driven detailed captures
* ETW where appropriate
* Windows Event Log
* Performance counters
* Crash-resilient persistence

### Incident recording

When an anomaly occurs:

1. Preserve recent diagnostic history.
2. Collect additional bounded telemetry where safe.
3. Correlate related events.
4. Identify potential contributors.
5. Generate a readable incident report.
6. Display evidence and confidence levels.

Never claim causation when only correlation is established.

### Privacy

Do not capture:

* Keystrokes
* Document contents
* Browser page contents
* Network packet payloads
* Passwords
* Sensitive window text

All diagnostics must remain local unless the user explicitly exports them.

---

## PART 7 — SWEEPGUARD

Build a predictive system stability engine.

### Detection targets

* Sustained memory growth
* Commit exhaustion
* CPU runaway workloads
* Handle exhaustion
* Thread exhaustion
* Kernel pool pressure
* Driver failures
* GPU timeouts
* WHEA errors
* Disk deterioration
* Storage latency spikes
* Service crash loops
* Application hangs
* Boot degradation
* Unexpected shutdowns
* Thermal throttling
* DPC/ISR latency anomalies

### Detection techniques

Implement lightweight statistical methods first:

* Exponentially weighted moving averages
* Median and median absolute deviation
* Robust anomaly scores
* Sustained slope detection
* Change-point detection
* Multi-signal correlation
* Confidence scoring

Do not introduce large AI models without demonstrating that they outperform simpler methods.

### Output

Every incident should include:

* Observed symptoms
* Supporting evidence
* Possible causes
* Confidence level
* Recommended actions
* Risk level
* Whether automatic recovery is permitted

---

## PART 8 — SWEEPHEAL

Implement a transactional self-healing system.

### Recovery architecture

Each recovery operation must include:

* Unique transaction identifier
* Trigger
* Evidence
* Preconditions
* Original state snapshot
* Proposed action
* Required authorisation
* Verification procedure
* Rollback procedure
* Cooldown
* Maximum attempts
* Audit log

### Automatic recovery

Initially permit automatic recovery only for low-risk OpenSweep-owned components.

Examples:

* Restart failed OpenSweep workers.
* Reconnect failed OpenSweep telemetry consumers.
* Clear bounded OpenSweep internal caches.
* Restore OpenSweep-owned temporary policies.

Require user approval for higher-impact operations.

Examples:

* Restarting Explorer
* Restarting third-party services
* Resetting network settings
* Running SFC or DISM
* Terminating applications

Never automatically disable Windows security protections, uninstall drivers or alter boot configuration.

### Circuit breaker

Implement protection against repeated failed recoveries.

After repeated failures, suspend automatic recovery and notify the user.

---

## PART 9 — SWEEPIO AND SWEEPLATENCY

Develop diagnostics for:

* Storage queue depth
* Disk latency
* Read/write contention
* Excessive background I/O
* DPC latency
* ISR latency
* Driver-associated latency
* GPU scheduling symptoms
* Thermal throttling
* System responsiveness

Use ETW and documented Windows interfaces.

Identify problematic drivers through measured evidence rather than arbitrary assumptions.

Avoid automatically changing storage drivers, GPU drivers or kernel configurations.

---

## PART 10 — ADAPTIVE CONTROL ENGINE

Create a unified policy controller.

The optimisation loop must be:

OBSERVE

↓

ESTABLISH BASELINE

↓

DETECT BOTTLENECK

↓

CLASSIFY WORKLOAD

↓

ESTIMATE RISK

↓

SELECT MINIMAL INTERVENTION

↓

APPLY REVERSIBLE POLICY

↓

MEASURE RESULT

↓

RETAIN TEMPORARILY OR ROLLBACK

### Requirements

Implement:

* Machine-specific baselines
* Workload-specific policies
* Hysteresis
* Cooldowns
* Rate limiting
* Policy conflict detection
* Intervention cost scoring
* Automatic rollback
* Persistent transaction journaling
* Fail-safe defaults
* Simulation mode
* Monitoring-only mode

The engine must be capable of deciding that no optimisation is necessary.

Never optimise simply because CPU or RAM usage is high.

---

## PART 11 — REA REVERSE-ENGINEERING WORKFLOW

If REA is available, use it to research Windows internal behaviour in controlled environments.

Research:

* ntoskrnl.exe
* ntdll.dll
* hal.dll
* dxgkrnl.sys
* Relevant storage drivers
* Relevant network drivers

Combine findings with:

* WinDbg
* Microsoft public symbols
* Windows Performance Recorder
* Windows Performance Analyzer
* ETW traces
* Microsoft documentation

### Rules

Record the exact Windows build and binary versions.

Separate:

* Documented behaviour
* Observed behaviour
* Reverse-engineered hypotheses
* Unsupported assumptions

Never incorporate undocumented kernel offsets or private structures into production code.

Never attempt to bypass Windows kernel protections.

All reverse-engineering output must be documented under:

research/rea/

If REA is unavailable, continue with documented tools and record the limitation.

> **Note (added with the owner):** Microsoft's licence terms restrict reverse engineering of Windows
> binaries. Unless legal clearance is confirmed, this part is carried out with the public tools listed
> above (WinDbg with public symbols, WPR/WPA, ETW traces, Microsoft documentation) only, and that
> limitation is recorded under `research/rea/`.

---

## PART 12 — OPTIONAL KERNEL DRIVER

Do not implement a kernel driver during the initial development phases.

Create:

docs/core/driver-justification.md

Investigate whether a future KMDF driver would provide measurable benefits unavailable through user-mode APIs.

Document:

* Exact required capability
* Why ETW is insufficient
* Relevant documented kernel API
* Security implications
* Driver signing requirements
* HVCI compatibility
* Secure Boot compatibility
* Driver Verifier testing
* Fault injection
* Fuzz testing
* Rollback and uninstall
* Maintenance cost

Only propose implementation after the evidence supports it and explicit project approval is given.

The future driver must not expose arbitrary kernel memory access or unrestricted privileged operations.

---

## PART 13 — ELECTRON UI INTEGRATION

Integrate OpenSweep Core into the existing interface.

Create a dedicated Core dashboard showing:

* System stability score
* CPU utilisation
* RAM utilisation
* Commit pressure
* Storage latency
* Recent incidents
* Background service health
* Active optimisation policies
* Policy history
* Recovery history
* Hardware topology
* Performance measurements

### Modes

Implement three operating modes:

#### Monitor

Collect telemetry and generate recommendations without changing Windows policies.

#### Balanced

Apply only approved, low-risk and verified optimisations.

#### Advanced

Expose additional controls and experiments with explicit warnings and user consent.

Advanced mode must not bypass Windows security protections.

### Interface requirements

* Match existing OpenSweep design language.
* Avoid unnecessary visual clutter.
* Show measured outcomes rather than exaggerated claims.
* Provide explanations for every intervention.
* Provide immediate policy disable and rollback controls.
* Show whether the Core service is running.
* Clearly distinguish simulated data from live system data.

Do not claim a performance improvement without supporting measurements.

---

## PART 14 — PERFORMANCE AND BENCHMARKING

Create a reproducible benchmarking framework.

Compare:

1. Stock Windows.
2. OpenSweep monitoring-only.
3. OpenSweep active optimisation.
4. Process Lasso where legally and technically appropriate.

### Metrics

* Application launch time
* Foreground responsiveness
* CPU ready time
* Context-switch overhead
* Hard page faults
* Commit pressure
* Storage latency
* Average gaming FPS
* Gaming 1% lows
* Frame-time variance
* Idle CPU overhead
* Background RAM usage
* Crash frequency
* Recovery success rate

### Methodology

Use repeatable workloads, multiple runs, controlled environments and statistical reporting.

Report:

* Median
* p95/p99
* Confidence intervals where appropriate
* Run count
* Hardware details
* Windows build
* Benchmark configuration

Never fabricate benchmark results.

### Initial targets

* Native service RAM: 15–25 MB.
* Idle CPU: below 0.1% average.
* No mandatory cloud dependency.
* No external telemetry.
* Minimal persistent disk writes.
* No measurable foreground responsiveness regression.

These are aspirational targets and must be validated.

---

## PART 15 — SECURITY HARDENING

Conduct a full security review of the existing and new architecture.

Focus on:

* Electron privilege separation
* Secure named pipes
* Windows service permissions
* IPC authentication
* Command validation
* DLL search path security
* Configuration validation
* Privilege escalation prevention
* Supply-chain security
* Secure updates
* Code signing
* Crash handling
* Logging privacy
* Race conditions
* Memory safety

Use least privilege throughout.

No renderer-controlled arbitrary shell execution.

No unnecessary SYSTEM privileges.

Any high-risk change must require explicit user approval.

---

## PART 16 — AUTOMATED TESTING

Create comprehensive tests.

### Unit tests

* Memory pressure calculations
* Scheduling policy decisions
* Workload classification
* Anomaly scoring
* Hysteresis
* Cooldowns
* Rollback logic
* Transaction journaling

### Integration tests

* Service lifecycle
* IPC authentication
* Concurrent requests
* Invalid requests
* Telemetry collection
* Electron integration
* Configuration migration

### Stress tests

* Low-memory conditions
* CPU saturation
* Disk saturation
* Synthetic memory leaks
* Service crash loops
* Sleep and resume
* System restart
* Abrupt service termination

### Security tests

* IPC fuzzing
* Oversized messages
* Unauthorised clients
* Malformed configurations
* Privilege escalation attempts
* Service impersonation scenarios

Run destructive and fault-injection tests only in disposable virtual machines.

---

## PART 17 — IMPLEMENTATION PHASES

Work incrementally.

### Phase 1 — Foundation

* Audit repository.
* Establish architecture.
* Create native C++ service.
* Implement secure IPC.
* Integrate service health into Electron.
* Add tests.

### Phase 2 — Observability

* Implement telemetry collectors.
* Implement SweepTrace.
* Implement machine-specific baselines.
* Add live dashboard.

### Phase 3 — Stability intelligence

* Implement SweepGuard.
* Implement incident reports.
* Implement statistical anomaly detection.
* Implement simulation mode.

### Phase 4 — Resource intelligence

* Implement SweepMemory.
* Implement SweepScheduler.
* Add hardware topology detection.
* Implement conservative policy recommendations.

### Phase 5 — Controlled optimisation

* Implement transaction journaling.
* Implement rollback.
* Add opt-in active policies.
* Implement verification windows.

### Phase 6 — Self-healing

* Implement SweepHeal.
* Add safe automatic recovery.
* Add user-approved recovery actions.
* Add circuit breakers.

### Phase 7 — Validation

* Benchmark against stock Windows.
* Perform long-duration stability testing.
* Conduct security review.
* Optimise service resource usage.
* Validate Windows compatibility.

### Phase 8 — Kernel feasibility

* Investigate optional driver requirements.
* Produce technical justification.
* Do not implement without approval.

---

## PART 18 — CLAUDE CODE EXECUTION INSTRUCTIONS

You must work directly in the repository.

Do not stop after writing an architecture proposal.

For every phase:

1. Inspect the relevant existing source code.
2. Identify dependencies and integration points.
3. Create a concrete implementation plan.
4. Implement the required functionality.
5. Add unit and integration tests.
6. Run available builds and tests.
7. Fix errors.
8. Review security implications.
9. Update documentation.
10. Summarise completed changes and remaining limitations.

Do not mark a phase complete unless its acceptance criteria are satisfied.

If the development environment is macOS or Linux, implement portable algorithmic components there, but clearly distinguish them from Windows-specific functionality that cannot be validated without a Windows test machine.

Do not claim Windows service, ETW, native IPC or scheduling integration is tested unless it has actually run successfully on Windows.

Never silently skip tests.

Never fabricate results.

If a phase is blocked, document the blocker and continue only with independent, safe work.

Do not make unrelated changes.

Do not push to the main branch or publish releases without explicit approval.

Create a dedicated development branch:

feature/opensweep-core

Use small, reviewable commits with meaningful messages.

---

## PART 19 — ACCEPTANCE CRITERIA

The initial OpenSweep Core implementation is successful only when:

* Existing OpenSweep functionality remains operational.
* The native Windows service starts and stops reliably.
* Electron communicates with Core securely.
* System metrics are collected accurately.
* Local telemetry storage is bounded.
* Machine-specific baselines work.
* Anomalies generate evidence-backed incidents.
* Memory and scheduling policies operate in monitoring-only mode.
* Simulation mode accurately displays proposed interventions.
* Configuration and recovery journaling function correctly.
* Tests pass in supported environments.
* No custom kernel driver is required.
* No unsupported kernel modifications are introduced.
* No unverified performance claims are presented.

Active optimisation is a later milestone requiring additional validation.

---

## FINAL INSTRUCTION

Begin by inspecting the existing OpenSweep repository and its architecture.

Then implement Phase 1.

Continue through subsequent phases only when the previous phase meets its acceptance criteria and the required environment is available.

Prioritise a fully working, secure and testable foundation over generating large amounts of incomplete code.

Your objective is not to create another Windows tweaking utility.

Your objective is to build OpenSweep Core: an evidence-driven, adaptive Windows stability and resource management platform that can diagnose problems, select safe interventions, verify their effectiveness and restore the original system state when necessary.

Start now.
