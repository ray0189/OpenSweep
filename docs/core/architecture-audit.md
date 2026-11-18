# OpenSweep architecture audit

Scope: the baseline application as committed at `94c5eea` on `feature/opensweep-core` (app version 0.3.2), i.e.
before any Phase 1 Core code. Phase 1 changes being added on the branch (native service under `core/`,
`src/core-client.js`, new workflow and packaging steps) are not part of this audit.
This is the Part 2 audit from `docs/core/MASTER_PROMPT_CORE.md`. It describes what exists. It does not
describe the planned Core service except where stated.

Nothing in this document was run on Windows for this audit. Statements about runtime behaviour come
from reading the code.

## 1. Summary

OpenSweep is a single Electron process tree (main + sandboxed renderers) that runs **as administrator
for its whole life** (`"requestedExecutionLevel": "requireAdministrator"` in `package.json`). Every Windows
operation is a PowerShell 5.1 script, written to a temp `.ps1` file and executed with `powershell.exe -File`.
There is no native code, no service and no background component that runs when the UI process is closed,
apart from two scheduled tasks the app registers (start at logon, scheduled clean), which also launch the
full Electron app elevated.

Main weaknesses:

1. One privilege level for everything. The UI, the HTML renderer host, the updater and every feature run
   elevated. See `security-audit.md`.
2. PowerShell as the system interface: ~0.5–2 s process start per call, heavy CIM queries, polling loops,
   text parsing, and script files in user-writable folders.
3. Monitoring only exists while the app runs, and it is polling (hourly stability check, 15-minute tick,
   1.8 s process loop).
4. The Boost feature trims every process's working set (`EmptyWorkingSet`), which the Core plan
   (Part 1 rule 11, Part 4) explicitly rejects.
5. Little automated testing: a syntax check and a Windows smoke test of the backend; no unit tests of the
   main-process logic and no renderer tests.

## 2. Process and module layout

| Piece | File | Role |
|---|---|---|
| Main process | `src/main.js` | Settings, health score, stability monitor, settings guard, auto-clean, updater, tray, widget, IPC `api` object |
| Extra IPC methods | `src/api-extra.js` | Clean everything, uninstaller, space analyzer, reliability, live processes. Merged into `api` in `main.js` |
| Preload | `src/preload.js` | `contextBridge` exposes `window.api.call(method, args)` and `window.api.on(cb)` only |
| Renderer | `src/renderer/index.html`, `app.js`, `pages.js`, `icons.js` | UI. Builds DOM with `h()` (`app.js`), which uses `createTextNode` for text |
| Widget | `src/renderer/widget.html`, `widget.js` | Always-on-top mini toolbar: `stats`, `boost`, `openMain`, `closeWidget` |
| Backend selector | `src/main.js` line 9–10 | `DEMO` (non-Windows or `--demo`) loads `backend/mock.js`, else `backend/windows.js` |
| PowerShell runner | `src/backend/pscore.js` | `ps()`, `elevated()`, `isAdmin()`, `asSystem()`, `stream()` |
| Windows backend | `src/backend/windows.js` + `win-clean.js`, `win-procs.js`, `win-reliability.js`, `win-uninstall.js` | All Windows features as PowerShell scripts |
| Catalog | `src/backend/catalog.js` | Static data: 97 tweaks, 25 cleanup targets, 15 services, 12 scheduled tasks, 22 repairs, 43 toolbox entries, 39 bloat apps, 17 protected process names, DNS providers |
| Mock backend | `src/backend/mock.js` | Demo data for macOS/Linux or `--demo` |
| Space analyzer | `src/backend/disk.js`, `diskscan-worker.js` | Pure Node: `worker_threads` folder scan, tree queries, snapshots in `userData` |

### Electron window settings

`webPrefs` in `src/main.js`: `contextIsolation: true`, `nodeIntegration: false`, `sandbox: true`, preload only.
`createMain()` denies `window.open` (`setWindowOpenHandler` → `deny`) and blocks navigation
(`will-navigate` → `preventDefault`). Both HTML files set a meta CSP (`default-src 'self'; script-src 'self'; ...`).
This part is in good shape.

## 3. IPC model

- One channel: `ipcMain.handle('api', (e, method, args) => api[method](args))` in `src/main.js`.
- The `api` object has about 100 methods (54 in `main.js`, 45 from `api-extra.js`, plus `guardStatus`
  and `guardRun`). `INTERNAL_ONLY` (`organizeUndo`, `restoreBackup`, `_stopAll`) are refused from the renderer.
