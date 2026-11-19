# Security review — Phase 7 (Part 15)

Scope: the architecture after Phases 2–6 — `OpenSweepCore.exe` (service, named pipe, engine, actuator, journal,
SweepHeal), the Electron main-process bridge (`src/main.js`, `src/core-validate.js`, `src/core-client.js`), the Core
dashboard (`src/renderer/core-page.js`) and the benchmark harness (`bench/run.ps1`). It builds on `security-audit.md`
(findings S1–S13, recommendations R1–R10), which stays the reference for the rest of the app.

**Verification legend.** *Code-reviewed* = read in the source on `feature/opensweep-core`. *CI-verified* = exercised by
`core.yml` on GitHub's `windows-latest` runner (a VM, elevated). *Not verified on Windows* = reasoning or Linux-only
tests; must be confirmed on a Windows machine before it is relied on. Nothing below has been checked on a real
(non-CI) Windows 10/11 PC.

## 1. Summary

| # | Area | Status | Severity of what remains |
|---|---|---|---|
| P1 | Pipe authentication and `admin` capability | Token-based, fail-closed (code-reviewed); non-admin `forbidden` test added (needs CI run) | Low |
| P2 | Server identity (pipe squatting while the service is stopped) | **Open** — the Node client does not verify the server | Medium |
| P3 | Service account and privileges | LocalSystem with a reduced privilege list (code-reviewed) | Low (verify ETW needs) |
| P4 | DLL search path and image-load mitigations | Service hardened (code-reviewed); Electron side unchanged | Low |
| P5 | Configuration validation | Contract requires validated load + atomic writes; portable tests only | Low (not verified on Windows) |
| P6 | Journal / trace / diagnostics file ACLs | Inherit `D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)` (code-reviewed); stricter than the contract table | Low |
| P7 | Renderer → main → Core argument validation | Fixed method table + strict validation + native confirmation (unit-tested) | Low |
| P8 | `reportForeground` hint can be spoofed within a session | **Open** design risk, mitigated by session check | Low–Medium |
| P9 | Race conditions (PID reuse, TOCTOU, concurrent admin clients, crash during apply) | Mitigations in code; partly not verified on Windows | Low |
| P10 | Logging and data privacy | Mostly good; event messages may carry paths | Low |
| P11 | Update, signing and app privilege (S1, S5, S6) | **Still open** | High (inherited) |
| P12 | Memory safety of the C++ service | Bounded parsers, unit tests, 20-frame fuzz smoke test; no coverage-guided fuzzing | Medium |
| P13 | Benchmark harness | Admin-only tool, owns-only cleanup, no downloads | Low |

## 2. Findings

### P1 — Pipe authentication and the `admin` capability (code-reviewed; non-admin path not yet CI-verified)

- Pipe DACL `D:P(D;;GA;;;NU)(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)`, `PIPE_REJECT_REMOTE_CLIENTS`,
  `FILE_FLAG_FIRST_PIPE_INSTANCE` (CI-verified: the integration test prints the SDDL).
- `PipeServer::identify_client` impersonates the client after the first read, opens the thread token and grants
  `admin` only if `CheckTokenMembership(BUILTIN\Administrators)` is true. A UAC-filtered token has Administrators as
  deny-only, so it gets `read` only. If impersonation or `OpenThreadToken` fails (e.g. an anonymous-level client) the
  client stays `read` — **fail closed**. If `RevertToSelf` fails the process terminates.
- The PID (`GetNamedPipeClientProcessId`) is recorded for the audit log only and never used for authorisation. Good.
- Identity is fixed per connection; a client cannot gain `admin` later on the same connection.
- Test: `test/core-ipc.js` → "admin commands from a non-elevated client" starts a `runas /trustlevel:0x20000` child
  (Administrators deny-only) and expects `forbidden` for `setMode`, `rollbackAll`, `approve`, `disablePolicy`,
  `exportDiagnostics`, while `status` still works. If the runner cannot create such a client it prints
  "not tested: …" (never silent). **Not yet run** at the time of writing.
- Residual: while S1 is open the Electron app runs elevated, so for the UI "admin" means "OpenSweep is open". A
  compromised renderer is still contained by P7 (it can only name fixed methods), but a compromised main process is
  an administrator anyway.

### P2 — Server identity / pipe squatting (open, Medium)

`FILE_FLAG_FIRST_PIPE_INSTANCE` stops squatting only while the service runs. When it is stopped, any local user can
create `\\.\pipe\OpenSweepCore.v1`. `src/core-client.js` does not verify the server, so a squatter could:
feed the dashboard false incidents/recommendations, receive foreground-process hints (PIDs only), and receive admin
requests (`setMode`, `approve` …) — which it cannot act on. It **cannot** make OpenSweep run anything: the renderer can
only call fixed methods, every admin action is confirmed natively, and no Core result is executed.
*Recommendation:* verify the server before sending anything: `GetNamedPipeServerProcessId` on the client handle and
compare with the `OpenSweepCore` service PID from `QueryServiceStatusEx` (or check the server token is SYSTEM).
Node cannot call this without a native addon; options are a tiny signed helper, or doing it in the service's
installer-provided broker later (R7). Until then the dashboard's data is only as trustworthy as the pipe's owner.
`hello.pid` is self-reported by the server and is **not** a check.

