# Kernel driver justification (Part 12)

**Decision (unchanged in Phase 8): no kernel-mode driver is justified.** None is implemented, planned or proposed, and
no project approval has been requested. Sections 1–4 are the original analysis; section 5 is the Phase 8 re-evaluation
with the evidence available after Phases 2–7; section 6 maps every Part 12 requirement to where it is answered.

Part 12 requires, for any future driver: the exact capability, why ETW is insufficient, the documented kernel API,
security implications, signing, HVCI, Secure Boot, Driver Verifier, fault injection, fuzzing, rollback/uninstall and
maintenance cost. A driver may only be proposed when measurements show a user-mode gap that matters, and only with
explicit project approval.

## 1. Candidate capabilities

| Capability (plan part) | User-mode source | Sufficient? | What a driver would add | Verdict |
|---|---|---|---|---|
| DPC/ISR latency attribution (Part 7, 9) | ETW system logger with `EVENT_TRACE_FLAG_DPC` and `EVENT_TRACE_FLAG_INTERRUPT`: each event has the routine address and duration; map addresses to drivers with image-load events (`EVENT_TRACE_FLAG_IMAGE_LOAD`) and loaded-module lists. This is how WPA and LatencyMon-style tools attribute latency. | Yes, for on-demand bounded captures | Nothing documented. Hooking DPC dispatch is not possible with documented APIs and would be PatchGuard-hostile (Part 1 rules 8–9). | Not needed |
| Per-process I/O latency and queue depth (Part 9) | ETW disk I/O (`EVENT_TRACE_FLAG_DISK_IO` / Microsoft-Windows-Kernel-Disk) with issuing thread/process, plus Kernel-File for file-level; PDH `PhysicalDisk` counters for device latency and queue length | Mostly | A file-system minifilter (`FltRegisterFilter`) could time every IRP per process, including cached I/O. ETW attribution is approximate for lazy writer / cache manager I/O (charged to System). | Not needed; document the attribution limit in incidents |
| Memory pressure signals (Part 4) | `GetPerformanceInfo`, `GlobalMemoryStatusEx`, PDH Memory counters (hard faults, commit), `CreateMemoryResourceNotification`, Kernel-Memory ETW | Yes | Nothing the memory manager exposes to drivers in a documented way that user mode lacks for this purpose. | Not needed |
| Process/thread/image notifications (Part 5, 6) | ETW Kernel-Process (process start/stop, image load); WMI process traces | Yes (asynchronous, small delay) | `PsSetCreateProcessNotifyRoutineEx` gives synchronous notification and the ability to block creation. Blocking is not a goal; synchronous timing is not needed for policies that act over seconds. | Not needed |
| Scheduling latency / ready time (Part 5, 14) | ETW context switch and ready-thread events (`EVENT_TRACE_FLAG_CSWITCH`, `DISPATCHER`) | Yes, for short captures | Nothing documented. | Not needed; heavy, so capture only on demand |
| Kernel pool pressure (Part 7) | PDH `Pool Paged/Nonpaged Bytes`, `GetPerformanceInfo`; ETW pool events for short captures (**verify** availability) | Partly | Per-tag pool usage is available in user mode only via an undocumented `NtQuerySystemInformation` class; a driver does not get a documented per-tag API either. | Not needed; totals are enough for detection, per-tag via WPR/poolmon by the user |
| GPU timeouts (Part 7, 9) | Event Log Display 4101 (used today in `stabilityCheck()`), DxgKrnl ETW | Yes | Nothing documented outside WDDM drivers themselves. | Not needed |
| Thermal throttling (Part 7, 9) | PDH `% Processor Performance`, thermal-zone counters, Kernel-Power events (**verify** per platform) | Partly; firmware-dependent | Reading MSRs/package temperatures needs vendor-specific MSR access from a driver. Generic MSR read/write drivers are a well-known vulnerability class (blocked by Microsoft's vulnerable driver blocklist). | Not justified; accept "not available on this hardware" |
| Hardware performance counters (Part 14 research) | ETW PMC sampling via `TraceSetInformation` (profile sources) where supported (**verify** per Windows version) | Partly | Direct PMC programming = MSR access, same risk as above. | Not justified |
| Enforcing policies (CPU sets, EcoQoS, memory priority) | `SetProcessInformation`, `SetProcessDefaultCpuSets`, `SetPriorityClass` | Yes | Nothing; these are user-mode policy APIs by design. | Not needed |
| Protecting OpenSweep's own process/service | Service DACL, protected directory ACLs, signed binaries | Yes for the threat model (local non-admin attacker) | Process protection (`ObRegisterCallbacks`) only matters against admins, who can unload drivers anyway. | Not needed |

## 2. Why ETW is generally enough here

- The plan's detection goals (Parts 4–9) act on trends over seconds to hours, not on single events, so ETW's asynchronous
  delivery and occasional lost events are acceptable if the service records "events lost" counts with each incident.
- The heavy kernel flags (DPC, ISR, CSWITCH, disk I/O) are needed only during short, bounded diagnostic captures, which ETW
  system logger sessions (Windows 8+) support without exclusive use of the "NT Kernel Logger".
- Attribution limits that remain (cache-manager I/O, interrupts shared between devices) are limits of what the OS knows, not
  of user mode. A driver would not remove most of them.

Known costs of the ETW approach that must be measured in Phase 2/7: CPU and memory of a real-time consumer, buffer sizing to
avoid lost events, session-count limits (64 global), and interaction with other tools (WPR, anti-cheat, EDR) that also use
kernel sessions.

## 3. What a driver would cost

| Topic | Requirement / implication |
|---|---|
| Signing | Since Windows 10 1607, new kernel drivers must be signed by Microsoft through the Hardware Dev Center. That needs an EV code-signing certificate for the dashboard account. Options: **attestation signing** (Windows 10/11 client only; not accepted on Windows Server; no HLK tests) or **WHQL/HLK** certification (full HLK test runs per OS version). Cross-signing is no longer accepted for new drivers. |
| Secure Boot | With Secure Boot on (default on Windows 11), only properly Microsoft-signed drivers load. Test signing requires turning Secure Boot off — not acceptable for users (Part 1 rule 9). |
| HVCI / Memory integrity | On by default on many Windows 11 installs. The driver must be HVCI-compatible: no RWX memory, NX pool allocations (`POOL_FLAG_NON_PAGED` / `NonPagedPoolNx`), no dynamic code, no writable+executable sections, validated with the HVCI readiness tool and Driver Verifier's code-integrity checks. Incompatible drivers are blocked from loading. |
| Vulnerable driver blocklist / Smart App Control | Any IOCTL that exposes memory, MSRs, physical memory or process handles to user mode will get the driver blocklisted and is forbidden by Part 12. |
| Driver Verifier | Mandatory in development and CI VMs: standard settings plus pool tracking, IRQL checking, deadlock detection, DDI compliance, low-resources simulation. |
| Fault injection | Driver Verifier low-resource simulation; forced IRP cancellation; unload during I/O; sleep/resume and hibernation; surprise removal if it touches devices. |
| Fuzz testing | Every IOCTL with malformed sizes, buffers, method types; concurrent opens; random close order. The device object needs a restrictive SDDL (`IoCreateDeviceSecure`) and `FILE_DEVICE_SECURE_OPEN`. |
| Static analysis | CodeQL for drivers / Static Driver Verifier as required by WHQL. |
| Bugs = blue screens | A user-mode service crash restarts the service; a driver bug crashes the PC, which is the opposite of a stability product. |
| Rollback and uninstall | Install via INF (`pnputil /add-driver` or SetupAPI), ordered with the service; uninstall must stop dependent services, unload, delete the service key and remove the driver package from the driver store. Must handle "pending reboot" states. Updates of a loaded driver usually need a reboot. |
| ARM64 | Separate arm64 driver build and signing submission. |
| Maintenance | Re-test per Windows feature update (two client releases a year), per HLK kit, per Server version if ever supported; kernel-mode C/C++ expertise on the team; incident response if a vulnerability is found (signed vulnerable drivers stay loadable on unpatched machines until blocklisted). |
| Project fit | MIT open-source project with no current signing infrastructure (`signAndEditExecutable: false`, see `security-audit.md` S6). Code signing for user-mode binaries is a prerequisite even before a driver could be considered. |

## 4. Conclusion

A driver would add risk (system crashes, a privileged attack surface, signing and certification overhead, per-release
maintenance) and no capability that the current plan needs. ETW, Event Log, PDH and the documented process/memory/CPU-set
APIs cover Phases 1–7.

Revisit only if, in Phase 8, all of the following are true:

1. A specific Part 4–9 detection is shown (with WPR/WPA traces from real machines) to be impossible or materially wrong with
   user-mode data.
2. A documented kernel API exists that provides the missing data without exposing memory, MSRs or handles to user mode.
3. The project has user-mode code signing in place and a plan (and funding) for EV certificate, attestation/WHQL submission,
   HVCI validation and per-release testing.
4. Explicit project approval is recorded.

## 5. Phase 8 re-evaluation (feasibility only — no implementation)

### 5.1 What changed since the first version

Phases 2–7 added a user-mode engine (collectors, SweepTrace, baselines, SweepGuard incidents, SweepMemory /
SweepScheduler recommendations, controlled policies with journal and rollback, SweepHeal). Everything it needs is
obtained from documented user-mode sources: PDH counters, `GetPerformanceInfo` / `GlobalMemoryStatusEx`, process
enumeration and `GetProcessMemoryInfo`, the Event Log, `GetSystemCpuSetInformation`, and the documented policy APIs
(`SetProcessInformation` for EcoQoS and memory priority, `SetPriorityClass`, `SetProcessDefaultCpuSets`).

**Evidence status.** The Phase 2–7 Windows code has been built and run only on GitHub's `windows-latest` CI runners
(a VM). There are **no** WPR/WPA traces from real user machines, and no benchmark results (`bench/` is a framework;
see `bench/README.md`). Therefore no measurement exists that shows a user-mode gap. Criterion 1 of section 4 is not met.

### 5.2 Per-capability assessment

| Exact capability | Why user mode / ETW might be insufficient | Documented kernel API that would provide it | Security implication if implemented | Evidence today | Verdict |
|---|---|---|---|---|---|
| Per-process storage latency including cached and lazy-writer I/O (Part 9 storage incidents) | ETW Kernel-Disk charges cache-manager write-back to `System`; per-file attribution needs Kernel-File + correlation; PDH is per device only | File-system minifilter: `FltRegisterFilter`, pre/post-operation callbacks, `FltGetRequestorProcessId` | Minifilter sits on every file I/O path: a bug is a bugcheck or data corruption; altitude allocation from Microsoft; must not expose file contents or names to unprivileged clients | None: no trace shows that incidents are wrong because of this attribution limit | Not justified; incidents state "attribution approximate for cached I/O" |
| Synchronous process-creation notification (to classify a workload before it runs) | ETW Kernel-Process is asynchronous (typically sub-second delay) | `PsSetCreateProcessNotifyRoutineEx` | Can block process creation → must never be used to block; registered callbacks are a common EDR/anti-cheat surface and a compatibility risk | Policies act after ≥ 10 min of sustained contention (hysteresis), so sub-second delay is irrelevant | Not justified |
| DPC/ISR attribution beyond ETW | ETW DPC/ISR events already give routine address and duration | None documented that adds data | — | — | Not needed (ETW sufficient) |
| Thermal / package power / MSR-based frequency | Firmware-dependent PDH/ACPI data | None generic; MSR access is undocumented-by-design for this purpose | Generic MSR drivers are a known vulnerability class and are blocklisted | — | Rejected (Part 12 forbids this class) |
| Per-tag kernel pool usage | Only via an undocumented `NtQuerySystemInformation` class in user mode | None documented for drivers either | — | — | Rejected; totals suffice |
| Protecting OpenSweep's own service from tampering | Service DACL, protected `%ProgramData%\OpenSweep` ACL, signed binaries | `ObRegisterCallbacks` | Only meaningful against administrators, who can unload drivers anyway | — | Not justified |

### 5.3 Decision and conditions to revisit

No driver. The conditions of section 4 still apply unchanged, and all four must hold before a proposal is written:

1. Real-machine WPR/WPA traces (named builds and hardware, as in `research/rea/README.md`) show a specific Part 4–9
   detection that is impossible or materially wrong with user-mode data, **and** that the error changes a user-visible
   decision (an incident, a recommendation or a policy).
2. A **documented** kernel API provides the missing data (no undocumented structures or offsets, Part 11).
3. Signing is solved first for the user-mode binaries (security review S6), then EV certificate + attestation/WHQL
   submission is funded and planned, with HVCI validation and per-release testing.
4. Explicit project approval is recorded in this document.

### 5.4 Non-negotiable constraints for any future driver

- No IOCTL that reads or writes arbitrary kernel or physical memory, MSRs, I/O ports, or returns kernel addresses.
- No IOCTL that opens handles to other processes/threads on behalf of the caller, changes protection levels, or
  performs any privileged operation on a caller-chosen target. Only fixed, typed, read-only telemetry queries.
- Device object created with `IoCreateDeviceSecure` and a restrictive SDDL (SYSTEM + the OpenSweep service SID only),
  `FILE_DEVICE_SECURE_OPEN`; the user-mode service remains the only client and keeps all policy decisions.
- Every input buffer validated for size and type (METHOD_BUFFERED only); no user-mode pointers dereferenced.
- HVCI-compatible (NX pool, no RWX, no dynamic code), Driver Verifier clean (standard + IRQL, pool, deadlock, DDI
  compliance, low-resources simulation), fuzzed per IOCTL, Static Driver Verifier / CodeQL clean.
- Uninstall path tested (stop, unload, delete service key, `pnputil /delete-driver`), including pending-reboot states;
  OpenSweep must keep working without the driver (feature detection, never a hard dependency).

## 6. Part 12 checklist

| Part 12 requirement | Where answered |
|---|---|
| Exact required capability | §1 table (candidates), §5.2 "Exact capability" column |
| Why ETW is insufficient | §2, §5.2 second column |
| Relevant documented kernel API | §1 "What a driver would add", §5.2 third column (`FltRegisterFilter`, `PsSetCreateProcessNotifyRoutineEx`, `ObRegisterCallbacks`, `IoCreateDeviceSecure`) |
| Security implications | §3 (bugs = bugchecks, blocklist), §5.2 fourth column, §5.4 constraints |
| Driver signing requirements | §3 "Signing" (Hardware Dev Center, EV certificate, attestation vs WHQL/HLK) |
| HVCI compatibility | §3 "HVCI / Memory integrity", §5.4 |
| Secure Boot compatibility | §3 "Secure Boot" |
| Driver Verifier testing | §3 "Driver Verifier", §5.4 |
| Fault injection | §3 "Fault injection" (low-resource simulation, IRP cancellation, unload during I/O, sleep/resume) |
| Fuzz testing | §3 "Fuzz testing", §5.4 |
| Rollback and uninstall | §3 "Rollback and uninstall", §5.4 |
| Maintenance cost | §3 "Maintenance", "ARM64", "Project fit" |
| Propose only with evidence + explicit approval | §4, §5.3 (not met: no real-machine evidence, no approval requested) |
| No arbitrary kernel memory access / unrestricted privileged operations | §5.4 |