- The handler does not check `e.sender` / `e.senderFrame`. Both windows (main and widget) can call every method.
- Events go back over one channel `event` (`send()` in `main.js`): `stability`, `update`, `procs`,
  `disk-progress`, `clean-progress`, `batch-progress`, `go`.

### "ID-only" validation

The stated rule (comments in `main.js` and `api-extra.js`): the renderer sends IDs; commands and paths are
looked up from data the main process produced. Implemented as caches:

| Cache | Filled by | Used by |
|---|---|---|
| `cache.apps`, `cache.startup`, `cache.deletable` (`main.js`) | `installedApps`, `startupItems`, `largeFiles`/`duplicates` | `uninstallApp`, `setStartup`, `deleteFiles`, `revealFile` |
| `cache.programs`, `leftovers`, `clean`, `exts`, `features`, `winget`, `procs` (`api-extra.js`) | the matching list calls | uninstall, leftovers, extensions, features, winget, priority |
| `disk.known()` (`disk.js`) | last space-analyzer scan | `diskTrash`, `diskMove`, `diskCompress`, `diskReveal` |

Catalog lookups are used for tweaks (`C.TWEAKS`), cleanup (`C.CLEANUP`), services (`C.SERVICES` + `SERVICE_TYPES`),
tasks (`C.SCHEDULED_TASKS`), repairs (`C.REPAIRS`), tools (`C.TOOLBOX`), DNS (`C.DNS_PROVIDERS`). Regex checks
are used for package names, KB IDs, power plan GUIDs, winget IDs and URIs.

Strength: arbitrary PowerShell text never comes from the renderer. Gaps are listed in `security-audit.md`
(scan roots chosen by the renderer, shortcuts, `undo` replaying stored calls, no sender check).

## 4. PowerShell execution model (`src/backend/pscore.js`)

| Function | What it does |
|---|---|
| `withArgs(script, args)` | Prepends `PRELUDE` (`Out-Json`, `Get-DirSize`) and embeds `args` as JSON in a single-quoted here-string `@'...'@`, parsed with `ConvertFrom-Json` into `$a`. `'@` in the JSON is escaped so data cannot end the here-string. |
| `writeScript(text)` | Writes the script to `%TEMP%\opensweep-<16 hex>.ps1` (UTF-8 with BOM). |
| `ps(script, args)` | `execFile(powershell.exe, -NoProfile -NonInteractive -ExecutionPolicy Bypass -File <tmp>)`, 120 s default timeout, 64 MB buffer, parses JSON from stdout, deletes the file. Runs with the app's token, which is elevated in the installed app. |
| `isAdmin()` | One cached PowerShell call checking the Administrators role. |
| `elevated(script, args, {wait, visible})` | If already admin and not `visible`: runs through `ps()`. Otherwise writes an inner script plus a result file in `%TEMP%`, starts it with `Start-Process -Verb RunAs` (UAC prompt when not admin), waits, reads the result JSON. `visible` opens a console (used by `runRepair`). |
| `asSystem(script, args)` | Writes `sys-<id>.ps1` into `%ProgramData%\OpenSweep`, registers a one-shot scheduled task running as `SYSTEM` (via `elevated()`), polls for the result file, unregisters the task. Used only by `setTweaks` (`windows.js`) to retry HKLM writes denied to administrators. |
| `stream(script, args, onData, onExit)` | `spawn`s a long-running script and parses one JSON object per stdout line. Used by the live process view (`win-procs.js` `LOOP`, one sample every ~1.8 s, auto-stopped after 10 min without `procKeepAlive`). |

Call-site counts: `elevated(` appears 20 times in `windows.js`, 8 in `win-uninstall.js`, 4 in `win-clean.js`.

Observations:

- Every call starts a new `powershell.exe` (no pooling). Typical reads (`systemInfo`, `security`,
  `tweakStates`) use several CIM queries each.
- Because the installed app is already elevated, `elevated()` almost always takes the in-process `ps()` branch;
  the UAC branch matters only for `npm start` / unelevated runs and for `visible` repairs.
- JSON parsing slices stdout from the first JSON-looking character, so stray output can be misparsed.
- `relaunchAsAdmin` (`main.js`) and some scripts call tools by name (`reg.exe`, `sc.exe`, `pnputil.exe`,
  `winget.exe`, `compact.exe`, `takeown.exe`, `icacls.exe`, `dism.exe`), resolved through `PATH`.

## 5. Feature inventory