### P3 — Service account and privileges (code-reviewed; verify on Windows)

- Installed as LocalSystem with `SERVICE_CONFIG_REQUIRED_PRIVILEGES_INFO` = `SeChangeNotifyPrivilege`,
  `SeImpersonatePrivilege` (for client identification), `SeCreateGlobalPrivilege`; everything else (SeDebug,
  SeTcb, SeBackup, SeLoadDriver, …) is removed from the process token. Unrestricted service SID
  (`NT SERVICE\OpenSweepCore`) is set so resources can be ACL'd to the service.
- *Verify on Windows:* (a) the actuator can still open background processes of other users with
  `PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION` without SeDebug (expected, SYSTEM is in default
  process DACLs; protected processes must fail and be skipped); (b) the collectors' ETW sessions start with this
  privilege set (kernel providers may need `SeSystemProfilePrivilege`). If a privilege must be added, add only that
  one and record why.
- *Recommendation (later):* evaluate running as `NT AUTHORITY\LocalService` plus the specific rights needed, once the
  actuator's access requirements are measured. Not possible to decide without Windows measurements.

### P4 — DLL search path and image load (code-reviewed)

`main.cpp` calls `SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32)` and `SetDllDirectoryW(L"")` first, sets
`ProcessImageLoadPolicy` (no remote images, no low-label images) and `ProcessExtensionPointDisablePolicy`. Statically
imported DLLs are KnownDLLs/System32. The binary must live in an admin-only directory (`Program Files` per-machine
install, R6) — **not yet true for per-user installs** (see S5). Electron side: every helper is started by absolute path
(`powershell.exe` under `%SystemRoot%`, `sc.exe` under System32). The new foreground-hint script defines its P/Invoke
in memory with `Reflection.Emit` (no compiled DLL is written to `%TEMP%`, unlike `Add-Type`).

### P5 — Configuration validation (contract; not verified on Windows)

