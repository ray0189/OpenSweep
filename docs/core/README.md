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

## Phase status

"Done" means the phase's acceptance criteria are met and the Windows-specific parts have run on Windows (Part 18).
Nothing is marked done yet.

| Phase | Scope (Part 17) | Status |
|---|---|---|
| 1 — Foundation | Audit, architecture, native service, secure IPC, service health in Electron, tests | **In progress.** Audit documents and IPC spec written. Service, Node client and UI integration are being added on `feature/opensweep-core`; not yet verified on Windows. |
| 2 — Observability | Telemetry collectors, SweepTrace, baselines, live dashboard | Not started |
| 3 — Stability intelligence | SweepGuard, incident reports, anomaly detection, simulation mode | Not started |
| 4 — Resource intelligence | SweepMemory, SweepScheduler, topology, recommendations | Not started |
| 5 — Controlled optimisation | Journaling, rollback, opt-in policies, verification windows | Not started |
| 6 — Self-healing | SweepHeal, safe auto-recovery, approvals, circuit breakers | Not started |
| 7 — Validation | Benchmarks, long-run stability, security review, compatibility | Not started |
| 8 — Kernel feasibility | Driver investigation only; no implementation without approval | Not started (initial position in `driver-justification.md`) |

## Key open items from the audits

- High-severity findings in `security-audit.md` (S1–S6) are not fixed yet; Stage A of `migration-plan.md` lists the fixes
  that do not need Core.
- The current Boost feature trims all working sets (`EmptyWorkingSet`) and must be replaced (Part 1 rule 11).
- Builds are unsigned; code signing is a prerequisite for installing a service.
- Release builds are x64 only; arm64 needs native builds of both the app and the service.
