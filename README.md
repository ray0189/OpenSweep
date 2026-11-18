# OpenSweep

A free, open-source PC care toolkit for **Windows 10 and 11**: cleanup, boost, startup control,
debloat, Microsoft AI removal, privacy, tweaks, security, network and repair, all in one app.
No telemetry, no ads, no account.

> OpenSweep is an independent project. It is not affiliated with or endorsed by Microsoft.

## Features

| Area | What it does |
|---|---|
| **Home / Health check** | One-click score covering disk space, junk, startup load, Defender, firewall, updates, SMB1, AI/ad settings and memory, with a fix button for each issue. Live CPU/RAM and **Boost** (trims app memory, clears old temp files, flushes DNS). |
| **Cleanup** | 25 targets: user and Windows temp, Recycle Bin, thumbnails, Windows Update and Delivery Optimization caches, error reports, crash dumps, logs, shader caches, recent-files list, Chrome/Edge/Brave/Opera/Vivaldi/Firefox caches, Discord/Spotify/Teams/Steam, and VS Code/npm/pip/NuGet developer caches. |
| **Optimize** | 97 reversible registry/policy tweaks in 8 categories (AI & Copilot, Ads & pop-ups, Privacy, Performance, Gaming, Explorer & taskbar, Edge, Windows Update), plus 5 presets: **Fix Windows annoyances**, Recommended, AI-free, Gaming, Maximum privacy. |
| **Windows Update** | Stop forced restarts, pause 7 days–1 year, active hours, lock to your current version (security fixes only, no feature upgrades), block driver updates, stop new Outlook/Dev Home reinstalling, and uninstall a bad update. |
| **Debloat & AI** | "Remove all Microsoft AI": turns off Copilot, Recall, Click to Do, generative-AI app access, and AI in Edge, Paint, Notepad and Search, then uninstalls the AI apps. Removes 39 known preinstalled apps (optionally for all users), sets 15 telemetry/unneeded services, and disables 12 telemetry scheduled tasks. |
| **Startup apps** | Registry Run keys (user, machine, 32-bit), startup folders and logon scheduled tasks, each with an on/off switch (uses Windows' own StartupApproved flags, the same as Task Manager). |
| **Installed apps** | Desktop and Store apps with search, sort by size and uninstall. |
| **Processes** | Live, grouped by app, sortable by CPU/RAM. Critical Windows processes are protected. |
| **Storage** | Drive usage, large-file finder, duplicate finder (SHA-256 content match), folder sizes, Downloads organiser. Deletions go to the Recycle Bin. |
| **Security** | Defender status, quick/full scan, definition update, firewall, Windows Update, UAC, SmartScreen, BitLocker, RDP and SMB1, with one-click fixes, plus recent threat history. |
| **Network** | Adapters, DNS switcher (Cloudflare, Google, Quad9, AdGuard, family filter), flush DNS, renew IP, hosts editor, latency test. |
| **Repair & power** | Fix Start menu/taskbar, rebuild Search index, fix sound, fix Bluetooth, delete Windows.old, turn off Reserved Storage, restore point, SFC, DISM repair and component cleanup, chkdsk, network reset, Windows Update reset, print spooler fix, time sync, Store reset, Explorer restart, icon cache rebuild, hibernation on/off, Ultimate Performance plan, battery report, power plan switcher. |
| **Toolbox** | 43 Windows tools and settings pages, plus your own shortcuts. |
| **History & undo** | Every change is logged, and tweak, startup and task changes can be undone. |
| **Mini toolbar & tray** | Floating always-on-top CPU/RAM widget with Boost; tray menu; start with Windows. |

## Install (Windows)

Download `OpenSweep Setup x.y.z.exe` (installer) or `OpenSweep-x.y.z-win.zip` (portable) from Releases.
The builds are not code-signed yet, so SmartScreen will warn: choose **More info → Run anyway**.

## Build

Works on macOS, Linux or Windows (Node.js 20+):

```bash
npm install
npm run demo        # run the UI with sample data (any OS, changes nothing)
npm start           # on Windows: run for real
npm run dist:win    # build Windows x64 installer + zip into dist/
npm run dist:win-arm64
```

## How it works

- `src/backend/catalog.js` is a declarative list of every tweak, cleanup target, app, service, task, tool and repair.
  **Most contributions are a single entry here.**
- `src/backend/windows.js` runs everything with the PowerShell 5.1 that ships with Windows. Scripts are written to a
  temp `.ps1` and arguments are passed as JSON, so nothing user-controlled is ever spliced into a command line.
  Actions needing admin rights trigger a normal UAC prompt for that action only.
- `src/backend/mock.js` provides realistic sample data for `--demo` and for non-Windows development.
- `src/main.js` validates every request from the UI. The UI passes only IDs, and commands, paths and uninstall
  strings are looked up from data the main process produced itself.

## Safety

- Cleanup only touches cache/temp locations listed in the catalog. Files in use are skipped.
- Large/duplicate file deletion is limited to files from your last scan and goes to the Recycle Bin.
- Tweaks use documented Windows policy/registry values. "Off" restores the Windows default.
- **Create a restore point** (Repair & power) before applying presets or debloating.

## Licence

MIT. See [LICENSE](LICENSE).