Contract (`phases-2-8.md`): `config.json` validated on load, atomic writes (temp + flush + rename), append-only files
with per-line checksums, corrupt lines skipped and counted. `journal.cpp` / `incident.cpp` provide `from_json` that
returns `nullopt` on garbage (code-reviewed, Linux unit tests). Not verified on Windows: rename atomicity on NTFS with
the file open by a reader, behaviour when the directory ACL has been tampered with (the log code refuses untrusted
owners and reparse points — the engine's storage should use the same `ensure_secure_directory`), and a full disk.
*Recommendation:* add a "malformed config" case to the Windows integration test (write garbage as admin, restart the
service, expect `monitor` mode and a recorded event).

### P6 — Journal, trace, incidents and diagnostics files (code-reviewed)

`%ProgramData%\OpenSweep` and `\core` are created with `D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)` and re-secured if they exist
with a trusted owner and no reparse point. Files inherit it, so **standard users cannot read** the journal/trace/
diagnostics directly; they read through the pipe (`read` capability). This is stricter than the "Users read" in the
contract's storage table — the safer choice; the table should be updated by the owner of that document.
`exportDiagnostics` writes into the same directory (admin-only) and returns the path; the Electron app does not open it.
*Not verified on Windows:* that every engine file is created inside that directory (no fallback to `%TEMP%`).

### P7 — Renderer argument validation (unit-tested)

- The renderer can only name methods. `src/core-validate.js` holds a frozen table mapping 14 renderer methods to the 14
  contract commands; args are **rebuilt** (unknown keys dropped): `mode` ∈ {monitor, balanced, advanced}, `range` ∈
  {30m, 24h, 14d}, `metric` from a fixed list of Sample fields, `limit` integer 1–500, ids = positive safe integer or
  `^[A-Za-z0-9][A-Za-z0-9_.:-]{0,63}$`. `hello`/`health`/`metrics` and `reportForeground` are not reachable with
  renderer-supplied args. `test/core-validate.js` (200 checks) covers accept/reject cases incl. `__proto__`.
- Admin methods: the main process first looks the target up (`policies` / `heal`) — an unknown id fails before
  anything is sent — then shows `dialog.showMessageBox` stating exactly what happens. Advanced mode additionally needs
  the dialog's consent checkbox. The dialog text is plain text and truncates service-provided strings to 300 chars.
  The renderer's own `confirmActions` setting cannot skip it.
- Renderer: `contextIsolation`, `sandbox`, CSP `default-src 'self'; style-src 'self'` — the dashboard sets styles only
  through `element.style`/attributes; SVG charts are built from numbers only; all service strings are inserted as text
  nodes (no `innerHTML` with service data).

### P8 — Foreground hint spoofing (open design risk, Low–Medium)

`reportForeground {pid}` is accepted from any `read` client; the service validates that the PID exists in the caller's
session. A local process in the same session could therefore claim another process is in the foreground. Effects:
(a) shield a process from policies (it is treated as foreground), or (b) make the real foreground app look like
background, so that in Balanced mode it could receive EcoQoS/lower memory priority until verification (≤ 60 s) rolls it
back. Cross-session influence is blocked by the session check. *Mitigations to consider:* accept hints only from a
client whose token user equals the session's interactive user (`WTSQueryUserToken` comparison), require the PID to
own a visible top-level window (checkable only from that session — another reason for a per-session helper), rate-limit
hints, and keep the verification window + automatic rollback (already in the contract). The hint is never used to
*grant* anything.

### P9 — Race conditions

- **PID reuse:** the actuator records `process_start_ms` and compares the creation time of the opened handle before
  apply/restore (`actuator.cpp`, code-reviewed); since the comparison uses the same handle that is then used for the
  change, the classic check-then-open race is closed. *Not verified on Windows.*
- **Crash during apply:** write-ahead journal (intent → applied → verified/rolled_back) and rollback on start-up/stop
  (contract; portable tests). *Not verified on Windows* (needs an abrupt-termination test in a VM, Part 16).
- **Concurrent admin clients:** two clients can issue `setMode`/`disablePolicy` concurrently. The engine must serialise
  commands that change state with the engine tick. *Verify* in the integration test (parallel `setMode` from two
  connections, final state consistent and journaled once each).
- **Electron:** `coreAdmin` reads the policy list, shows a dialog, then sends the command — the policy may have been
  rolled back meanwhile; the service then reports `restored:false`, which the UI shows. No harm.
- **Benchmark memory helper:** bounded to leave ≥ 10 % RAM free, self-terminates after 3 minutes.

### P10 — Logging and privacy (code-reviewed)

Service log: client PID, SID, command name, error codes — no arguments, no window titles, no command lines. Samples
carry image names only (`test/core-ipc.js` asserts that `sample`, `events` and the diagnostics bundle contain no
title/command-line fields). *Risk:* Event Log messages (≤ 200 chars) can include full paths with the user name, e.g.
a faulting application path under `C:\Users\<name>\`. *Recommendation:* reduce paths to image names in the event
collector. Diagnostics bundles stay local (admin-only folder); the user decides whether to share them.
The Electron app adds History entries for Core admin actions (`settings.json`, user profile) — names and modes only.

### P11 — Update, signing, app privilege: S1, S5, S6 still open (High, inherited)

- **S1** — the whole Electron app runs as administrator (`requireAdministrator`). Core does not change this yet; the
  plan is R7–R9 (standard-user UI, typed broker commands in the service).
- **S5** — elevated logon task may start a user-writable executable (per-user install path). Installing a SYSTEM
  service from such a location makes this more important: the service binary must be under `Program Files`.
- **S6** — builds are unsigned, auto-update accepts pre-releases silently. The service binary is also unsigned. Code
  signing (and `publisherName` verification in electron-updater) is a prerequisite before recommending Balanced or
  Advanced mode to users, and before any driver discussion (see `driver-justification.md`).

### P12 — Memory safety of the C++ service

Bounded framing (64 KiB requests, 1 MiB responses, zero/oversize frames rejected — CI-verified), a hand-written JSON
parser with unit tests, 20 random-garbage frames in the integration test. *Missing:* coverage-guided fuzzing of the JSON
parser, framing and every command's argument handling. *Recommendation:* a libFuzzer target for the portable layer in
the Linux job (clang `-fsanitize=fuzzer,address,undefined`), seeded from the protocol tests; ASan/UBSan builds of the
portable unit tests.

### P13 — Benchmark harness

`bench/run.ps1` is a developer tool: requires an elevated prompt and an explicit `YES`, never touches windows it did not
open, refuses to start if Notepad/Calculator/Paint are open, kills only its own helper processes, downloads nothing, and
writes results without user or computer name. DiskSpd is used only if the user provides it.

## 3. Security tests status (Part 16)

| Test | Where | Status |
|---|---|---|
| Oversized / zero / huge-declared frames, malformed JSON, wrong version, unknown command | `test/core-ipc.js` | CI-verified (Phase 1) |
| 9th client, slow-loris, 20 garbage frames | `test/core-ipc.js` | CI-verified (Phase 1) |
| Admin commands from a non-elevated client → `forbidden` | `test/core-ipc.js` (`runas /trustlevel`) | Written; not yet run |
| Invalid args for new commands (`history`, `incidents`, `setMode`, `disablePolicy`, `reportForeground`) → `bad_args` | `test/core-ipc.js` | Written; not yet run |
| Privacy of `sample`, `events`, diagnostics bundle | `test/core-ipc.js` | Written; not yet run |
| Renderer argument validation | `test/core-validate.js` | Passing (Linux) |
| Pipe squatting while stopped | — | Not tested (P2 open) |
| Malformed `config.json`, corrupt journal lines on Windows | — | Not tested (P5) |
| Abrupt termination during an applied policy → rollback on restart | — | Not tested (VM only, Part 16) |
| Coverage-guided fuzzing | — | Not implemented (P12) |
