# OpenSweep Core documentation

OpenSweep Core is the planned native Windows service (`OpenSweepCore.exe`) and secure IPC layer behind the existing
Electron app. The full plan is in `MASTER_PROMPT_CORE.md`.

## Documents

| File | Contents |
|---|---|
| [MASTER_PROMPT_CORE.md](MASTER_PROMPT_CORE.md) | The owner's full plan (Parts 1–19). |
| [ipc-protocol.md](ipc-protocol.md) | Named-pipe protocol v1: transport, ACL, framing, commands. Shared by the service, the Node client and tests. |
| [architecture-audit.md](architecture-audit.md) | How OpenSweep 0.3.2 works today: Electron layout, IPC, PowerShell model, features (incl. Boost, Settings guard, stability monitor), updater, packaging, CI; weaknesses and native opportunities. |
| [security-audit.md](security-audit.md) | Threat model, findings with severity, review of the Core IPC design, recommendations mapped to Core. |
| [migration-plan.md](migration-plan.md) | Stages A–D from the admin Electron app to a standard-user UI + service; install/update/uninstall, config migration, rollback, breaking changes. |
| [compatibility-matrix.md](compatibility-matrix.md) | Windows 10 22H2, Windows 11 23H2/24H2/25H2 on x64/arm64; minimum versions of the planned APIs. Items marked "verify" are unconfirmed. |
| [driver-justification.md](driver-justification.md) | Part 12: why no kernel driver is justified now and what one would cost. |
| [phases-2-8.md](phases-2-8.md) | Contract for Phases 2–8: modules, engine loop, safety rules, storage, IPC additions. |
| [security-review-phase7.md](security-review-phase7.md) | Part 15 review of the new architecture, with open items. |
| [user-guide.md](user-guide.md) | Plain-language guide to Core and its modes. |
| [../../bench/README.md](../../bench/README.md) | Benchmark methodology and harness (no results yet). |
| [../../research/rea/README.md](../../research/rea/README.md) | Part 11 research notes (public tools only). |

## Phase status

"Done" means the phase's acceptance criteria are met and the Windows-specific parts have run on Windows (Part 18).
"CI-verified" = ran green on the GitHub `windows-latest` runner (`core.yml`, run 13, commit aed88c8), which is a
Windows Server VM, not a real desktop PC. Nothing is marked done until it has also been checked on a real Windows 10/11 PC.

| Phase | Scope (Part 17) | Status |
|---|---|---|
| 1 — Foundation | Audit, architecture, native service, secure IPC, service health in Electron, tests | **CI-verified.** MSVC build; service install/start/stop/uninstall as LocalSystem; pipe ACL matches spec; all protocol error paths, limits, 8-client queueing, 1000 concurrent requests, 5 s stall cut-off; admin commands refused for a de-elevated client. Not tested: 60 s idle timeout, reportForeground from a second session. |
| 2 — Observability | Collectors, SweepTrace, baselines, live dashboard | **CI-verified (collectors, trace, commands); dashboard mock-only.** 12 PDH counters, per-process top 40, event-log backfill + live subscription, history for 30 m/24 h/14 d, baselines (need ~2 h to become ready, so only their shape is tested on CI). Dashboard clicked through in demo mode only. Deferred: ETW (per-driver DPC/ISR), audio detection, memory-compression size. |
| 3 — Stability intelligence | SweepGuard, incidents, anomaly detection, simulation | **Portable logic unit-tested; CI saw a real incident** (driver failure from the runner's event log). Leak/runaway/latency detectors tested only on synthetic data. |
| 4 — Resource intelligence | SweepMemory, SweepScheduler, topology, recommendations | **Portable logic unit-tested; topology + CPU sets CI-verified.** No real contention scenario has been run on Windows; no recommendation was produced on the idle runner. |
| 5 — Controlled optimisation | Journaling, rollback, opt-in policies, verification windows | **Unit-tested; actuator CI-verified on a child process** (EcoQoS, memory priority, BELOW_NORMAL apply → read back → restore; PID-reuse and protection refusals). Monitor → Balanced → Monitor with zero applied policies verified. An end-to-end Balanced intervention on Windows has **not** been run. Active optimisation stays opt-in and is a later milestone (Part 19). |
| 6 — Self-healing | SweepHeal, auto-recovery, approvals, circuit breakers | **Unit-tested.** Approve/unknown-id paths CI-verified; real SFC/DISM/netsh/service-restart actions have **not** been run (they need a disposable VM). |
| 7 — Validation | Benchmarks, long-run stability, security review, compatibility | **Partly.** 60 s service soak on CI: 17.4 MB working set, 0 % growth, ~0.07 ms per tick. Benchmark harness (`bench/`) written but **no benchmark results exist**. Security review written (`security-review-phase7.md`). No long-duration, arm64 or Windows 10 testing yet. |
| 8 — Kernel feasibility | Driver investigation only | Done as documentation (`driver-justification.md`): no driver is justified; none implemented. |

## Key open items from the audits

- S2, S3 and S4 from `security-audit.md` are fixed on `feature/opensweep-core` (Windows smoke test passes). S1 (whole app
  runs as admin), S5 (elevated scheduled tasks) and S6 (unsigned builds + silent auto-update) are still open.
- The current Boost feature trims all working sets (`EmptyWorkingSet`) and must be replaced (Part 1 rule 11).
- Builds are unsigned; code signing is a prerequisite for installing a service.
- Release builds are x64 only; arm64 needs native builds of both the app and the service.
