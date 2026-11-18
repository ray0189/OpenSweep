# Feature research: Microsoft PC Manager vs OpenSweep

## Method

The public product page `https://pcmanager.microsoft.com/en-us` was loaded in a headless Chromium browser
and captured passively with **REA** (`inspect_web_page`, accessibility tree, evidence
`ev_8b182a974bdcc00c52fc54fdec7a3b16f4e365479a6cd69e3e7ca7da9806f303`, captured 2026-11-18).

PC Manager itself is a Windows-only app, and no Windows binaries or installation image were available on the
build machine. So this research covers the **advertised feature set**, not the app's internals. No Microsoft
code, assets or text are used in OpenSweep.

## Observed (from the REA capture)

| PC Manager feature (as advertised) | Advertised description (paraphrased) |
|---|---|
| Boost | Clean up the system, free space, boost performance |
| Health checkup | Find and fix issues fast; scan and clear threats with one click |
| Manage your storage | Large-file management; uses Windows Storage Sense |
| Pop-up management | Reduce ads and app pop-ups |
| Toolbox & Mini toolbar | Built-in Windows tools |
| Protection | "Safeguard your PC in a quiet and reliable way" |
| Compatibility | Windows 10 (19042+) and Windows 11 |

## Not observed

- Internal implementation (APIs, cleanup paths, scoring). Defining OpenSweep's behaviour came from documented
  Windows policies and registry values, not from PC Manager.
- Whether PC Manager removes Copilot/AI or preinstalled apps (not advertised on the page).

## OpenSweep coverage

| Area | PC Manager (advertised) | OpenSweep |
|---|---|---|
| Boost | ✓ | ✓ memory trim + temp + DNS, plus tray/widget one-click |
| Health check | ✓ | ✓ 15+ checks, each with a fix button and score |
| Junk cleanup | ✓ | ✓ 25 targets incl. 6 browsers, apps, dev caches |
| Large files | ✓ | ✓ plus duplicates, folder sizes, Downloads organiser |
| Storage Sense | ✓ | ✓ toggle in Optimize → Performance |
| Pop-up/ad control | ✓ | ✓ 11 ad/suggestion tweaks + 6 Edge tweaks |
| Toolbox & mini toolbar | ✓ | ✓ 40 tools + custom shortcuts + floating widget |
| Threat scan | ✓ | ✓ Defender quick/full scan, definitions, threat history |
| **Microsoft AI removal** | – | ✓ Copilot, Recall, Click to Do, Paint/Notepad/Edge AI, AI apps |
| **Debloat** | – | ✓ 39 apps, 15 services, 12 telemetry tasks |
| **Privacy** | – | ✓ 10 privacy tweaks, max-privacy preset |
| **Gaming / performance** | – | ✓ 15 tweaks, gaming preset, power plans |
| **Startup manager** | – | ✓ Run keys, startup folders, logon tasks |
| **Uninstaller** | – | ✓ desktop + Store apps |
| **Network tools** | – | ✓ DNS switcher, resets, latency test |
| **Repair** | – | ✓ SFC, DISM, chkdsk, WU reset, spooler, icons… |
| **Undo history** | – | ✓ |
| Open source / no telemetry | – | ✓ MIT |

## Next research step

With a Windows ISO (`install.wim`) or a Windows VM, REA can be used to check the shipped Windows image:
which AppX packages are provisioned, which services and scheduled tasks exist per build, and which policy keys
the AI components read. That would let the catalog be checked automatically per Windows build.

## Round 2: what people complain about (web research, 2026-11-18)

Sources: Windows Central, Windows Latest, Tom's Hardware, TWiT, Ars Technica (25H2 declutter guide), Make Tech Easier,
Pureinfotech, Winaero, Neowin, Eleven Forum, Microsoft Q&A. Each complaint is mapped to what OpenSweep now does.

| Complaint | OpenSweep fix |
|---|---|
| Updates restart the PC on their own | `NoAutoRebootWithLoggedOnUsers` policy, restart notifications, active hours |
| Can't pause updates long enough | Pause 7/14/35/90/365 days (`FlightSettingsMaxPauseDays` + pause timestamps) |
| Buggy updates (boot failures, black screens) | Uninstall a recent KB; lock to current feature version (`TargetReleaseVersionInfo`) |
| Windows Update replaces good drivers | `ExcludeWUDriversInQualityUpdate` |
| New Outlook / Dev Home come back after uninstall | Delete `UScheduler_Oobe\OutlookUpdate` and `DevHomeUpdate` |
| Microsoft account / OneDrive / M365 nags | `Start_AccountNotifications=0`, Settings `EnableAccountNotifications=0`, hide Settings Home (`SettingsPageVisibility=hide:home`) |
| Ads in Start "Recommended" | `Start_IrisRecommendations=0`, recent-files off, more-pins layout |
| Search Highlights / Bing in search | `IsDynamicSearchBoxEnabled=0`, search box → icon |
| Spotlight "Learn about this picture" icon | HideDesktopIcons `{2cc5ca98-…}` |
| File Explorer / Downloads slow to open | `FolderType=NotSpecified` (folder type discovery off), remove Gallery/Home/OneDrive from nav pane |
| Edge tabs flooding Alt+Tab | `MultiTaskingAltTabFilter=3` |
| Edge nags & desktop shortcut returns | Edge `DefaultBrowserSettingEnabled=0`, EdgeUpdate `CreateDesktopShortcutDefault=0`, promos/Rewards off |
| Sticky Keys pop-up in games | StickyKeys/ToggleKeys/FilterKeys hotkey flags off |
| Teams Chat icon, combined taskbar buttons, snap flyout, shake-to-minimise | Individual toggles |
| Start menu / Search broken, no sound, Bluetooth | One-click repairs |
| Windows.old & Reserved Storage eat disk space | One-click removal |
| Forced Microsoft account at setup | Out of scope for an installed app (setup-time bypass); link to Settings → switch to local account |

