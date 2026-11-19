# OpenSweep benchmarks (Part 14)

This folder is a **framework and methodology**. It contains no results. No benchmark number is written down
anywhere in this repository unless a run of `run.ps1` produced it, and then only together with the result file,
the run count, the hardware and the Windows build.

| File | Purpose |
|---|---|
| `run.ps1` | Harness. Puts the PC in one configuration, runs the workloads `-Runs` times, writes `results/<config>-<time>.json`. |
| `analyze.js` | Reads result files, prints a Markdown report: runs, n, median, p95, p99, bootstrap 95 % CI, Δ vs stock with its CI. |
| `../test/bench-analyze.js` | Unit test of the statistics on fixed synthetic data (runs in CI). |

## Configurations

| Name | What it means | How `run.ps1` sets it |
|---|---|---|
| `stock` | Windows without OpenSweep Core | `OpenSweepCore` service stopped; refuses to run if an `OpenSweepCore.exe` console instance is running |
| `monitor` | OpenSweep Core measuring only | service running, `setMode monitor` over the pipe |
| `active` | OpenSweep Core allowed to apply low-risk policies | service running, `setMode balanced` over the pipe |

Advanced mode is not a benchmark configuration: its actions need per-action approval, so they are not repeatable
unattended. **Process Lasso** (plan item 4) is not compared: no run has been made with it, and the harness does
not install or drive third-party tools. If a comparison is ever made, it must use the same machine, build and
workloads, follow the product's licence terms, and be reported as its own configuration with its own result
files — never with numbers taken from elsewhere.

The OpenSweep app itself (Electron UI) should be **closed** in every configuration, so that only the service
differs. `run.ps1` restores the service state and mode it found when it finishes.

## Workloads and metrics

| Workload id | Metric (Part 14) | How it is measured | Unit |
|---|---|---|---|
| `launch.<app>.cold` | Application launch time | Notepad, Calculator, Paint. First launch after closing the app and 30 s idle. Time from `Start-Process` until a **new** top-level window of the app exists and `WaitForInputIdle` returns (UWP apps may not support it; then the window time is used). **Not** a post-reboot cold start: the file cache is not flushed (OpenSweep never purges the standby list, and neither does the harness). | ms |
| `launch.<app>.warm` | Application launch time | 3 repeated launches 2 s apart | ms |
| `probe.idle.wakeupDelayUs` | Foreground responsiveness (baseline) | Overshoot of `Sleep(1)` with 1 ms timer resolution (`timeBeginPeriod(1)`), no load, 5 s | µs |
| `cpu.contention.wakeupDelayUs` / `…P99Us` | Foreground responsiveness, CPU ready time proxy | Same probe while N = logical-CPU-count busy background processes run (`-ContentionSeconds`, default 20 s). Raw delays (evenly sub-sampled to ≤ 2 000 per run) and the per-run p99. | µs |
| `cpu.contention.contextSwitchesPerSec` | Context-switch overhead | `System\Context Switches/sec`, 1 s samples during contention | /s |
| `cpu.contention.queueLength` | CPU ready time proxy | `System\Processor Queue Length`, 1 s samples | threads |
| `memory.pressure.launch.notepad` | Launch time under commit/RAM pressure | A helper fills RAM with random bytes until load ≈ `-MemoryTargetPercent` (default 85 %), never leaving < 10 % free; 3 warm Notepad launches | ms |
| `memory.pressure.wakeupDelayUs` | Responsiveness under memory pressure | Probe, 5 s | µs |
| `memory.pressure.pageReadsPerSec` | Hard page faults | `Memory\Page Reads/sec`, 10 × 1 s | /s |
| `disk.randrw4k.p50Ms` / `p99Ms` | Storage latency | Microsoft DiskSpd: 1 GiB file in `%TEMP%`, 4 KiB random, 30 % writes, 2 threads × 8 outstanding, caching off (`-Sh`), 20 s + 3 s warm-up, latency percentiles from `-Rxml`. **Skipped with an explicit message if `diskspd.exe` is not installed.** | ms |
| `idle.coreCpuPercent` | Idle CPU overhead of the service | `OpenSweepCore.exe` CPU time delta per 10 s / (10 s × logical CPUs). `-IdleMinutes` in total (default 10), split across runs. `monitor`/`active` only. | % |
| `idle.coreWorkingSetMB`, `idle.corePrivateMB` | Background RAM usage | Working set / private bytes of `OpenSweepCore.exe` every 10 s | MB |

Not automated (documented only):

