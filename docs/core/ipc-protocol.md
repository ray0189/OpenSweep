# OpenSweep Core IPC protocol v1

Single source of truth shared by `OpenSweepCore.exe` (C++), the Node client (`src/core-client.js`) and tests.

## Transport
- Named pipe: `\\.\pipe\OpenSweepCore.v1` (byte mode, duplex, overlapped I/O on the server).
- Server creates the first instance with `FILE_FLAG_FIRST_PIPE_INSTANCE` (prevents pipe squatting) and
  `PIPE_REJECT_REMOTE_CLIENTS`.
- Security descriptor (SDDL): `D:P(D;;GA;;;NU)(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)`
  — deny network logons, full access for SYSTEM and Administrators, read/write for interactive users.
- Max 8 concurrent clients. A request must arrive completely within 5 s of its first byte; idle
  connections are closed after 60 s.

## Framing
Every message = 4-byte **little-endian** unsigned length N, then N bytes of UTF-8 JSON.
- Requests: 1 ≤ N ≤ 65 536 (64 KiB). Larger → error `too_large`, then the server closes the connection.
- Responses: N ≤ 1 048 576 (1 MiB).

## Request
```json
{ "v": 1, "id": 7, "cmd": "metrics", "args": {} }
```
- `v` must be 1 (else `bad_version`). `id` is an integer echoed back. `cmd` is a known command name.
- `args` is optional; when present it must be an object. Unknown top-level fields are ignored.

## Response
```json
{ "v": 1, "id": 7, "ok": true, "result": { } }
{ "v": 1, "id": 7, "ok": false, "error": { "code": "unknown_cmd", "message": "..." } }
```
If the request could not be parsed, `id` is 0.

Error codes: `bad_frame`, `too_large`, `bad_json`, `bad_version`, `unknown_cmd`, `bad_args`, `forbidden`,
`internal`, `timeout`.

## Capabilities
- `read` — every client allowed by the pipe ACL.
- `admin` — the client token (read while impersonating the pipe client) has the Administrators group
  *enabled* and `TokenElevation` set, i.e. an elevated administrator or SYSTEM. A non-elevated client
  calling an admin command gets `forbidden` (checked before argument validation).
The server records the client PID (`GetNamedPipeClientProcessId`), user SID and token session id for the
audit log. The token session id is also the "caller's session" used by `reportForeground`; it never comes
from request data. A client whose token cannot be read gets `read` only and an unknown session.

Argument validation (all commands): `args` keys not listed for a command are `bad_args`; wrong types and
out-of-range values are `bad_args`. `limit` is an integer ≥ 1 (maximum per command below).
Ids (`txId`, `id`) are a string `[A-Za-z0-9_.:-]{1,64}` or a positive integer (converted to its decimal
string; real ids are strings such as `tx-1779999999999-3` / `ap-1779999999999-1`).

## Phase 1 commands
| cmd | result |
|---|---|
| `hello` | `{ service: "OpenSweepCore", version: "0.1.0", protocol: 1, pid, mode, startedAt (ISO 8601 UTC), uptimeSec }` — `mode` is the engine's current mode |
| `health` | `{ status: "running", mode, uptimeSec, workingSetBytes, privateBytes, cpuTimeMs, clients, requests, errors }` |
| `metrics` | `{ timestamp, cpu: { percent }, memory: { totalPhys, availPhys, loadPercent, commitTotal, commitLimit, commitPeak, systemCache, kernelPaged, kernelNonpaged }, system: { processes, threads, handles }, pageSize }` (bytes; cpu percent is 0–100 over the last sampling interval) |
| `topology` | `{ logicalProcessors, cores, packages, numaNodes, groups, hybrid (bool), efficiencyClasses: [n per class], l3Caches, cpuSets }` — `cpuSets` added in v1 Phases 2–6, see below |

## Phase 2–6 commands (additive, protocol v1)

Contract: `docs/core/phases-2-8.md` ("IPC additions"). Fields marked *(+)* are additive extras beyond the
contract table. Lists are newest first and additionally trimmed so a response stays below 1 MiB.

