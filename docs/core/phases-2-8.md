# OpenSweep Core — Phases 2–8 contract

This is the shared contract for the parallel work on Phases 2–8. Each module has one owner; others only
consume its header. Shared types live in `core/src/portable/telemetry.hpp` (do not change field meanings
without updating this file).

## Module ownership

| Area | Files | Owner |
|---|---|---|
| Observability (Phase 2) | `core/src/portable/{trace,baseline}.{hpp,cpp}` + tests | engine-observe |
| Stability intelligence (Phase 3) | `core/src/portable/{guard,incident}.{hpp,cpp}` + tests | engine-observe |
| Resource intelligence (Phase 4) | `core/src/portable/{memory_policy,scheduler_policy}.{hpp,cpp}` + tests | engine-act |
| Controlled optimisation (Phase 5) | `core/src/portable/{control,journal}.{hpp,cpp}` + tests | engine-act |
| Self-healing (Phase 6) | `core/src/portable/heal.{hpp,cpp}` + tests | engine-act |
| Windows collectors + actuator | `core/src/win/{collectors,events,actuator,foreground}.{hpp,cpp}` | win-collect |
| Engine wiring + Phase 2–6 IPC commands | `core/src/portable/engine.{hpp,cpp}`, `commands2.{hpp,cpp}`, `src/win/app.cpp` | integration (after the above) |
| Electron dashboard, client, mock, benchmarks, docs | `src/**`, `test/**`, `bench/**`, `research/rea/**`, `docs/core/**` | ui-bench |

`core/CMakeLists.txt`: each owner adds only their own files (one line each) to `opensweep_portable`,
`core_tests` or `OpenSweepCore`. Re-read the file before editing; others edit it concurrently.

## Engine loop (Part 10)

Every `kSampleIntervalMs` (2 s) the engine runs:

```
OBSERVE (collect Sample + new EventRecords)
→ Trace.append            (bounded storage)
→ Baselines.update        (per-metric EWMA + rolling median/MAD, persisted)
→ Guard.evaluate          (anomaly detectors → Incidents with evidence + confidence)
→ MemoryPolicy / SchedulerPolicy .assess   (→ Recommendations; classify workloads)
→ Control.decide          (mode, hysteresis, cooldown, rate limit, conflicts, cost → Decisions)
     monitor   : decisions are recorded as "simulated", nothing applied
     balanced  : only actions on the low-risk allow-list, only after user opt-in, journaled
     advanced  : extra actions, each requires explicit per-action consent from an admin client
→ Journal                 (write-ahead: intent → applied → verified | rolled_back)
→ verification window     (measure; rollback if worse or no improvement)
→ Heal.tick               (OpenSweep-owned recovery; circuit breaker)
```

The engine must be able to decide that nothing should be done, and must never act just because CPU or
RAM usage is high (only on sustained, attributed contention with a verified background target).

## Safety rules every module enforces
- Default mode is `monitor`. Mode changes require an admin client and are journaled.
- Never act on: foreground process, `is_system_critical`, `is_security_software`, `has_audio`, session 0
  services not owned by OpenSweep, processes on the built-in protection list (csrss, wininit, winlogon,
  lsass, services, smss, svchost, dwm, audiodg, MsMpEng, explorer, System, Registry, Memory Compression,
  fontdrvhost, sihost, ctfmon, any anti-cheat image on the list), or unknown workloads.
- No EmptyWorkingSet, no standby-list purge, no affinity pinning, no killing processes automatically.
- Allowed reversible actions: EcoQoS on (background), memory priority lowered (background, ≥ 2),
  priority class BELOW_NORMAL (background), CPU Sets to efficiency cores (hybrid only, advanced only).
- Every applied action has: journal entry written **before** applying, original value, verification
  window (default 60 s), automatic rollback, cooldown per target (default 10 min), global rate limit
  (default ≤ 3 actions / 5 min), and is rolled back on service stop or on start-up (crash recovery).
- Incidents state correlation, never causation; each has symptoms, evidence, possible causes,
  confidence (0–1 + low/medium/high), recommended actions, risk level, and `autoRecoveryAllowed`.
- Privacy: no keystrokes, window titles, command lines, document/browser contents, packet payloads.

## Storage (all under `%ProgramData%\OpenSweep\core`, ACL: SYSTEM + Administrators full, Users read)
| File | Content | Bound |
|---|---|---|
| `trace.bin` | 3 tiers: 2 s × 30 min, 1 min × 24 h, 15 min × 14 d (system metrics) | fixed size |
| `events.jsonl` | stability events | 2 000 records, rotated |
| `baselines.json` | per-metric baselines | small |
| `incidents.jsonl` | incident reports | 500 records, rotated |
| `journal.jsonl` | policy + recovery transactions | 5 000 records, rotated; append + fsync |
| `config.json` | mode, opt-ins, consented actions | small, validated on load |

Writes are atomic (write temp + flush + rename) or append-only with per-line checksums; corrupt lines are
skipped on load and counted.

## IPC additions (protocol v1, additive)

All return `ok:true` shapes below; `admin` = requires elevated Administrators client token.

| cmd | cap | args | result |
|---|---|---|---|
| `status` | read | — | `{ mode, simulation:bool, engine:{ running, ticks, lastTickMs, tickCostUs }, stabilityScore 0–100, scoreReasons:[str], counts:{ incidentsOpen, policiesActive, recommendations }, heal:{ breakerOpen } }` |
| `sample` | read | — | latest `Sample` as JSON (camelCase field names of telemetry.hpp; `top` included) |
| `history` | read | `{ metric, range:"30m"\|"24h"\|"14d" }` | `{ metric, unit, points:[[tsMs, value]...] }` (≤ 900 points) |
| `baselines` | read | — | `{ metrics:{ name:{ median, mad, ewma, samples, ready } } }` |
| `incidents` | read | `{ limit? }` | `{ incidents:[Incident] }` newest first |
| `events` | read | `{ limit? }` | `{ events:[EventRecord] }` newest first |
| `recommendations` | read | — | `{ items:[{ id, area:"memory"\|"cpu", target, workload, action, value, reason, evidence, expectedEffect, risk, cost, wouldApply:bool, blockedBy? }] }` |
| `policies` | read | — | `{ active:[{ txId, kind, target, value, original, appliedAt, verifyUntil, state }] }` |
| `journal` | read | `{ limit? }` | `{ entries:[Transaction] }` newest first |
| `heal` | read | — | `{ breaker:{ open, failures, openedAt }, pending:[Approval], history:[Transaction] }` |
| `topology` | read | — | Phase 1 shape + `{ cpuSets:[{ id, group, core, efficiencyClass, numa }] }` |
| `reportForeground` | read | `{ pid }` | `{}` — hint from the logged-on user's app (session 0 cannot see the foreground window); validated: pid must exist in the caller's session |
| `setMode` | admin | `{ mode:"monitor"\|"balanced"\|"advanced" }` | `{ mode }` |
| `rollbackAll` | admin | — | `{ restored, failed }` |
| `disablePolicy` | admin | `{ txId }` | `{ restored:bool }` |
| `approve` | admin | `{ id }` | `{ txId }` (user-approved recovery or advanced action) |
| `exportDiagnostics` | admin | — | `{ path }` (writes a local zip-free JSON bundle; nothing leaves the machine) |

## Honest-testing rules
Portable modules are unit-tested on Linux in CI. Anything that touches Windows (collectors, ETW/event
log, actuator, service) counts as tested only after it runs green in the `windows` job of `core.yml`.
Benchmarks are a framework plus methodology; no numbers are written down unless a run produced them.
