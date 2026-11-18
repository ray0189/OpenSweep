# OpenSweep security audit

Scope: OpenSweep 0.3.2 as committed at `94c5eea` (`package.json`, `src/**`, `.github/workflows/**`), plus a design
review of the Core IPC spec in `docs/core/ipc-protocol.md`. The Phase 1 service implementation being added under `core/`
needs its own review once it is committed and has run on Windows. Part 15 of `MASTER_PROMPT_CORE.md` is the
checklist.

Method: code reading only. No finding below was reproduced on Windows for this audit. Items whose
exploitability depends on Windows ACL or integrity-level behaviour that was not tested are marked **verify**.
They should be confirmed on a disposable Windows VM before being treated as proven, and fixed regardless,
because the fixes are cheap.

Severity: **High** = plausible path to admin/SYSTEM code execution or silent compromise of all installs;
**Medium** = privileged action beyond intent, needs another bug or local foothold; **Low** = hardening or privacy.

## 1. Threat model

| Actor | Starting point | Goal |
|---|---|---|
| T1 Malware running as the same user, medium integrity (no admin) | Can write `%TEMP%`, `%APPDATA%`, `%LOCALAPPDATA%`, HKCU | Get admin without a UAC prompt, using OpenSweep |
| T2 Another standard user on the same PC | Can write `%ProgramData%` subfolders (default ACL) | Get admin/SYSTEM |
| T3 Attacker controlling data OpenSweep displays | Process names, window titles, event log messages, registry values, app names, extension manifests | Script injection in the renderer → admin IPC |
| T4 Supply-chain attacker | GitHub account/token, a dependency, a CI action, the release assets | Push code to every install |
| T5 Future: any local process talking to the Core pipe | Interactive user session | Abuse privileged Core commands |

## 2. Current privilege model

- `package.json` → `build.win.requestedExecutionLevel: "requireAdministrator"`. Every launch asks for UAC, and the
  main process, the GPU/utility processes, the renderers and the updater all run with the full admin token
  (renderers are sandboxed, see §3).
- "Start with Windows" (`setAutoStart()` in `src/backend/windows.js`) and scheduled clean (`setCleanSchedule()`)
  register scheduled tasks with `RunLevel Highest` that start `process.execPath` with no UAC prompt.
- `asSystem()` (`src/backend/pscore.js`) runs scripts as `SYSTEM` through a temporary scheduled task when an HKLM
  write is denied to administrators.

Consequence: any code execution in the main process, or any IPC method that can be steered to do more than
intended, is admin code execution.

## 3. Electron configuration

| Control | Status | Where |
|---|---|---|
| `contextIsolation` | on | `webPrefs` in `src/main.js` |
| `nodeIntegration` | off | same |
| `sandbox` | on | same |
| Preload surface | 2 functions: `api.call(method, args)`, `api.on(cb)` | `src/preload.js` |
| CSP | meta tag `default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data:` (widget: no `img-src`) | `src/renderer/index.html`, `widget.html` |
| Navigation | `will-navigate` prevented, `setWindowOpenHandler` denies | `createMain()` |
| DOM building | `h()` uses `createTextNode` for text; `innerHTML` only in 4 places with numeric/static SVG | `src/renderer/app.js`, `pages.js`, `icons.js` |
| IPC sender check | none (`ipcMain.handle('api', ...)` ignores the event) | `src/main.js` |
| Electron fuses (`RunAsNode`, `EnableNodeOptionsEnvironmentVariable`, `EnableNodeCliInspectArguments`, `EnableEmbeddedAsarIntegrityValidation`, `OnlyLoadAppFromAsar`) | not configured (Electron defaults) | `package.json` |
| Permission request handler | not set | — |

The renderer hardening is good. T3 (XSS from displayed system data) is low risk because text goes through
text nodes. The weak point is not the renderer but what the main process will do on its behalf, and what it
trusts on disk.

## 4. Findings

### S1 — High — Whole app runs as administrator

`requireAdministrator` means a renderer compromise (Chromium bug, future XSS) or a logic bug in any of ~100 IPC
methods is admin. It also means the updater, file dialogs, `shell.openPath`, `shell.openExternal` and
`app.getFileIcon` run elevated. Part 15 asks for least privilege and "no renderer-controlled arbitrary shell
execution"; this design cannot meet it.