| Area | IPC methods | Backend | Privilege used |
|---|---|---|---|
| Dashboard / health score | `health`, `stats`, `systemInfo` | `health()` in `main.js` combines `systemInfo`, `cleanupScan`, `startupItems`, `security`, `tweakStates`, stability; `stats()` uses `os.cpus()`/`os.freemem()` | read |
| Boost | `boost` (also tray menu and widget) | `boost()` in `windows.js` | app token (admin) |
| Cleanup | `cleanupScan`, `cleanupRun`, `cleanAllScan`, `cleanAllRun` | `cleanupScan/Run`, `oldDrivers`, `removeDrivers`, `windowsOldSize`, `removeWindowsOld`, `componentCleanup` | `elevated` for admin categories |
| Scheduled clean | `setSettings({autoClean})` → `setCleanSchedule`; `--auto-clean` argv | Task "OpenSweep Auto Clean", Highest run level | elevated task |
| Storage | `largeFiles`, `duplicates`, `folderSizes`, `deleteFiles`, `revealFile`, `disk*` | PowerShell + `disk.js` worker | app token |
| File organiser | `organizeDownloads`, `organizePick`, undo via `organizeUndo` | pure Node in `main.js` | app token |
| Apps / uninstaller | `installedApps`, `uninstallApp`, `programs`, `uninstall`, `uninstallBatch`, leftovers, orphans, extensions, features, install tracker, `hunt`, winget | `win-uninstall.js` | `elevated` |
| Startup | `startupItems`, `setStartup` | StartupApproved registry values and scheduled tasks | `elevated` for HKLM/tasks |
| Processes | `processes`, `killProcess`, `procStart/Stop/KeepAlive`, `setPriority`, `processLocation`, `processSearch` | `windows.js`, `win-procs.js` | app token |
| Security | `security`, `threats`, `defenderAction` | Defender cmdlets, firewall, SMB1, RDP | `elevated` |
| Network | `network`, `latency`, `networkAction` | DNS set (`elevated`), flush, renew, hosts in Notepad (`-Verb RunAs`); `latency` is TCP connect in Node | mixed |
| Tweaks + Settings guard | `tweakStates`, `setTweaks`, `guardStatus`, `guardRun` | `TWEAK_SET` script; HKLM via `elevated`, retry via `asSystem` | admin, SYSTEM fallback |
| Debloat, services, tasks | `bloatStatus`, `removeApps`, `servicesStatus`, `setServices`, `tasksStatus`, `setTasks` | Appx, `Set-Service` + registry `Start` fallback, task cmdlets | `elevated` |
| Windows Update | `updatesStatus`, `updatesAction` | UX\Settings pause/active hours, `TargetReleaseVersion` policy, `wusa` | `elevated` |
| Power | `powerPlans`, `setPowerPlan` | `powercfg` | app token |
| Repair / toolbox | `runRepair`, `launchTool` | catalog scripts (SFC, DISM, chkdsk, network reset...) in a visible elevated console | `elevated` |
| Drivers & stability | `stability`, `scanDevices`, `reliability` | `stabilityCheck()` (PnP problem codes, System log events, old/unsigned drivers), `win-reliability.js` (Reliability Monitor WMI) | read; `pnputil /scan-devices` elevated |
| History & undo | `history`, `undo` | `settings.history` (max 300) with optional `undo: { fn, args }` | replays an `api` call |
| App | `info`, `getSettings`, `setSettings`, `relaunchAsAdmin`, `updateStatus`, `checkUpdate`, `installUpdate`, shortcuts, `openUri`, `openRepo` | `main.js` | — |

### Boost (flagged)

`boost()` in `src/backend/windows.js`:

1. Calls `psapi!EmptyWorkingSet` on **every process** it can open (`Get-Process | ForEach-Object { EmptyWorkingSet($_.Handle) }`).
2. Deletes files older than one day from `%TEMP%`.
3. Runs `Clear-DnsClientCache`.
4. Reports `memFreedMB` as the change in `Win32_OperatingSystem.FreePhysicalMemory` 700 ms later.

