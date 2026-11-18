# OpenSweep — master prompt (v0.3.0 → v0.4.0)

You are continuing work on **OpenSweep** (this repo): a free, MIT, open-source Windows 10/11 PC-care app.
Stack: Electron + vanilla JS UI (`src/renderer/app.js`, `styles.css`), main process with strict IPC validation
(`src/main.js`: the renderer passes only IDs; paths/commands come from main-side caches), Windows backend =
PowerShell 5.1 scripts (`src/backend/windows.js`), demo backend (`src/backend/mock.js`, `npm run demo`),
everything data-driven from `src/backend/catalog.js`. Read `README.md`, `docs/RESEARCH.md` and the catalog first.

The app runs as administrator. Every change is read back from Windows and reported (applied ✓ / failed + reason).
Registry writes that Windows denies to admins are retried as SYSTEM (`asSystem()` in `windows.js`).
Auto-update comes from GitHub releases (`electron-updater`); the **Release** workflow builds and publishes.
The **Windows test** workflow (`test/windows-smoke.js`) runs the real backend on a Windows VM on every push.

The user is not a developer and prefers short, plain explanations. They want lots of options and
Windows "BS" (AI, ads, nags, telemetry, forced restarts) gone.

## Ground rules

- **Research competitors only from public sources**: websites, docs, feature lists, changelogs, reviews.
  Never decompile, disassemble or copy code or assets from Revo Uninstaller, TreeSize, WizTree, PC Manager or
  any other product. Write everything ourselves.
- **Safety first**: deletes go to the Recycle Bin; registry removals are exported to a `.reg` backup first;
  risky items are never pre-selected and always explained; offer a restore point before big changes.
- **Verify everything**: after any change, read it back and show ✓ or ✗ with the reason.
- **Low resource use**: background work runs at idle priority, rarely, and never blocks the UI.
- **Every feature works in demo mode** with realistic sample data (`mock.js`).
- **Extend `test/windows-smoke.js`** for every new backend function and keep the Windows test green.
- Plain, friendly UI text: short sentences, no jargon (explain any technical word in a tooltip).
- Commit in small steps with clear messages. Never put the user's personal email in commits.

## Part A — UI redesign (all pages)

Goal: a cleaner, calmer, more modern UI that is easier to read.

- **Layout**: more whitespace, consistent 8 px spacing scale, max content width ~1200 px, sticky page header
  with title, one-line description and actions on the right.
- **Typography**: Segoe UI Variable (fallback Inter/system); clear sizes (page title 24, section 16, body 14,
  meta 12); higher-contrast text; no walls of text — descriptions max 1–2 lines, details in an expandable "More".
- **Components**: one card style, one table style (sticky header, zebra-free, hover row, right-aligned numbers),
  one button style per role (primary / secondary / danger / ghost), pill tabs, consistent icons (inline SVG set,
  not emoji), skeleton loaders instead of spinners, friendly empty states, toasts with ✓ / ✗ icons.
- **Status indicator on every setting** (Optimize, Debloat, Updates, Security, Services, Tasks, Startup):
  a circle at the far right end of each row:
  - green circle with ✓ = applied (read back from Windows)
  - grey/red circle with ✗ = not applied
  - amber circle with ! = tried but Windows didn't keep it (hover shows the reason)
  - spinner while applying. The toggle sits just left of the circle.
- **Admin tags**: green with ✓ when OpenSweep is running as admin; amber when not.
- **Sidebar**: grouped, with icons, a compact mode, and a small status footer (admin ✓, version, update status).
- **Charts**: small, clean SVG charts (sparklines, bars, donut, treemap, line) with one shared colour palette that
  works in light and dark mode.
- Accessible: keyboard navigation, focus rings, colour is never the only signal (icons + text too).

## Part B — v0.3.0

### B1. Research write-up
Add to `docs/RESEARCH.md`: every feature Revo Uninstaller Pro and TreeSize Pro (and WizTree, BCUninstaller,
WinDirStat) advertise, plus common user complaints. Table: their feature → our version → how ours goes further.

### B2. "Clean everything" tab (one click)
- One big **Scan** button that checks everything in parallel: junk/temp, browser and app caches, Windows Update
  leftovers, Delivery Optimization, crash dumps, error reports, old driver packages in the driver store
  (`pnputil`; keep the newest of each), leftovers from uninstalled apps, empty folders, Recycle Bin,
  duplicate installers in Downloads, old Windows.old, thumbnail/icon caches.