Fix: Core architecture — UI as standard user (`asInvoker`), privileged work in a service behind an ACL'd pipe with
typed commands (see §6 and `migration-plan.md`).

### S2 — High (verify) — Elevated execution of scripts stored in the user's `%TEMP%`

`writeScript()` (`pscore.js`) writes `%TEMP%\opensweep-<random>.ps1`; `ps()` then runs it with the app's token
(admin in the installed app). In the non-admin branch of `elevated()`, the script and the result file are written
to `%TEMP%` and executed after the user approves a UAC prompt (which can take seconds). `runRepair` scripts
(`visible: true`) stay on disk for 60 s.

`%TEMP%` belongs to the user. A medium-integrity process of the same user (T1) can watch the folder
(`ReadDirectoryChangesW`) and replace or append to the `.ps1` between write and execution. Whether a file created
by an elevated process in `%TEMP%` stays writable to medium-integrity processes depends on the inherited DACL and
the mandatory label applied on creation — **verify**. If it does, this is a UAC bypass: the attacker's code runs
with the admin token without its own prompt. The result file (`Out-Result` JSON) can also be forged to make the
UI report success.

Also in `%TEMP%`: assemblies compiled by `Add-Type` (`boost()`, `hunt()`).

Fix now (independent of Core): pass the script without a file (e.g. `-EncodedCommand` for short scripts, or stdin
with `-File -`, **verify** size limits), or write into a directory created with an explicit DACL (SYSTEM +
Administrators only) and open the file with exclusive sharing until PowerShell has read it. Long term: no
PowerShell executed elevated from user-writable locations at all.

### S3 — High (verify) — `%ProgramData%\OpenSweep` holds scripts run as SYSTEM and data used for elevated actions

| Path | Written by | Consumed as |
|---|---|---|
| `%ProgramData%\OpenSweep\sys-<id>.ps1` | `asSystem()` | executed by a `SYSTEM` scheduled task |
| `%ProgramData%\OpenSweep\backups\<stamp>\*.reg`, `task-*.xml` | `removeLeftovers()` (`win-uninstall.js`) | `restoreBackup()` runs `reg.exe import` on **every** `*.reg` and `Register-ScheduledTask -Xml` on every `task-*.xml` in the folder, elevated |
| `%ProgramData%\OpenSweep\tracks\*.json` | `trackStart()`/`trackFinish()` | `trackItems()` → `removeLeftovers()` deletes the listed registry keys, services, tasks and firewall rules elevated |

The directory is created with `fs.mkdirSync(..., { recursive: true })`, so it inherits the `%ProgramData%` DACL.
On a default Windows install that DACL lets `BUILTIN\Users` create files and subfolders in subfolders, with
`CREATOR OWNER` full control over what they create. Two consequences (**verify** on Windows 10 and 11):

1. A standard user (T2) can add a `.reg` file to an existing backup folder, or a `task-*.xml`, which is imported
   with admin rights the next time someone clicks Undo for that History entry. A `.reg` import can write any HKLM
   value (services, IFEO, Run keys) → admin/SYSTEM.
2. If a standard user creates `%ProgramData%\OpenSweep` before OpenSweep does, they own it and can replace
   `sys-<id>.ps1` between write and task start → SYSTEM code execution.

T1 (same user, medium integrity) has the same reach. Track JSON files can be edited to make the install tracker
offer arbitrary registry keys or services for deletion.