Not fixable from an app: Edge being forced for links opened from Widgets/Search outside the EEA, and Microsoft's
right to re-enable features in later updates. That's why the health check re-checks every setting each time it runs.

## Round 3: uninstallers and disk analyzers (public sources only, 2026-11-18)

Sources: the public product pages, feature lists, help pages and changelogs of Revo Uninstaller (Free/Pro),
TreeSize (Free/Professional), WizTree, WinDirStat, Bulk Crap Uninstaller and IObit Uninstaller, plus reviews
and forum threads (Reddit r/Windows, r/sysadmin, TenForums, MajorGeeks, PCWorld, How-To Geek).
**No software was decompiled, disassembled or copied** — only advertised features and user complaints.

### Uninstaller: what they advertise → what OpenSweep does

| Feature | Revo / BCU / IObit | OpenSweep 0.3 |
|---|---|---|
| Run the app's own uninstaller, then scan leftovers | ✓ (3 scan modes) | ✓ files, folders, registry, startup entries, services, scheduled tasks, desktop/Start shortcuts and firewall rules, sorted into *definitely / very likely / check first* |
| Backup before removing leftovers | Registry backup (Pro) | Files → Recycle Bin, registry/services/tasks → `.reg`/XML backup, one-click restore from History |
| Forced uninstall of broken apps | ✓ (Pro) | ✓ by name or folder |
| Batch uninstall | ✓ (Pro/BCU) | ✓ silent where the installer supports it (MSI, Inno Setup, NSIS, QuietUninstallString) |
| Install monitoring / logs | ✓ (Pro, real-time driver) | ✓ snapshot before/after an install; remove exactly what it added |
| Hunter mode (target a window) | ✓ | ✓ click any window → identify app → uninstall / end / startup |
| Leftovers of apps removed earlier | ✓ (Pro) | ✓ old-leftover finder (unowned folders not changed for 30+ days) |
| Browser extensions | ✓ (Pro) | ✓ Chrome, Edge, Brave, Vivaldi, Opera, Firefox, with risky-permission and **policy-forced extension** detection (common adware trick) |
| Windows apps (Store) | ✓ | ✓ plus 39-app debloat list |
| Last used / usage info | Partly | ✓ last run time from Windows Prefetch, "not used in 6 months" filter |
| Bloat/adware warnings | IObit only | ✓ trialware, promo apps, adware and scareware "optimisers" flagged |
| App updates | IObit (paid) | ✓ via winget, free |
| Windows optional features | – | ✓ on/off with friendly names |
| Price | Pro ≈ $25–40/yr | Free, MIT |

Common complaints we avoid: aggressive "leftover" scans that delete shared folders (we never pre-tick
publisher-level folders), no undo, nag screens/upsells, bundled offers, telemetry.

### Disk analyzer: what they advertise → what OpenSweep does

| Feature | TreeSize / WizTree / WinDirStat | OpenSweep 0.3 |
|---|---|---|
| Fast scan | WizTree/TreeSize Pro read the NTFS MFT | Parallel worker-thread scan (no UI freeze, live progress, cancel). MFT reading is a planned speed-up. |
| Treemap | ✓ | ✓ squarified, two levels, coloured by file type, click to drill in |
| Folder tree with size/%/files/date | ✓ | ✓ with lazy expand and right-click actions |
| File types / extensions | ✓ | ✓ by category and extension |
| File age | ✓ (Pro) | ✓ age buckets + "big files untouched for a year" |
| Largest files / search & filters | ✓ (Pro) | ✓ name, size, age and type filters |
| Duplicates | ✓ (Pro) | ✓ SHA-256 content match |
| History / compare scans | ✓ (Pro) | ✓ snapshots per location, biggest growth since last scan |
| Actions | Delete, move, compress (Pro) | Recycle Bin, move to another drive, NTFS compress, export CSV |
| "What can I delete?" | – | ✓ Insights: caches, node_modules, temp, crash dumps, Windows.old, old installers, big videos, disk images, empty folders |
| Owners / network drives | ✓ (Pro) | Not yet |
| Price | Pro ≈ $60+ | Free, MIT |
