# Migration plan: admin Electron → standard-user UI + OpenSweepCore service

Starting point: OpenSweep 0.3.2 — one Electron app running as administrator, PowerShell for every Windows operation
(`architecture-audit.md`). Target (Part 3 of `MASTER_PROMPT_CORE.md`):

```
OpenSweep UI (Electron, standard user)
  → named pipe \\.\pipe\OpenSweepCore.v1 (ACL'd, versioned, typed commands — ipc-protocol.md)
  → broker (command dispatcher inside the service: capability check, validation, journal)
  → OpenSweepCore.exe engines (monitoring, policy, recovery)
  → documented Windows APIs, ETW, Event Log, PDH
```

The "broker" is a layer inside `OpenSweepCore.exe`, not a separate process, unless a later review shows a separate
low-privilege process for monitoring is worth the cost. This keeps one binary to install, sign and update.

Rules for the whole migration:

- No feature is removed (Part 1 rule 2). A feature may move behind a different implementation; any user-visible change is
  listed in §8.
- The app must keep working with the service missing, stopped or broken (fallback to today's code paths) until stage D.
- Nothing is marked done until it ran on Windows (Part 18).

## 1. Stages

| Stage | Privilege of UI | Service | Part 17 phases |
|---|---|---|---|
| A — Harden in place | admin (unchanged) | none | Phase 1 (audit) |
| B — Read-only service alongside | admin (unchanged) | installed, `monitor` mode, `read` commands only | Phase 1 (service, IPC, health in UI), Phase 2 |
| C — Broker for privileged features | admin, but privileged features go through the service when it is available | `admin` capability commands added one feature at a time | Phases 3–6 |
| D — Standard-user UI | `asInvoker` | required for privileged features | after C covers every admin feature; Phase 7 validates |
| (Phase 8) | — | driver feasibility only, see `driver-justification.md` | Phase 8 |

### Stage A — Harden in place (no new components)

Small fixes from `security-audit.md` that do not depend on Core:

1. R1: create `%ProgramData%\OpenSweep` with an explicit DACL; check before use (S3).
2. R2: stop running elevated scripts from `%TEMP%` (S2).
3. R3: allowlist undo operations; schema-validate `settings.json` (S4).
4. R4: `killProcess` name lookup from PID; per-window IPC allowlist (S8, S10).
5. Absolute paths for system tools in scripts (S9).
6. Add unit tests for `main.js` logic that can run on Linux (validation, undo allowlist, settings migration).

### Stage B — Read-only service alongside the current app (Phase 1 → 2)

What moves first: **read-only monitoring**, because it needs no admin capability on the pipe and cannot change the system.

| Today | Service command (from `ipc-protocol.md` or planned) | UI change |
|---|---|---|
| — | `hello`, `health` | New "Core" status: running / stopped / not installed, version, uptime, own CPU and memory |
| `stats()` (`os.cpus()`, `os.freemem()`) | `metrics` | Dashboard and widget read `metrics` when connected; otherwise keep `stats()` |
| CPU model/cores from `systemInfo()` | `topology` | Hardware topology card |
| Hourly `stabilityCheck()` | Phase 2: Event Log subscriptions + bounded SweepTrace history | Reliability page shows service history when available; `runStability()` stays as fallback |
| `win-procs.js` PowerShell loop | Phase 2: process sampler in the service | Processes page uses service data when connected |

Node side: a small client module (planned name `src/core-client.js`) that connects, verifies the server process (see
`security-audit.md` §5), frames messages, applies timeouts and exposes `core.call(cmd, args)` to `main.js`. New IPC methods
in `api` (e.g. `coreStatus`, `coreMetrics`) are added the same way as the existing ones; the renderer never talks to the pipe.

Service mode is always `monitor` in this stage. The service must not require the UI to be open (Part 3).

### Stage C — Broker for privileged features (Phases 3–6)

Each privileged feature moves as a unit: a typed command, its validation, its verify-after-change step, its undo record.
Order, lowest risk and highest value first:

| Order | Feature | Current code | Broker command shape |
|---|---|---|---|
| 1 | Settings guard | `runGuard()` hourly poll | Service watches guarded values (`RegNotifyChangeKeyValue`), checks for policy ownership, journals restores. Tweak IDs only, resolved from a catalog compiled into or shipped with the service. |
| 2 | Tweaks (HKLM part) | `setTweaks` → `elevated(TWEAK_SET)` / `asSystem` | `tweaks.set { changes: [{ id, on }] }`; service reads back and returns per-ID result. `asSystem()` is removed. |
| 3 | Services, scheduled tasks | `setServices`, `setTasks` | `services.setStart { name ∈ catalog, type ∈ {Automatic, Manual, Disabled} }`, `tasks.set { id ∈ catalog, enable }` |
| 4 | Startup (HKLM, tasks) | `setStartup` | `startup.set { id, enabled }` where `id` is re-enumerated by the service, not taken from the UI's cache |
| 5 | Security, network, updates, power | `defenderAction`, `networkAction('dns')`, `updatesAction`, `setPowerPlan` | One command per action, same validation as today's `api` methods |
| 6 | Cleanup (admin categories), drivers, Windows.old, component cleanup | `cleanupRun({elevate})`, `removeDrivers`, `removeWindowsOld`, `componentCleanup` | Catalog IDs only; long-running jobs with progress events and cancellation |
| 7 | Uninstaller, leftovers, features, winget, install tracker | `win-uninstall.js` | Service re-reads uninstall entries itself; backups and tracks stored in the service's ACL'd directory |
| 8 | Repairs | `runRepair` (visible elevated console) | Fixed repair IDs; output streamed to the UI instead of a console window |
| 9 | SweepMemory / SweepScheduler policies | new | Phase 4 recommendations only; Phase 5 opt-in with journal and rollback |
| 10 | SweepHeal | new | Phase 6, with circuit breaker |

How existing PowerShell features keep working during C:

- The UI checks `hello` at start and on reconnect. For each feature, `main.js` uses the broker command if the service reports
  it (a `commands` list in `hello`, planned) and falls back to the current `backend.*` function otherwise.
- The PowerShell scripts may be reused by the service for a while, but only as **fixed files shipped in the signed install
  directory**, run by the service with arguments passed as data — never script text received over the pipe (Part 3).
  Native rewrites replace them feature by feature; the Windows smoke test is extended to call the broker path.
- `elevated()` keeps working for features not yet migrated. In stage C the app is still admin, so nothing changes for users.

### Stage D — UI as standard user

Preconditions: every feature that today calls `elevated()`, `asSystem()` or needs the admin token has a broker command,
or is explicitly documented as "asks for UAC each time" (e.g. opening the hosts file in an elevated Notepad, visible repair
consoles if kept).

Changes:

1. `package.json`: `requestedExecutionLevel: "asInvoker"`.
2. Start with Windows: `app.setLoginItemSettings({ openAtLogin })` (HKCU Run). On first start of the new version, remove the
   old "OpenSweep" logon task (`setAutoStart({ enabled: false })` needs admin → done by the installer or a broker command).
3. Scheduled clean: moves into the service scheduler; the "OpenSweep Auto Clean" task is removed by the installer.
4. `relaunchAsAdmin` is removed or kept only as a fallback when the service is unavailable.
5. User-level features (organiser, space analyzer, Recycle Bin, HKCU tweaks, per-user Appx removal, per-user startup)
   run directly in the UI with the user's token — this is also what makes their file operations safe (S7).

## 2. Service install, update, uninstall

Requirements: per-machine install (`nsis.perMachine: true`), service binary under `Program Files\OpenSweep\`, signed.

| Step | Mechanism |
|---|---|
| Ship binary | `extraResources` (x64 and arm64 builds; see `compatibility-matrix.md`) |
| Install | NSIS `customInstall` macro (electron-builder `nsis.include`, e.g. `build/installer.nsh`): run `OpenSweepCore.exe --install` (service registers itself with `CreateService`, sets description, failure actions, delayed auto-start, service SID type, required privileges) or `sc.exe` with absolute path. Create `%ProgramData%\OpenSweepCore` with an explicit DACL. Start the service. |
| Update (electron-updater) | The NSIS update runs the same installer. Before files are replaced: stop the service and wait for `SERVICE_STOPPED` (with timeout); after: start it. The UI must tolerate the pipe disappearing during update. Protocol version in `hello` lets a new UI detect an old service and the reverse. Per-machine updates need elevation; electron-updater handles this for per-machine NSIS installs (**verify** with a signed build). |
| Uninstall | NSIS `customUnInstall`: stop and delete the service; remove the old logon/auto-clean tasks; ask whether to keep `%ProgramData%\OpenSweepCore` (history, journal) — default remove. Anything the service changed in Windows stays as the user set it; the journal is exported to the user's Documents on request before removal. |
| Repair | `OpenSweepCore.exe --install` is idempotent; the UI shows "service not installed" with a button that runs the installer's repair path (UAC prompt). |

Per-user installs (possible today, see S5) cannot host a service safely. The first version that installs the service must
detect a per-user install and migrate: install per-machine, carry over `settings.json`, remove the per-user copy
(**verify** how electron-builder handles a change of install mode on update; it may need a one-time full installer download
instead of a delta update).

## 3. Configuration migration

| Data | Today | After |
|---|---|---|
| UI preferences (theme, tray, widget, shortcuts, confirmActions, compactNav, scanRoot) | `%APPDATA%\OpenSweep\settings.json` | stays there (per-user, owned by UI) |
| `history` (with `undo`) | `settings.json` | UI history stays for user-level actions; privileged actions are journaled by the service in `%ProgramData%\OpenSweepCore` and shown merged in History |
| `guardTweaks`, `settingsGuard` | `settings.json` | machine-wide HKLM tweaks move to service config; HKCU tweaks can stay with the UI |
| `stabilityMonitor`, `stabilityNotify`, `stabilitySeen` | `settings.json` | service config + SweepTrace; notifications still shown by the UI |
| `autoClean` | `settings.json` + scheduled task | service config + service scheduler |
| `autoUpdate`, `launchAtLogin` | `settings.json` | unchanged |
| Leftover backups, install tracks | `%ProgramData%\OpenSweep\backups`, `tracks` | moved to `%ProgramData%\OpenSweepCore\...` by the installer after a DACL check; files that fail validation are not imported |

Mechanics: add `schemaVersion` to `settings.json`; on first start of a new version, back up the file to
`settings.json.bak-<version>`, migrate, validate with the schema, write. Unknown keys are kept (forward compatibility for
downgrades) but ignored. Service config has its own `schemaVersion` and is only written by the service.

## 4. Rollback

| Failure | Response |
|---|---|
| Service will not start or crashes repeatedly | Service recovery actions restart it a limited number of times; then the UI shows "Core stopped" and uses the PowerShell fallbacks (stages B–C). |
| A broker command misbehaves | Per-feature switch in UI settings ("Use Core for X"), default on only after the feature passed the Windows smoke test. Turning it off returns to the old path in stages B–C. |
| Bad release | Users install the previous GitHub release over it; the installer handles service stop/start in both directions. `hello.protocol` lets an older UI refuse a newer service gracefully. Settings backups (`settings.json.bak-*`) allow reverting the config. |
| A policy change makes things worse (Phase 5+) | Service journal rollback (Part 8/10), automatic after the verification window fails. |
| Stage D regression | Ship an update that sets `requireAdministrator` again; this is a manifest change only. |

Each stage ends in a release that can be rolled back to the previous stage's release without data loss.

## 5. Tests per stage

| Stage | Tests |
|---|---|
| A | New Node unit tests (Linux CI) for validation/undo/settings migration; existing `windows-smoke.js` |
| B | Service unit tests (portable parts on Linux); Windows CI job: build service, install, `hello`/`health`/`metrics`/`topology`, stop, uninstall; pipe security tests from `security-audit.md` §7 |
| C | Smoke test extended to call each migrated feature through the broker and through the fallback |
| D | Install as admin, run UI as a standard user account on a VM, exercise every page |

## 6. Effort and ordering notes

- Stage A is independent and should ship first; it closes the high-severity file-location issues without Core.
- Code signing (S6) is a prerequisite for stage B: an unsigned binary should not be installed as a LocalSystem service.
- arm64: the release workflow builds only x64 today. The service needs native x64 and arm64 builds before stage B ships to
  arm64 users (`compatibility-matrix.md`).

## 7. Status

See the phase table in `README.md` in this folder.

## 8. Breaking changes (user-visible)

| Change | Stage | Why |
|---|---|---|
| Boost no longer calls `EmptyWorkingSet` on all processes; it shows a memory diagnosis and offers targeted actions (temp clean and DNS flush remain available separately) | B or C | Part 1 rule 11, Part 4 |
| Installer becomes per-machine; per-user installs are migrated | B | Service hosting, S5 |
| A Windows service "OpenSweepCore" is installed and runs while the UI is closed | B | Part 3 |
| Undo entries created by older versions with unknown `fn` values are shown but cannot be replayed | A | S4 |
| `asSystem()` fallback removed: HKLM settings that Windows locks against administrators report "Windows protects this setting" instead of being forced | C | Part 1 rule 9, Part 15 |
| UI no longer asks for UAC at start; some rarely used actions may prompt individually | D | S1 |
| "Start with Windows" uses the normal startup list; the "OpenSweep" logon task is removed | D | S5 |
| Scheduled clean runs from the service; the "OpenSweep Auto Clean" task is removed | D | S5 |
| Auto-update no longer installs pre-releases by default and asks before a silent install | A–B | S6 |
| Minimum supported OS and architecture list as in `compatibility-matrix.md` | B | API availability |