Fix: create `%ProgramData%\OpenSweep` (later `OpenSweepCore`) in the installer with an explicit, non-inherited DACL
(SYSTEM and Administrators full; Users read or nothing) and refuse to run if the owner or DACL is not as expected.
Restore only files listed in a manifest written at backup time, with hashes. Remove `asSystem()`; HKLM keys that
Windows locks against administrators are usually locked on purpose (Part 1 rule 9, Part 15 "No unnecessary SYSTEM
privileges").

### S4 — High — `settings.json` drives privileged actions

`settings.json` lives in `app.getPath('userData')` (`%APPDATA%\OpenSweep`), writable by any process of the user (T1).
It is loaded with `JSON.parse` and spread over `DEFAULTS` (`loadSettings()` in `main.js`) with no schema check. The
elevated app then trusts it:

- **Undo replays arbitrary IPC calls.** `api.undo({ index })` runs `api[h.undo.fn](h.undo.args)` for the stored
  History entry. `fn` is not checked against an allowlist, and `INTERNAL_ONLY` is not applied here, so a planted
  entry can call `organizeUndo` with `{ moves: [{ from, to }] }` (arbitrary elevated `fs.renameSync`, e.g. replace a
  file in `Program Files` or `System32`), `restoreBackup` (with the S3 problem), `setStartup`, `setFeature`, etc.
  The user only has to click Undo on an entry whose summary the attacker also wrote.
- **Shortcuts run elevated.** `api.openShortcut` passes `settings.shortcuts[i].target` to `shell.openPath` (or
  `openExternal` for http/https). Targets can be any path, set either by editing the file or by the renderer through
  `api.setSettings({ patch: { shortcuts } })`. This is renderer-controlled arbitrary execution as admin, which Part 15
  forbids.
- `guardTweaks` is filtered against `C.TWEAKS`, so the Settings guard cannot be pointed at arbitrary keys (good), but
  a planted list will silently re-apply catalog tweaks every hour.
- `stabilitySeen` and `history` sizes are capped only when written, not when read.

Fix: validate settings with a schema on load; store undo as `{ kind, id }` references resolved against an allowlist
of undoable operations with validated arguments; never replay `INTERNAL_ONLY` methods from data; shortcuts should run
with the UI's (standard-user) token once the UI is unelevated, and until then be limited to paths the user picks with a
file dialog in the main process. Long term the service keeps its own journal (Part 8/10) in its protected directory.

### S5 — High (verify) — Elevated logon tasks start an executable that may be user-writable

`setAutoStart()` and `setCleanSchedule()` register tasks with `RunLevel Highest` running `process.execPath`. If the app
was installed per-user (electron-builder's default `perMachine: false`; **verify** which mode the current installer
uses), the executable and `resources\app.asar` are in `%LOCALAPPDATA%\Programs\OpenSweep`, writable by the user.
T1 can modify `app.asar` (no asar integrity fuse) or plant a DLL next to the exe, and the next logon runs it elevated
with no prompt — a persistent UAC bypass. Even per-machine, the elevated process inherits the user's environment
block, and Electron's `ELECTRON_RUN_AS_NODE` / `NODE_OPTIONS` handling is not disabled with fuses (**verify** which
variables a packaged Electron 45 app honours).

Fix: per-machine install only (`nsis.perMachine: true`) for anything that is started elevated; enable fuses
(`RunAsNode` off, `EnableNodeOptionsEnvironmentVariable` off, `EnableNodeCliInspectArguments` off,
`EnableEmbeddedAsarIntegrityValidation` on, `OnlyLoadAppFromAsar` on). With the Core design the UI no longer needs an
elevated logon task at all: it can use the normal Run key (`app.setLoginItemSettings`), and scheduled work moves into
the service.

### S6 — High — Unsigned builds with silent auto-update and pre-releases

- `signAndEditExecutable: false`; no code-signing configuration in `package.json` or `release.yml`.
- `setupUpdater()` (`main.js`): `autoDownload: true`, `autoInstallOnAppQuit: true`, `allowPrerelease: true`, and a
  silent `quitAndInstall(true, true)` when the window is hidden.
- Without a signed build, `electron-updater` has no publisher signature to check on Windows; integrity rests on the
  SHA-512 in `latest.yml`, which is published in the same GitHub release as the installer. Anyone who can publish a
  release (maintainer account, a leaked token, a compromised workflow step) can push an admin-level binary to every
  install, and with `allowPrerelease: true` even a pre-release is enough.
- `release.yml` pins actions by tag (`actions/checkout@v4`, `actions/setup-node@v4`), not by commit SHA, and grants
  `contents: write` to the whole job. `npm ci` with a lockfile is used (good).

Fix: Authenticode-sign the installer, the app exe and (later) `OpenSweepCore.exe`; set `publisherName` so
electron-updater verifies the signature of downloaded updates; set `allowPrerelease: false` for the stable channel;
ask before installing silently; pin actions by SHA; use environment protection rules for the release job. The service
binary must be signed before it is installed as a service (Part 15 "Code signing", "Secure updates").

### S7 — Medium — Renderer chooses the scan roots that later authorise deletion

