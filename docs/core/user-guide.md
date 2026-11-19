# OpenSweep Core — user guide

OpenSweep Core is an optional background service that comes with OpenSweep for Windows. It watches how your PC
behaves, learns what is normal **for your PC**, tells you when something unusual happens, and — only if you allow
it — makes small, reversible changes to background apps. You find it under **Protect → OpenSweep Core**.

You do not need it: OpenSweep works the same without it. You can stop or uninstall it at any time from that page.

## What it does

- **Measures** CPU, memory, commit charge (how much memory programs have reserved), storage latency (how long the
  disk takes to answer) and hard page faults (when Windows has to read memory back from disk), every 2 seconds.
- **Learns your PC's normal range.** For each measurement it keeps a usual value and a typical spread. The charts show
  this as a shaded band. Until it has about 4 minutes of data (120 samples), a measurement shows "learning (n/120)" and
  is not used for decisions.
- **Reports incidents** when something stays outside its normal range or Windows logs repeated problems — for example
  memory that keeps growing, or a service that keeps crashing. Each incident lists the symptoms, the measurements
  (evidence), what happened at the same time, how confident it is, the risk and what you can do.
- **Recommends** small changes, and explains each one: what, why, what it expects to happen, and what would block it.
- **Applies** those changes only in Balanced or Advanced mode (see below), measures whether they helped, and undoes
  them automatically if they did not.
- **Keeps a journal** of every change and every recovery action, with the original value, so everything can be undone.
- Shows a **stability score** (0–100) with the reasons that lowered it. It describes stability, not speed.

## What it never does

- It never changes anything in **Monitor** mode, which is the default.
- It never touches the app you are using (the foreground app), Windows system processes, security software (for
  example Microsoft Defender), apps playing audio, or programs it cannot identify.
- It never ends (kills) programs, never empties other programs' memory, never clears Windows' file cache and never
  pins programs to specific CPU cores.
- It never acts only because CPU or memory use is high. High use is normal when you are working or playing; it only
  considers a change when a background program is measurably slowing down the one you are using, for a sustained time.
- It never restarts or repairs third-party apps or services on its own. Those actions wait for your approval.
- It never turns off or bypasses Windows security features, and never installs a kernel driver.
- It never reads what you type, window titles, command lines, document or browser contents, or network traffic.
- It never sends anything over the internet. Everything stays on your PC (`%ProgramData%\OpenSweep\core`, readable only
  by administrators). "Export diagnostics" writes a file there; sharing it is up to you.

## Modes

| Mode | What happens | When to use it |
|---|---|---|
| **Monitor** (default) | Measures, explains and recommends. What it *would* do is shown as **simulated** — nothing is changed. | Always a safe choice. Start here and look at the recommendations for a few days. |
| **Balanced** | May apply only low-risk, reversible changes to verified **background** programs: Windows efficiency mode (EcoQoS), lower memory priority, below-normal priority. Each change is written to the journal first, measured for 60 seconds and undone if it did not help. Limits: at most 3 changes per 5 minutes, and a 10-minute pause per program after a change. | If the recommendations in Monitor mode made sense to you and you want them applied. |
| **Advanced** | Adds experimental changes (for example moving background work to efficiency cores on CPUs that have them). **Each one needs your separate approval.** Same journal, verification and automatic undo. It does not bypass any Windows protection. | Only if you are comfortable reviewing each change. If anything feels slower, use "Roll back everything". |

Changing the mode needs OpenSweep to run as administrator. OpenSweep asks you to confirm in a Windows dialog that
explains the change; Advanced mode also asks you to tick a consent box. Switching back to Monitor undoes every change
that is currently applied.

## Reading the dashboard

- **Overview:** stability score and its reasons, engine status, live measurements with the usual range (shaded band and
  dashed median line). Switch the range between 30 minutes, 24 hours and 14 days. Recent incidents are listed below.
- **Incidents:** click one to open its details. "Correlated with" shows what changed **at the same time** (with a
  correlation value or confidence); it is not proof of what caused the problem.
- **Optimisation:** the mode switch, recommendations ("would apply" or "blocked" with the reason), active changes with a
  **Disable** button each, **Roll back everything**, and the history of changes with their measured outcome.
- **Recovery:** whether automatic recovery is available (it pauses itself — "circuit breaker open" — after repeated
  failures), actions waiting for your approval with an **Approve** button, and the history of recovery actions.
- **Events:** crashes, hangs, service failures, hardware and update events collected from the Windows logs.
- **System:** the service's own memory and CPU use, how long each engine cycle takes, your processor layout (cores,
  NUMA, CPU sets) and **Export diagnostics**.

Anything marked **Simulated** is not a measurement of your PC: either OpenSweep runs in demo mode (every number is made
up to show how the page works), or it is a decision the engine only simulated in Monitor mode.

## If something seems wrong

1. Open **Optimisation** and press **Roll back everything**. Every applied change is restored to its original value.
2. Switch to **Monitor** mode.
3. If needed, **Stop** or **Uninstall** OpenSweep Core from the top of the page. Stopping the service also undoes any
   applied change, and after a crash the service undoes leftover changes the next time it starts.

## Honest limits

- OpenSweep does not claim that Core makes your PC faster. Any effect is measured per change (60-second verification)
  and shown in the history. Benchmarks are described in `bench/README.md`; no results have been published yet.
- The Windows parts have been tested on GitHub's Windows test machines, not yet on a wide range of real PCs.
- While OpenSweep runs as administrator (see `security-audit.md`, S1), "administrator" for Core means "the OpenSweep
  app is open"; the confirmation dialogs are the safeguard.
