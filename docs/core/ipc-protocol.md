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
- `read` — every client allowed by the pipe ACL. All Phase 1 commands are `read`.
- `admin` — reserved for later phases (client token must be elevated/Administrators); no Phase 1 command uses it.
The server records the client PID (`GetNamedPipeClientProcessId`) and user for the audit log.

## Phase 1 commands
| cmd | result |
|---|---|
| `hello` | `{ service: "OpenSweepCore", version: "0.1.0", protocol: 1, pid, mode: "monitor", startedAt (ISO 8601 UTC), uptimeSec }` |
| `health` | `{ status: "running", mode: "monitor", uptimeSec, workingSetBytes, privateBytes, cpuTimeMs, clients, requests, errors }` |
| `metrics` | `{ timestamp, cpu: { percent }, memory: { totalPhys, availPhys, loadPercent, commitTotal, commitLimit, commitPeak, systemCache, kernelPaged, kernelNonpaged }, system: { processes, threads, handles }, pageSize }` (bytes; cpu percent is 0–100 over the last sampling interval) |
| `topology` | `{ logicalProcessors, cores, packages, numaNodes, groups, hybrid (bool), efficiencyClasses: [n per class], l3Caches }` |

Modes: Phase 1 is always `monitor` (never changes Windows policy).