The "ID-only" rule holds for apps, startup items, tweaks, services, tasks and features. For files it is weaker:
`largeFiles({ root })`, `duplicates({ root })` and `diskScan({ root })` accept any absolute path from the renderer, and
every path they return becomes deletable (`cache.deletable`, `disk.known()`). A compromised renderer can therefore scan
`C:\Windows\System32` with `minMB: 1` and then call `deleteFiles` / `diskTrash` / `diskMove` / `diskCompress` on the
results, all elevated. `largeFiles` skips files with the System attribute; `duplicates` and the disk scan do not.

Fix: in the unelevated UI this becomes ordinary user-level file access (the OS ACLs then protect system files). Until
then, refuse deletes under `%SystemRoot%`, `Program Files` and `ProgramData` unless the scan root was picked with a
main-process folder dialog.

### S8 — Medium — `killProcess` protection relies on a renderer-supplied name

`api.killProcess({ pid, name })` checks `name` against `C.PROTECTED_PROCESSES` but kills `pid`. A renderer can pass a
harmless name with the PID of `lsass`/`csrss` (killing `csrss` bugchecks the machine). `setPriority` does it right (name
taken from `cache.procs`). Fix: look the name up from the PID in the main process; in Core, the service resolves the
image path and checks critical/protected status (`IsProcessCritical`, protected-process checks).

### S9 — Medium — Executables resolved through `PATH` inside elevated scripts

Elevated scripts call `reg.exe`, `sc.exe`, `pnputil.exe`, `compact.exe`, `takeown.exe`, `icacls.exe`, `dism.exe`,
`winget.exe` by name; `relaunchAsAdmin` runs `powershell.exe` by name. System directories come first in the machine
`PATH`, so most are safe, but `winget.exe` is an App Execution Alias in `%LOCALAPPDATA%\Microsoft\WindowsApps`, a
user-writable directory on the user `PATH` (**verify** whether a medium process can replace the alias). `powershell.exe`
itself is called by absolute path in `pscore.js` (good). Fix: absolute `%SystemRoot%\System32\...` paths; resolve winget
through its package location or run it unelevated (winget elevates per package when needed). For the native service:
`SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32)` and no `PATH` lookups (Part 15 "DLL search path security").

### S10 — Medium — No sender validation on IPC; widget has the full API

The handler does not check `event.senderFrame` URL or which window sent the call. Today both windows load local files
only, so this is defence in depth. Fix: per-window method allowlists (widget needs 4 methods) and a `file://` origin check.

### S11 — Low — `shell.openExternal` and outbound requests

- `openUri` allows only `ms-settings:[a-z-]*` and `windowsdefender:` (good).
- `openShortcut` opens any `http(s)` URL from settings (see S4).
- `processSearch` sends `<process name>.exe <company>` to DuckDuckGo on user click (privacy: user-initiated, but say so in the UI).
- `openRepo` opens `https://github.com/` rather than the project page (functional bug, not security).
- `latency()` connects to 1.1.1.1, 8.8.8.8, 9.9.9.9 on request; the updater contacts GitHub every 4 h when enabled.
  No other network traffic and no telemetry were found.

### S12 — Low — Logging and local data privacy

There is no log file. Persistent local data: `settings.json` History (up to 300 entries; includes full file paths for
organiser moves and undo args, app names, DNS choices), `stabilitySeen` keys, space-analyzer snapshots in `userData`,
leftover backups and install-tracker snapshots in `%ProgramData%\OpenSweep` (registry key names, service and task names of
the whole machine). Event messages (300/600 chars) are shown but not stored. Nothing is uploaded. For Core: keep
SweepTrace local and bounded (Part 6), exclude command lines and window titles from persisted incidents by default (they
can contain document names or tokens), and document retention.

### S13 — Info — Things done well

- No PowerShell text from the renderer; arguments go through JSON in a here-string. `JSON.stringify` never emits a raw
  newline, so `'@` can never start a line and terminate the here-string; the extra `'@` escape in `withArgs()` is belt and braces.
- Most mutating methods look IDs up in main-process caches or the static catalog, and many verify the result by reading
  back (`verifyTweaks`, `setServices`, `setTasks`, `setStartup`, `updatesAction`, `defenderAction`).
- Regex validation for package names, KB IDs, GUIDs, winget IDs, Windows version strings, priorities.
- Uninstall strings come from the registry, not the renderer; `RUN_UNINSTALL` splits the command rather than using `cmd /c`
  (the older `uninstallApp` path still uses `cmd.exe /c`).