- **Gaming FPS, 1 % lows, frame-time variance.** Capture with [PresentMon](https://github.com/GameTechDev/PresentMon)
  (external, installed by the user), e.g. `PresentMon-x64.exe --process_name game.exe --timed 120 --output_file run1.csv`
  over a fixed, repeatable in-game benchmark or replay, ≥ 5 runs per configuration. FPS = 1000 / mean(`MsBetweenPresents`);
  1 % low = 1000 / p99(`MsBetweenPresents`); frame-time variance = variance of `MsBetweenPresents`. Convert the per-run
  values into a result file with workload ids such as `game.<title>.fps` (`"better": "higher"`) and `game.<title>.frametimeP99Ms`
  to analyse them with `analyze.js`. **No such numbers exist yet.**
- **Crash frequency and recovery success rate.** These come from long-running use, not a benchmark: OpenSweep Core's
  journal (`journal` command: recovery transactions and their final state) and incidents over weeks. Report counts
  with the observation period; do not extrapolate.
- **Process Lasso** — see above.

## Methodology

1. **Controlled environment.** Same machine, same Windows build (the harness records it), power plan and AC power
   recorded, no other apps open, no user input during a run, Windows Update not installing (pause it), antivirus scans
   not scheduled during the run. Prefer a dedicated test install or VM snapshot for memory and contention workloads.
   Reboot before each configuration so every configuration starts from the same state.
2. **Repeatable workloads.** The workloads above are scripted and parameterised; parameters are stored in each result
   file. Workloads are interleaved per run (launch → contention → memory → disk → idle) so slow drift (thermal,
   background maintenance) affects all workloads similarly.
3. **Multiple runs.** At least **5 runs** per configuration (`-Runs 5`, the default). `analyze.js` warns below 5. Run
   the configurations in alternating order across days if possible (e.g. stock, monitor, active, active, monitor, stock).
4. **Statistics.** For each workload and configuration: run count, sample count n, **median, p95, p99** (linear
   interpolation, Hyndman–Fan type 7) and a **95 % percentile-bootstrap CI of the median** (2 000 resamples, fixed
   seed so the report is reproducible). Differences are reported as Δ median vs `stock` with a bootstrap CI of the
   difference; a CI that includes 0 means "no measurable difference". Raw probe delays within a run are not
   independent, so for those workloads the per-run summary (`…P99Us`) is the primary metric and the pooled CI should be
   read as optimistic.
5. **Context in every report.** Hardware (CPU, cores, RAM, disks, system model, hypervisor present), Windows edition,
   build and UBR, PowerShell version, OpenSweep Core version and mode, harness parameters — all captured automatically.
   No user name or computer name is recorded. `analyze.js` warns when files come from different machines or builds.
6. **Honesty rules.** Never fabricate or round towards a claim. Report "no measurable difference" when the CI includes 0.
   Report skipped workloads (they are listed in each result file and in the report). Do not compare against numbers
   from other machines, other builds or other people's publications.

## Running

From an **elevated** PowerShell on the test PC (Windows PowerShell 5.1 or PowerShell 7):

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\bench\run.ps1 -Config stock   -Runs 5 -DiskSpd C:\Tools\diskspd\amd64\diskspd.exe
.\bench\run.ps1 -Config monitor -Runs 5 -DiskSpd C:\Tools\diskspd\amd64\diskspd.exe
.\bench\run.ps1 -Config active  -Runs 5 -DiskSpd C:\Tools\diskspd\amd64\diskspd.exe
node bench\analyze.js bench\results\*.json --out bench\results\report.md
```

Options: `-Workloads launch,contention` to run a subset, `-IdleMinutes`, `-ContentionSeconds`, `-MemoryTargetPercent`,
`-CoreExe <path>` (install the service if missing), `-OutDir`, `-Force` (no confirmation prompt).

Safety: the harness refuses to start if Notepad, Calculator or Paint are already open, and only ever closes windows it
opened itself. The memory helper and busy processes are killed in `finally` blocks; the memory helper also exits by
itself after 3 minutes. DiskSpd's 1 GiB test file is deleted afterwards.

## Initial targets (aspirational, Part 14)

Native service RAM 15–25 MB; idle CPU < 0.1 % average; no measurable foreground responsiveness regression in
`monitor`. These are **targets to validate**, not results. The integration test `test/core-ipc.js` asserts only a
loose bound (working set < 64 MB, ≤ 20 % growth over a 60 s soak on the CI runner) and prints the measured values.

## Status

- `analyze.js` statistics: unit-tested on Linux and Windows CI (`test/bench-analyze.js`).
- `run.ps1`: parse-checked with Windows PowerShell 5.1 in CI (`core.yml`); its result-building helpers were exercised
  with PowerShell 7 on Linux during development. **It has not been run end to end on a Windows PC yet**, so no
  results exist.