This is a **fake RAM booster** in the sense of Part 1 rule 11 and Part 4 ("Do not implement periodic
EmptyWorkingSet sweeps"). Trimmed pages go to the standby/modified lists; processes fault them back in
(soft faults, or hard faults if they were written to the page file), so the system usually gets slower right
after a Boost, and the "freed" number mostly measures pages Windows would have reused on demand anyway.
It is reachable from the dashboard button, the tray menu ("Boost now"), the widget, and the health check
suggests it when memory use is above 85 % (`health()` in `main.js`). Each run is logged to History as
"Boost freed N MB RAM".

Recommendation: keep the button (Part 1 rule 2: do not remove features) but change what it does in a
documented breaking change: show a memory diagnosis (commit charge, hard faults, top private-bytes processes)
and only offer targeted actions. Temp-file clean and DNS flush already exist elsewhere (`cleanupRun`,
`networkAction('flushdns')`). See `migration-plan.md`.

### Settings guard

`runGuard()` in `main.js`. `settings.guardTweaks` holds tweak IDs the user turned on (`setTweaks` adds/removes
them). Every hour (from `scheduleStability()`'s 15-minute tick, 3 h on battery) and 2 minutes after start,
it reads all tweak states (`tweakStates()`), re-applies any guarded tweak that reads as off, logs a History
entry and shows a notification. It only runs while the app is running, and it re-applies without checking
why the value changed (Group Policy, MDM or a Windows update may be the owner).

### Startup optimisation

`startupItems()` reads HKCU/HKLM/HKLM32 `Run`, the user and common Startup folders, and non-Microsoft
scheduled tasks with logon/boot triggers. `setStartup()` writes the Explorer `StartupApproved` binary value
(same mechanism as Task Manager) or enables/disables the task, then `api.setStartup` re-reads to verify.
Undo is recorded. There is no measurement of boot impact.

### Process management

- Snapshot list: `processes()` (`Get-Process` + `Win32_PerfFormattedData_PerfProc_Process`).
- Live list: `win-procs.js` `LOOP` (CPU from `TotalProcessorTime` deltas, I/O from `Win32_Process` transfer
  counts, version info and Authenticode check with a 300 ms budget per sample).
- `killProcess`: PID > 4 and name not in `C.PROTECTED_PROCESSES`. The name is supplied by the renderer, not
  looked up from the PID, so the protected list is advisory.
- `setPriority`: PID must be in `cache.procs`, name checked against the protected list, priority checked
  against `PRIORITIES` (`Idle`..`High`; `RealTime` is not allowed).

### Stability monitor

`runStability()` / `scheduleStability()` in `main.js`, backed by `stabilityCheck()` in `windows.js`:
PnP devices with `ConfigManagerErrorCode` ≠ 0, and the last 7 days of System log events (BugCheck 1001,
Kernel-Power 41, WHEA-Logger, Display 4101, disk 7/51/153, Ntfs 55), plus third-party drivers older than five
years and unsigned drivers. Runs at Idle priority, hourly (3 h on battery), first run 90 s after start.
New items (tracked in `settings.stabilitySeen`, max 500 keys) trigger a desktop notification.
`reliability()` reads `Win32_ReliabilityStabilityMetrics` / `Win32_ReliabilityRecords` on demand.

This is the closest existing code to SweepTrace/SweepGuard. It is polling only, has no baseline, no
persistence beyond "seen" keys, and stops when the app quits.

### Auto-update

`setupUpdater()` in `main.js` using `electron-updater` 6.8.11 with the GitHub provider
(`build.publish` in `package.json`: `ray0189/OpenSweep`, `releaseType: release`). Settings: `autoDownload: true`,
`autoInstallOnAppQuit: true`, **`allowPrerelease: true`**. Checks 20 s after start and every 4 h if
`settings.autoUpdate`. If the window is hidden when a download finishes, it calls `quitAndInstall(true, true)`
(silent install, relaunch) without asking. Disabled in demo and unpackaged runs.

### Packaging

`electron-builder` 26, targets NSIS and zip, `signAndEditExecutable: false` (no code signing, no resource
editing), `requestedExecutionLevel: requireAdministrator`. NSIS: `oneClick: false`,
`allowToChangeInstallationDirectory: true`, desktop shortcut. `perMachine` is not set (electron-builder's
default is a per-user install; with `oneClick: false` the installer may offer a choice — verify on a build).
Scripts exist for x64 (`dist:win`) and arm64 (`dist:win-arm64`); the release workflow only builds x64.

Start with Windows: because a `requireAdministrator` app cannot start from the Run key, `setAutoStart()`
registers a logon task "OpenSweep" with `RunLevel Highest` that runs `process.execPath --hidden`
(`applySettings()` in `main.js`).

### CI and tests

| Workflow / test | What it does |
|---|---|
| `.github/workflows/windows-test.yml` | On pushes touching `src/backend/**` or `test/**`, PRs and manual runs: `node test/windows-smoke.js` on `windows-latest`. No `npm install`. |
| `test/windows-smoke.js` | Calls every read function, applies every tweak and reverts it, sets/restores services and tasks, Windows Update pause/resume, autostart task, `asSystem` whoami, reliability, process stream, leftovers on a fake app with backup/restore, install tracker, disk scan, clean schedule. Real system changes on a throwaway VM. |
| `test/check.js` (`npm run check`) | `node --check` syntax check of every `.js` in `src`, `src/backend`, `src/renderer`, `test`. |
| `.github/workflows/release.yml` | On `v*` tags or manual: `npm ci`, `npm run check`, `electron-builder --win --x64 --publish always` with `GITHUB_TOKEN` (`contents: write`). Actions pinned by tag (`@v4`), not commit SHA. |

Not covered: `main.js` and `api-extra.js` logic (validation, caches, undo, guard, health scoring), the renderer,
the mock backend, elevation paths other than "already admin", arm64, Windows 10 (runner image is a Windows
Server build).

## 6. Duplication

| Duplicate | Where | Note |
|---|---|---|
| Two installed-apps lists and two uninstall paths | `installedApps`/`uninstallApp` (`windows.js`, used by `main.js`) and `programs`/`runUninstall` (`win-uninstall.js`, used by `api-extra.js`) | Same registry keys read twice with different IDs (`w:`+PSPath vs `w:`+regKey). `uninstallApp` runs the uninstall string through `cmd.exe /c`. |
| Two process lists | `processes()` and the `win-procs.js` stream | Different CPU calculations. |
| Two CPU/memory sources | `stats()` (Node `os`) and `systemInfo()` (CIM) | `stats()` CPU is a per-call delta, so two callers (window + widget) disturb each other's sample. |
| `str()` helper and cache pattern | `main.js` and `api-extra.js` | |
| Logon scheduled-task registration | `setAutoStart()` and `setCleanSchedule()` | Same pattern, both Highest run level. |
| "Verify after change" logic | `api.setStartup`, `api.defenderAction`, `api.networkAction`, `api.updatesAction`, `verifyTweaks`, `setServices`, `setTasks` | Good practice, implemented separately each time. |
| Temp cleanup | `boost()` deletes `%TEMP%` files; `C.CLEANUP` has temp targets | |
| Health checks that rescan | `health()` runs `cleanupScan`, `security`, `tweakStates`, `startupItems` each call | Expensive; no caching except stability (3 h). |

## 7. Opportunities for native integration (Core service)

| Today | Native replacement (planned, not implemented) | Plan part |
|---|---|---|
| `stats()` via `os.cpus()` deltas; `systemInfo()` CIM | `GetSystemTimes`, `GlobalMemoryStatusEx`, `GetPerformanceInfo` in the service; `metrics` command in `ipc-protocol.md` | Phase 1–2 |
| `processes()` + 1.8 s PowerShell loop | Documented APIs (`CreateToolhelp32Snapshot` or `EnumProcesses`, `GetProcessMemoryInfo`, `GetProcessTimes`, `GetProcessIoCounters`) or PDH, sampled in the service | Phase 2 |
| Hourly `stabilityCheck()` polling | `EvtSubscribe` push subscriptions on the same System log queries; PnP via `CM_Register_Notification` (verify) | Phase 2–3 |
| `reliability()` WMI | Keep as on-demand read; correlate with service incident history | Phase 3 |
| Settings guard hourly poll | `RegNotifyChangeKeyValue` on guarded keys in the service, with a check for policy ownership before re-applying | Phase 5 |
| Boost (`EmptyWorkingSet`) | SweepMemory diagnosis; `SetProcessInformation(ProcessMemoryPriority)` only in later opt-in phases | Phase 4–5 |
| `setPriority` | SweepScheduler with EcoQoS (`ProcessPowerThrottling`), CPU Sets, journaled and reverted | Phase 4–5 |
| CPU/core info from `Win32_Processor` | `GetLogicalProcessorInformationEx` (`topology` command) | Phase 1 |
| `asSystem()` scheduled-task trick | Service running as LocalSystem executing typed commands | Phase 5+ |
| `elevated()` for every admin action | Broker commands with fixed implementations; UI runs as standard user | Migration stages C–D |

## 8. Weaknesses (consolidated)

1. Whole app elevated, including the renderer host and the updater.
2. PowerShell temp scripts in user-writable `%TEMP%` and `%ProgramData%\OpenSweep` executed elevated / as SYSTEM.
3. Polling-based monitoring that stops when the app closes.
4. Boost trims all working sets.
5. Unsigned builds plus silent auto-update with pre-releases enabled.
6. `settings.json` is both config and an action log whose `undo` entries are replayed as IPC calls.
7. No unit tests for main-process logic; smoke test only on Windows Server runner; no arm64 CI.
8. No structured logging; errors are mostly swallowed (`catch {}`) or shown as toasts.
9. Single-file `main.js` mixing settings, scheduling, health scoring, updater and IPC.