- Process stream auto-stops after 10 minutes; scans have time budgets.

## 5. Review of the planned Core IPC (`docs/core/ipc-protocol.md`)

| Aspect | Spec | Comment |
|---|---|---|
| Remote clients | `PIPE_REJECT_REMOTE_CLIENTS` + `D;;GA;;;NU` | Good. |
| Squatting | `FILE_FLAG_FIRST_PIPE_INSTANCE` on the server | Protects only if the service creates the pipe first. If the service is stopped, any user can create `\\.\pipe\OpenSweepCore.v1`. **The client must verify the server**: `GetNamedPipeServerProcessId` and compare with the service PID from `QueryServiceStatusEx`, or check the server process token is `SYSTEM`. Do not send anything sensitive before that check. |
| Access | SY/BA full, IU read/write | Read-only Phase 1 commands are fine for all interactive users, including other users in fast-user-switching and RDP sessions. Per-user data (other users' process command lines, window titles) must not be returned to every IU client. No explicit mandatory label means default medium: low-integrity and AppContainer clients cannot write — good, keep it. |
| `admin` capability | "client token must be elevated/Administrators" | Decide on the token, not on the PID: `ImpersonateNamedPipeClient` → `OpenThreadToken` → check `TokenElevation` and Administrators membership → `RevertToSelf`. A PID can be reused and says nothing about the token. |
| Limits | 64 KiB requests, 1 MiB responses, 8 clients, 5 s read deadline, 60 s idle | Good. Add a per-client request rate limit and cap concurrent in-flight requests per connection. |
| Commands | typed, versioned, unknown → `unknown_cmd` | Keep the rule from Part 3: no command takes a script, path to execute, registry path or service name chosen by the client except through an allowlist. |
| Audit | PID + user logged | Also log the capability decision and the command result; keep logs in the service's ACL'd directory. |

Service hardening checklist (for Phase 1 implementation): run as `LocalService` if the Phase 1 commands work without
SYSTEM, otherwise `LocalSystem` with `RequiredPrivileges` limited to what is used (**verify** which privileges ETW kernel
sessions in Phase 2 need); service SID type unrestricted so its directory can be ACL'd to the service SID; binary under
`Program Files` (per-machine install); `SetDefaultDllDirectories`; `SetProcessMitigationPolicy` (e.g. image-load
restrictions, **verify** compatibility); recovery actions with a restart limit; no PowerShell inside the service until the
broker stage, and then only fixed scripts shipped in the signed install directory.

## 6. Recommendations mapped to the Core architecture

| # | Recommendation | Fixes | Stage (see `migration-plan.md`) |
|---|---|---|---|
| R1 | Installer creates `%ProgramData%\OpenSweep*` with an explicit non-inherited DACL; app checks owner/DACL before use | S3 | A (now) |
| R2 | Stop writing elevated scripts to `%TEMP%`; use a protected directory or no file | S2 | A (now) |
| R3 | Allowlist undo operations; schema-validate `settings.json`; never replay `INTERNAL_ONLY` | S4 | A (now) |
| R4 | `killProcess` resolves name from PID; per-window IPC allowlist and sender check | S8, S10 | A (now) |
| R5 | Code signing, `publisherName`, no silent pre-release updates, SHA-pinned actions | S6 | A–B |
| R6 | Per-machine install, Electron fuses | S5 | B (needed before the service is installed) |
| R7 | Core service with ACL'd pipe, client verifies server, token-based `admin` capability | S1, §5 | B–C |
| R8 | Move privileged features to typed broker commands; remove `asSystem()` | S1, S3, S9 | C |
| R9 | UI `requestedExecutionLevel: asInvoker`; normal Run-key autostart; scheduled clean in the service | S1, S5, S7 | D |
| R10 | Absolute tool paths; `SetDefaultDllDirectories` in the service | S9 | A / B |

## 7. Security tests to add (Part 16)

- Unit: settings schema rejects unknown undo `fn`; `killProcess` refuses protected PIDs regardless of name.
- Integration (Windows VM): `%ProgramData%\OpenSweep` ACL after install; a standard user cannot add files to it.
- Pipe: oversized frame, malformed JSON, wrong version, unknown command, 9th client, slow-loris (no full frame in 5 s),
  connection from a network logon, client connecting to a squatted pipe when the service is stopped.
