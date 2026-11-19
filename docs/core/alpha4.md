# 0.4.0-alpha.4 — Hardware check, Game boost, Scorecard (contract)

New sidebar page **Performance** (Overview group, icon `zap`/`activity`), file `src/renderer/perf-page.js`,
with three tabs: **Hardware check**, **Game boost**, **Scorecard**.

## Ownership
| Part | Files | Owner |
|---|---|---|
| Hardware check backend + tab | `src/backend/hardware.js`, `src/api-hardware.js` (exports `(ctx) => ({ hwCheck, hwFix })`), `src/renderer/perf-hardware.js` (exports `window.PERF_HARDWARE(container)`), mock in `src/backend/mock-hardware.js`, test additions in `test/windows-smoke.js` (new step only) | hw agent |
| Core: game boost engine + scorecard | `core/**` | core agent |
| Page shell, Game boost + Scorecard tabs, main.js wiring (incl. requiring `./api-hardware`), core-validate, mock for boost/scorecard, nav entry, index.html script tags, test/core-ipc.js additions | `src/main.js`, `src/core-validate.js`, `src/renderer/perf-page.js`, `src/renderer/app.js` (NAV only), `src/renderer/index.html`, `src/backend/mock.js`, tests | app agent |

## 1. Hardware check (app, PowerShell, read-only unless the user presses a fix)
`hwCheck()` → `{ checks: [{ id, title, status: 'ok'|'warn'|'bad'|'unknown', found, expected, why, fix: { kind: 'button'|'steps'|'link', label, steps?: [str], url?: 'ms-settings:...' } }] }`
Checks (documented sources only):
- `ram_speed`: Win32_PhysicalMemory `Speed` (rated) vs `ConfiguredClockSpeed` (running). Lower → warn
  "XMP/EXPO likely off" + BIOS steps. Single-channel (1 stick, or sticks in one channel when detectable) → warn.
- `refresh_rate`: per monitor, current vs highest supported refresh at the current resolution
  (EnumDisplaySettings via in-memory P/Invoke like FG_SCRIPT; no DLL on disk). Lower → warn; fix =
  button that sets the highest rate for that monitor via ChangeDisplaySettingsEx after a confirm,
  with automatic revert if the user doesn't confirm within 15 s (like Windows' own dialog), or `ms-settings:display-advanced` link.
- `pcie_gpu`: Get-PnpDeviceProperty DEVPKEY_PciDevice_CurrentLinkSpeed/Width vs MaxLinkSpeed/Width for display adapters (note: GPUs downclock the link at idle — only warn on width, report speed as info).
- `ssd`: per fixed volume: free space < 15% → warn; TRIM enabled (`fsutil behavior query DisableDeleteNotify`); media type via Get-PhysicalDisk. Fix button: `Optimize-Volume -ReTrim` (admin).
- `power`: active power plan + Windows power mode (on battery / battery saver). Fix: switch plan (button).
- `thermal`: from Core when running (sustained cpuFreqPercent low while busy, Thermal events), else Kernel-Power 86/125 / Win32_Processor CurrentClockSpeed vs MaxClockSpeed under load; `unknown` if no evidence.
- `drivers_age`: display adapter + chipset/storage driver dates (Win32_PnPSignedDriver DriverDate) — older than 12 months → warn with vendor link (NVIDIA/AMD/Intel by manufacturer).
- `game_mode`, `hags`: Game Mode and Hardware-accelerated GPU scheduling registry state (info + toggle via existing tweak ids if present).
`hwFix({ id, target? })` performs only the fixes listed above, with ids validated in main.

## 2. Game boost
- App side: setting `gameBoost` (default **off**), toggle on the tab. The existing foreground helper
  also reports `fullscreen` using `SHQueryUserNotificationState` (QUNS_RUNNING_D3D_FULL_SCREEN = 3,
  QUNS_BUSY = 2, QUNS_PRESENTATION_MODE = 4) → sends `reportForeground { pid, fullscreen }`.
- When boost starts (fullscreen ≥ 5 s and gameBoost on): app switches to the High/Ultimate performance
  plan (remembering the previous one), pauses OpenSweep's own background scans; Core starts a **boost
  session**. When fullscreen ends (≥ 10 s) or the app/Core stops: everything restored. History entries
  for start/stop. A small banner/toast "Game boost on".
- Core side: IPC `setBoost {enabled}` (admin) persists `boost` in config.json; `boost` (read) →
  `{ enabled, active, since, foregroundPid, foregroundName, throttled:[{ pid, name, kind }], restored, lastSession:{ start, end, throttledCount } }`.
  While active, Core applies EcoQoS (and memory priority 2) to verified **background** processes using
  noticeable CPU (≥ 1 % avg over the last 30 s) — same protection list, never the foreground app/its
  children, audio, anti-cheat, security, system. Bounded (≤ 16 targets), journaled as policy
  transactions with kind `boost`, all rolled back on boost end, mode change, service stop, and crash
  recovery. Works in any mode (it is an explicit user opt-in), but Monitor-mode users see it as a
  separate switch. `reportForeground` accepts optional `fullscreen:boolean`.

## 3. Scorecard
- Core IPC `scorecard` (read) → `{ baseline: { capturedAt, metrics } | null, current: { metrics }, days }`
  where `metrics` = `{ bootMs (median of last boots from Diagnostics-Performance 100), idleCpuPercent,
  idleRamUsedBytes, diskLatencyMs, hardFaultsPerSec, stutterPerHour (DPC/ISR ≥ 2000 µs windows),
  crashesPerWeek }`, each `null` when not measured. Idle = samples with no foreground input... use
  cpu < 15% and no fullscreen; documented in code.
- `captureBaseline` (admin) stores the current 24 h numbers as "before" in config/scorecard.json;
  captured automatically 24 h after first start if none exists.
- UI: Before / Now / change table with green/red arrows, "not enough data yet (n h)" states, no
  invented numbers, Simulated badge in demo.

## Honesty
Nothing is claimed as faster without Before/Now numbers. Fixes that need the BIOS are instructions only.