- Result: one total ("You can free 23.4 GB"), grouped list with size and a tick per item; safe items ticked,
  risky ones unticked with a plain explanation.
- **Clean** → progress → report of what was freed (verified by re-measuring) → saved in History.
- Optional schedule (weekly/monthly, idle-only) via a scheduled task.

### B3. Space analyzer (Storage tab) — beyond TreeSize
- Scanner in a Node worker thread (no PowerShell), async with bounded concurrency, skips junctions/symlinks,
  counts access-denied folders, live progress (files, size, current folder), cancel.
  Stretch: when admin on NTFS, read the MFT for WizTree-class speed.
- Views: **treemap** (squarified, two levels, coloured by file type, click to drill in), **folder tree** with size
  bars / % of parent / files / folders / last modified, **file types**, **largest files**, **file age**,
  **duplicates**, **owners**.
- **Insights** with one-click actions: caches, `node_modules`, `.git` packs, old installers, ISOs, huge videos,
  hibernation file, empty folders, app leftovers.
- **Snapshots & history**: save scan summaries; show how folders grew ("Downloads +12 GB this month").
- **Search & filter**: name, size, date, type; act on results (Recycle Bin, move to another drive, NTFS compress).
- Export CSV/HTML. Keep the existing large-file finder, duplicate finder and Downloads organiser as tabs.

### B4. Already done on the branch
SYSTEM fallback for locked registry keys (fixes "Disable Widgets: Attempted to perform an unauthorized operation").

Release v0.3.0 when B1–B3 + Part A are done and the Windows test is green.

## Part C — v0.4.0

### C1. Uninstaller — beyond Revo
- One list: desktop programs, Store apps, browser extensions (Chrome/Edge/Brave/Firefox), Windows optional
  features. Size, install date, **last used**, publisher, rating (bloat/adware flags), update available (winget).
- **Deep uninstall**: run the vendor uninstaller (silent when supported), then scan leftovers: folders
  (Program Files, AppData, ProgramData), registry keys, services, scheduled tasks, startup entries, firewall rules,
  shortcuts, file associations, context-menu handlers. Three levels: safe / likely / careful.
  Backups: files → Recycle Bin, registry → `.reg` export; full undo from History.
- **Batch uninstall** (queue, silent where possible), **forced uninstall** (broken apps, by name/folder),
  **leftover scanner** for apps removed in the past.
- **Install tracker**: snapshot (files + registry + services + tasks) before an install, diff after, store the
  log; later remove exactly what that install added.
- **Hunter mode**: drag a crosshair onto any window/tray icon → identify the app → uninstall / stop startup / kill.

### C2. Reliability monitor (merge with Drivers & stability → "Reliability")
- Data from `Win32_ReliabilityStabilityMetrics` and `Win32_ReliabilityRecords` (+ the existing driver/event checks).
- Stability index line chart (1–10, 30 days, hover values); day columns with icons for app failures, Windows
  failures, misc failures, warnings (failed updates), information; click a day → plain-English details.
- **Top crashers** with actions: repair/reset app, check for updates, uninstall.

### C3. Live processes
- One long-running PowerShell reader streaming JSON lines every 2 s while the page is open (killed when you leave).
- CPU, RAM, disk I/O per app; grouped by app (expand for processes); search, sort; live CPU/RAM graphs.
- Flags: no publisher, unsigned, running from Temp/Downloads. Actions: end task, end app, open file location,
  set priority, search the name online.

### C4. Keep Windows optimal — settings guard
- Remember every tweak the user applied (desired state). On startup, after Windows updates, and on the hourly
  background tick, re-read; if Windows reverted something, re-apply it and notify
  ("Windows undid 3 settings — restored"). Toggle in Settings; list of what was restored in History.

Release v0.4.0 when Part C is done and the Windows test is green.

## Later ideas (ask the user first)
Driver updates via Windows Update, automatic restore points, scheduled maintenance, health doctor
("no sound" → runs the right fixes), fresh-PC setup wizard (debloat + tweaks + winget apps + defaults),
boot-time analyzer, privacy dashboard (camera/mic/location use), per-app firewall blocking, hosts-based ad blocking,
profiles (export/import a whole setup).

## Releasing
Bump `version` in `package.json` (and `npm install --package-lock-only`), merge to `main`, then run the
**Release** workflow (Actions → Release → Run workflow) or push a `vX.Y.Z` tag. Installed copies update themselves.