| cmd | cap | args | result |
|---|---|---|---|
| `status` | read | — | `{ mode, simulation, engine:{ running, ticks, lastTickMs, tickCostUs, tickCostMaxUs(+) }, stabilityScore 0–100, scoreReasons:[str], counts:{ incidentsOpen, policiesActive, recommendations, pendingApprovals(+) }, heal:{ breakerOpen }, simulationFlag(+), optIn(+):[kind], decision(+):{ noActionNeeded, reason }, memory(+):{ level, score }, cpu(+):{ contention } }`. `simulation` = decisions are simulated (monitor, or the simulation flag); `simulationFlag` = the dry-run flag itself. |
| `sample` | read | — | latest `Sample`: `{ tsMs, cpuPercent, cpuQueueLength, contextSwitchesPerSec, dpcPercent, interruptPercent, memTotal, memAvail, memLoadPercent, commitTotal, commitLimit, systemCache, kernelPaged, kernelNonpaged, compressedBytes, hardFaultsPerSec, pageFaultsPerSec, diskReadLatencyMs, diskWriteLatencyMs, diskQueueLength, diskBytesPerSec, cpuFreqPercent, processes, threads, handles, foregroundPid, top:[{ pid, parentPid, name, cpuPercent, privateBytes, workingSet, hardFaultsPerSec, pageFaultsPerSec, handles, threads, sessionId, priorityClass, foreground, isSystemCritical, isSecuritySoftware, isService, hasAudio, signedByMicrosoft, startMs }] }` (≤ 40 processes). `internal` until the first tick. |
| `history` | read | `{ metric, range:"30m"\|"24h"\|"14d" }` (both required) | `{ metric, unit, range(+), points:[[tsMs, value]...] }` ascending, ≤ 900 points. Metrics: the camelCase numeric `Sample` fields above (except `tsMs`/`foregroundPid`) plus `commitPercent`. |
| `baselines` | read | — | `{ metrics:{ name:{ median, mad, ewma, samples, ready } } }` — a metric appears once its first minute has been committed (no `null` medians); `bootDurationMs` appears after the first boot event. |
| `incidents` | read | `{ limit? 1–500, default 50 }` | `{ incidents:[{ id, key, detector, title, summary, severity, state:"open"\|"resolved", confidence 0–1, confidenceLevel, risk, autoRecoveryAllowed, firstSeen, lastSeen, resolvedAt, occurrences, symptoms:[str], evidence:[{ metric, unit, observed, baseline, robustZ, windowFrom, windowTo, note }], contributors:[{ name, kind, pid, correlation }], recommendedActions:[str] }] }` newest activity (`lastSeen`) first |
| `events` | read | `{ limit? 1–2000, default 100 }` | `{ events:[{ tsMs, kind, provider, eventId, level 1–4, subject, message (≤ 200 chars), value }] }` newest first. `kind` e.g. `app_crash`, `service_failure`, `opensweep_config` (mode changes). |
| `recommendations` | read | — | `{ items:[{ id, area:"memory"\|"cpu", target:{ pid, name, startMs }, workload, action:"ecoqos"\|"memory_priority"\|"priority_class"\|"cpu_sets", value, reason, evidence:[str], expectedEffect, risk (1–5), cost, wouldApply, blockedBy?, outcome(+):"simulated"\|"applied"\|"blocked"\|"pending_consent"\|"failed", txId(+)?, beneficiaryPid(+) }] }` — decisions of the latest tick |
| `policies` | read | — | `{ active:[{ txId, kind, area(+), target:{ pid, name, startMs }, value, original, appliedAt, verifyUntil, retainUntil(+), state:"verifying"\|"retained", reason(+) }] }` |
| `journal` | read | `{ limit? 1–500, default 100 }` | `{ entries:[Transaction] }` newest first; `Transaction` = `{ txId, kind:"policy"\|"recovery"\|"config", trigger, evidence, preconditions, target:{ pid, name, startMs }, action, value, original, requiredAuth, state:"intent"\|"applied"\|"verifying"\|"retained"\|"rolled_back"\|"failed"\|"expired", verification, verificationResult, rollback, cooldownUntil, attempt, maxAttempts, createdMs, updatedMs, audit:[{ ts, event, detail }] }` |
| `heal` | read | — | `{ breaker:{ open, failures, openedAt }, pending:[Approval], history:[Transaction] }` — `Approval` = `{ id, kind:"recovery"\|"policy", action, description, target (string), trigger, evidence:[str], risk:"low"\|"medium"\|"high", auth, createdMs, expiresMs }` (+ `targetPid`, `value` for `kind:"policy"`, the Advanced-mode consents); `history` = recovery transactions (≤ 50) |
| `topology` | read | — | Phase 1 shape + `cpuSets:[{ id, group, core, efficiencyClass, numa }]` (one per CPU set = logical processor) |
| `reportForeground` | read | `{ pid }` (integer > 4) | `{}`. The pid must exist and be in the caller's token session (`bad_args` otherwise; `forbidden` if the caller's session is unknown). A session-0 caller reporting its own session-0 pid is accepted but not recorded (session 0 has no foreground window). The hint expires after 10 s without a refresh. |
| `setMode` | admin | `{ mode:"monitor"\|"balanced"\|"advanced", simulation?(+): bool, optIn?(+): [kind] }` | `{ mode, simulation, simulationFlag(+), optIn(+), persisted(+), rolledBack(+):{ restored, failed, expired } }`. Journaled; → monitor rolls everything back; `optIn` replaces the opt-in set (opting out rolls back that kind). Persisted in `config.json`. Escalation is refused (`internal`) when the journal cannot record it. |
| `rollbackAll` | admin | — | `{ restored, failed, expired(+) }` |
| `disablePolicy` | admin | `{ txId }` | `{ restored:bool }` (`false` for an unknown or inactive txId) |
| `approve` | admin | `{ id }` | `{ txId, kind(+):"recovery"\|"policy", detail(+) }` — runs a pending recovery once, or records consent for an Advanced action (applied on a later tick if every gate still passes). Unknown, expired or refused ids → `bad_args`. |
| `exportDiagnostics` | admin | — | `{ path, bytes(+) }` — writes `%ProgramData%\OpenSweep\core\diagnostics.json` (overwritten each time): status, latest sample, baselines, incidents, events, recommendations, policies, journal, heal, captured incident windows, topology, collector counters. No window titles, command lines or document contents; nothing leaves the machine. |

Modes: `monitor` (default; decisions simulated, actuator never called), `balanced` (allow-listed, opted-in,
low-risk actions only), `advanced` (+ CPU Sets, each needing admin consent via `approve`).
