'use strict';
// Demo backend: realistic sample data so the UI can be developed and previewed on
// macOS/Linux (or with --demo on Windows). Nothing here touches the real system.
const C = require('./catalog');

const wait = (ms) => new Promise(r => setTimeout(r, ms));
const GB = 1024 ** 3, MB = 1024 ** 2;
const rnd = (a, b) => Math.round(a + Math.random() * (b - a));

const state = {
  tweaks: Object.fromEntries(C.TWEAKS.map(t => [t.id, ['explorer.extensions', 'gaming.gamemode'].includes(t.id)])),
  installed: new Set(C.BLOAT_APPS.filter((_, i) => i % 4 !== 3).map(a => a.pkg)),
  services: Object.fromEntries(C.SERVICES.map(s => [s.name, s.orig])),
  tasks: Object.fromEntries(C.SCHEDULED_TASKS.map(t => [t.path + t.name, 'Ready'])),
  junk: Object.fromEntries(C.CLEANUP.map(c => [c.id, c.special === 'recycle' ? 1.3 * GB : rnd(0, 900) * MB])),
  startup: [
    { id: 'HKCU|OneDrive', source: 'HKCU', name: 'OneDrive', command: '"C:\\Program Files\\Microsoft OneDrive\\OneDrive.exe" /background', enabled: true, admin: false, impact: 'High' },
    { id: 'HKCU|Discord', source: 'HKCU', name: 'Discord', command: 'C:\\Users\\you\\AppData\\Local\\Discord\\Update.exe --processStart Discord.exe', enabled: true, admin: false, impact: 'High' },
    { id: 'HKCU|Spotify', source: 'HKCU', name: 'Spotify', command: 'C:\\Users\\you\\AppData\\Roaming\\Spotify\\Spotify.exe /minimized', enabled: true, admin: false, impact: 'Medium' },
    { id: 'HKCU|MicrosoftEdgeAutoLaunch', source: 'HKCU', name: 'MicrosoftEdgeAutoLaunch', command: '"C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe" --no-startup-window', enabled: true, admin: false, impact: 'Medium' },
    { id: 'HKLM|SecurityHealth', source: 'HKLM', name: 'SecurityHealth', command: '%windir%\\system32\\SecurityHealthSystray.exe', enabled: true, admin: true, impact: 'Low' },
    { id: 'HKLM|RtkAudUService', source: 'HKLM', name: 'RtkAudUService', command: '"C:\\Windows\\System32\\RtkAudUService64.exe" -background', enabled: true, admin: true, impact: 'Low' },
    { id: 'HKLM32|Steam', source: 'HKLM32', name: 'Steam', command: '"C:\\Program Files (x86)\\Steam\\steam.exe" -silent', enabled: false, admin: true, impact: 'High' },
    { id: 'UserFolder|Notion.lnk', source: 'UserFolder', name: 'Notion', command: 'C:\\Users\\you\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu\\Programs\\Startup\\Notion.lnk', enabled: true, admin: false, impact: 'Medium' },
    { id: 'Task|\\AdobeGCInvoker-1.0', source: 'Task', name: 'AdobeGCInvoker-1.0', taskPath: '\\', command: 'C:\\Program Files (x86)\\Common Files\\Adobe\\AdobeGCClient\\AGCInvokerUtility.exe', enabled: true, admin: true, impact: 'Low' },
  ],
  procs: null,
};

const PROC_NAMES = [['chrome', 'Google LLC'], ['chrome', 'Google LLC'], ['chrome', 'Google LLC'], ['msedge', 'Microsoft Corporation'], ['Discord', 'Discord Inc.'], ['Spotify', 'Spotify AB'], ['explorer', 'Microsoft Corporation'], ['svchost', 'Microsoft Corporation'], ['svchost', 'Microsoft Corporation'], ['MsMpEng', 'Microsoft Corporation'], ['OneDrive', 'Microsoft Corporation'], ['Code', 'Microsoft Corporation'], ['steam', 'Valve'], ['SearchHost', 'Microsoft Corporation'], ['Widgets', 'Microsoft Corporation'], ['PhoneExperienceHost', 'Microsoft Corporation'], ['dwm', 'Microsoft Corporation'], ['Teams', 'Microsoft Corporation'], ['RuntimeBroker', 'Microsoft Corporation'], ['Copilot', 'Microsoft Corporation'], ['Notion', 'Notion Labs'], ['audiodg', 'Microsoft Corporation']];

// ---------------------------------------------------------------- OpenSweep Core Phase 2–8 simulation
// Everything below is SIMULATED (demo mode only): synthetic time series with noise, one synthetic
// memory-leak incident, one synthetic crash-loop incident, recommendations that would not apply in
// Monitor mode, and a short journal. Every result carries `simulated: true`.
// Shapes follow docs/core/phases-2-8.md; Incident and Transaction use the field names of the engine's
// to_json (core/src/portable/incident.cpp, journal.cpp). Approval JSON is not fixed by the contract yet:
// { id, action, target, trigger, evidence[], risk, createdMs, expiresMs } is what the dashboard reads.
const coreSim = (() => {
  const T0 = Date.now();
  const sim = { mode: 'monitor', txSeq: 1046, policies: [], ticks0: T0 - 3 * 3600e3 - 17 * 60e3, etw: true, etwSince: T0 - 3 * 3600e3 - 17 * 60e3 };
  const LEAK_START = T0 - 3 * 3600e3, LEAK_PID = 7712, CRASH_SVC = 'ExampleSyncSvc';
  // Deterministic noise per (metric, time bucket) so repeated history calls agree with each other.
  const hash = (str, n) => { let x = 2166136261 ^ n; for (let i = 0; i < str.length; i++) x = Math.imul(x ^ str.charCodeAt(i), 16777619); x ^= x >>> 13; x = Math.imul(x, 0x5bd1e995); x ^= x >>> 15; return ((x >>> 0) % 100000) / 100000; };
  const noise = (m, t, step) => hash(m, Math.floor(t / step)) * 2 - 1;
  const leak = (t) => Math.max(0, Math.min(1, (t - LEAK_START) / (3 * 3600e3))); // 0 → 1 over 3 h
  const day = (t) => Math.sin(((t / 3600e3) % 24) / 24 * 2 * Math.PI - Math.PI / 2) * 0.5 + 0.5; // daily rhythm
  const METRICS = {
    cpuPercent: { unit: '%', v: (t, s) => Math.max(0.5, Math.min(100, 6 + 10 * day(t) + 5 * noise('cpu', t, s) + (hash('cpuspk', Math.floor(t / 60e3)) > 0.97 ? 35 : 0))) },
    memLoadPercent: { unit: '%', v: (t, s) => 41 + 3 * day(t) + 1.2 * noise('mem', t, s) + 8 * leak(t) },
    commitTotal: { unit: 'bytes', v: (t, s) => (14.2 + 0.8 * day(t) + 0.25 * noise('com', t, s) + 2.6 * leak(t)) * GB },
    commitLimit: { unit: 'bytes', v: () => 37.9 * GB },
    diskReadLatencyMs: { unit: 'ms', v: (t, s) => Math.max(0.1, 0.7 + 0.35 * noise('drl', t, s) + (hash('drs', Math.floor(t / 120e3)) > 0.95 ? 9 : 0)) },
    diskWriteLatencyMs: { unit: 'ms', v: (t, s) => Math.max(0.1, 1.1 + 0.5 * noise('dwl', t, s)) },
    hardFaultsPerSec: { unit: '/s', v: (t, s) => Math.max(0, 18 + 14 * noise('hf', t, s) + 40 * leak(t) * (hash('hfl', Math.floor(t / 30e3)) > 0.8 ? 1 : 0)) },
    pageFaultsPerSec: { unit: '/s', v: (t, s) => 2400 + 900 * noise('pf', t, s) },
    cpuQueueLength: { unit: '', v: (t, s) => Math.max(0, 0.8 + 0.7 * noise('q', t, s)) },
    contextSwitchesPerSec: { unit: '/s', v: (t, s) => 21000 + 6000 * noise('cs', t, s) + 9000 * day(t) },
    dpcPercent: { unit: '%', v: (t, s) => Math.max(0, 0.25 + 0.15 * noise('dpc', t, s)) },
    interruptPercent: { unit: '%', v: (t, s) => Math.max(0, 0.18 + 0.1 * noise('isr', t, s)) },
    diskQueueLength: { unit: '', v: (t, s) => Math.max(0, 0.05 + 0.05 * noise('dq', t, s)) },
    diskBytesPerSec: { unit: 'bytes/s', v: (t, s) => Math.max(0, (1.5 + 1.4 * noise('db', t, s)) * MB) },
    cpuFreqPercent: { unit: '%', v: (t, s) => 96 + 4 * noise('fq', t, s) },
  };
  const RANGE = { '30m': [30 * 60e3, 2e3], '24h': [24 * 3600e3, 120e3], '14d': [14 * 86400e3, 1800e3] }; // ≤ 900 points each
  const valueAt = (m, t, step = 2e3) => { const d = METRICS[m]; return d ? d.v(t, step) : 0; };
  const leakBytes = (t) => Math.round((380 + 2500 * leak(t)) * MB);
  const BASE = { cpuPercent: [11.4, 3.2], memLoadPercent: [42.6, 1.4], commitTotal: [14.6 * GB, 0.45 * GB], diskReadLatencyMs: [0.68, 0.22], hardFaultsPerSec: [17, 9], cpuQueueLength: [0.8, 0.4], diskWriteLatencyMs: [1.1, 0.3] };
  const ago = (ms) => T0 - ms;

  // Field names follow core/src/portable/incident.cpp to_json (Incident) and journal.cpp to_json (Transaction).
  const W = (from, to) => ({ windowFrom: from, windowTo: to });
  const incidents = () => [
    { id: 'inc-0007', key: 'memory_leak:LeakyHelper.exe', detector: 'memory_leak', title: 'Steadily growing memory use: LeakyHelper.exe', state: 'open', severity: 'warning',
      summary: 'LeakyHelper.exe has grown almost every minute for about 3 hours while commit charge rose with it.',
      firstSeen: LEAK_START + 40 * 60e3, lastSeen: Date.now(), resolvedAt: 0, occurrences: 1,
      symptoms: ['Commit charge rose from 14.6 GB to ' + (commitAt(Date.now()) / GB).toFixed(1) + ' GB over ~3 hours', 'Physical memory load is above its usual range', 'Hard page faults are rising in bursts'],
      evidence: [
        { metric: 'commitTotal', unit: 'bytes', observed: commitAt(Date.now()), baseline: 14.6 * GB, robustZ: (commitAt(Date.now()) - 14.6 * GB) / (0.45 * GB * 1.4826), ...W(LEAK_START, Date.now()), note: 'outside the usual range' },
        { metric: 'memLoadPercent', unit: '%', observed: 41 + 8 * leak(Date.now()), baseline: 42.6, robustZ: (41 + 8 * leak(Date.now()) - 42.6) / (1.4 * 1.4826), ...W(Date.now() - 30 * 60e3, Date.now()) },
        { metric: 'LeakyHelper.exe private bytes', unit: 'bytes', observed: leakBytes(Date.now()), baseline: 380 * MB, robustZ: null, ...W(LEAK_START, Date.now()), note: 'monotonic growth, slope ≈ 830 MB/h' },
      ],
      contributors: [{ name: 'LeakyHelper.exe', kind: 'process', pid: LEAK_PID, correlation: 0.97 }, { name: 'chrome.exe', kind: 'process', pid: 4410, correlation: 0.12 }],
      confidence: 0.78, confidenceLevel: 'medium', risk: 'medium',
      recommendedActions: ['Restart LeakyHelper.exe when convenient (save work first)', 'Check for an update of the app that installed it', 'If it keeps growing, report it to its developer'],
      autoRecoveryAllowed: false, simulated: true },
    { id: 'inc-0006', key: `crash_loop:${CRASH_SVC}`, detector: 'crash_loop', title: `Service crash loop: ${CRASH_SVC}`, state: 'open', severity: 'error',
      summary: 'The service stopped unexpectedly 6 times in 20 minutes; Windows restarted it each time.',
      firstSeen: ago(52 * 60e3), lastSeen: ago(9 * 60e3), resolvedAt: 0, occurrences: 6,
      symptoms: ['The service stopped unexpectedly 6 times in 20 minutes', 'Windows restarted it each time (service recovery actions)'],
      evidence: [{ metric: 'Service Control Manager 7031', unit: 'events', observed: 6, baseline: 0, robustZ: null, ...W(ago(29 * 60e3), ago(9 * 60e3)) }, { metric: 'Application Error 1000 (ExampleSync.exe)', unit: 'events', observed: 6, baseline: 0, robustZ: null, ...W(ago(29 * 60e3), ago(9 * 60e3)), note: 'faulting module: examplesync_core.dll' }],
      contributors: [{ name: CRASH_SVC, kind: 'service', pid: 0, correlation: 0.93 }, { name: 'KB5048685 (installed 1 h before)', kind: 'update', pid: 0, correlation: 0.31 }],
      confidence: 0.88, confidenceLevel: 'high', risk: 'low',
      recommendedActions: ['Approve one controlled restart with back-off (waiting in Recovery)', 'Update or reinstall the app that owns the service', 'Disable the service if you do not use it'],
      autoRecoveryAllowed: false, simulated: true },
    { id: 'inc-0008', key: 'driver_latency:Netwtw10.sys', detector: 'driver_latency', title: 'Driver Netwtw10.sys correlated with latency spikes (max 2,400 µs)', state: 'open', severity: 'warning',
      summary: 'The Intel Wi-Fi driver had deferred procedure calls (DPCs) longer than 1,000 µs in 4 of the last 10 one-minute windows. Long DPCs can cause audio crackle, input lag or short stutters.',
      firstSeen: ago(14 * 60e3), lastSeen: ago(60e3), resolvedAt: 0, occurrences: 4,
      symptoms: ['Longest single DPC/ISR of Netwtw10.sys: 2,400 µs (usual for this PC: about 300 µs)', 'Spikes above 1,000 µs in 4 of the last 10 minutes'],
      evidence: [
        { metric: 'Netwtw10.sys longest DPC/ISR', unit: 'µs', observed: 2400, baseline: 290, mad: 80, robustZ: null, ...W(ago(10 * 60e3), ago(0)), note: '50 calls ≥ 1,000 µs in 10 min' },
        { metric: 'dpcPercent', unit: '%', observed: 0.42, baseline: 0.25, mad: 0.08, robustZ: 1.4, ...W(ago(10 * 60e3), ago(0)) },
      ],
      contributors: [{ name: 'Netwtw10.sys', kind: 'driver', pid: 0, confidence: 0.71, detail: 'Intel Wi-Fi adapter driver' }],
      confidence: 0.71, confidenceLevel: 'medium', risk: 'low',
      possibleCauses: ['An outdated or buggy Wi-Fi driver', 'Aggressive Wi-Fi power saving waking the adapter often', 'Wi-Fi roaming / background scanning'],
      recommendedActions: ['Update the Intel Wi-Fi driver (Device Manager → Network adapters → Intel Wi-Fi 6E AX211 → Update driver, or from Intel / the PC maker)', 'If the problem started after a driver update, roll it back (Device Manager → Properties → Driver → Roll Back Driver)', 'Check the adapter\'s power settings (Power Management tab, and the Wireless Adapter setting in the power plan)', 'If you can, test with Ethernet to see whether the stutter goes away'],
      autoRecoveryAllowed: false, autoRecoveryReason: 'OpenSweep Core never changes drivers on its own.', simulated: true },
    { id: 'inc-0005', key: 'storage_latency:disk0', detector: 'storage_latency', title: 'Short storage latency spike', state: 'resolved', severity: 'warning',
      summary: 'Read latency was far above its usual range for about 90 seconds, then recovered on its own.',
      firstSeen: ago(26 * 3600e3), lastSeen: ago(25.97 * 3600e3), resolvedAt: ago(25.6 * 3600e3), occurrences: 1,
      symptoms: ['Read latency reached 41 ms (usual 0.7 ms) for about 90 seconds'],
      evidence: [{ metric: 'diskReadLatencyMs', unit: 'ms', observed: 41, baseline: 0.68, robustZ: 124.7, ...W(ago(26 * 3600e3), ago(26 * 3600e3) + 90e3) }],
      contributors: [{ name: 'MsMpEng.exe', kind: 'process', pid: 3324, correlation: 0.46 }],
      confidence: 0.46, confidenceLevel: 'low', risk: 'low',
      recommendedActions: ['No action needed — it recovered on its own'], autoRecoveryAllowed: false, simulated: true },
  ];
  function commitAt(t) { return valueAt('commitTotal', t, 60e3); }

  // Contract shape (phases-2-8 "recommendations"); risk is the engine's 1 (lowest) .. 5, evidence strings.
  const recommendations = () => {
    const monitor = sim.mode === 'monitor';
    const active = new Set(sim.policies.map(p => p.target.name));
    return [
      { id: 'cpu:ecoqos:9120', area: 'cpu', target: { pid: 9120, name: 'OneDrive.exe' }, workload: 'Background', action: 'ecoqos', value: 1,
        reason: 'Background sync used 14–19% CPU for 12 minutes while the foreground app was waiting for CPU (queue length 4.1 per CPU, usual 0.8).',
        evidence: ['OneDrive.exe CPU 16.8 % over 12 min', 'cpuQueueLength 4.1 (baseline 0.8)'],
        expectedEffect: 'Less CPU contention for the foreground app. Kept only if the 60 s verification window shows lower queue length; otherwise rolled back.',
        risk: 1, cost: 0.1, wouldApply: !monitor && !active.has('OneDrive.exe'), blockedBy: monitor ? 'Monitor mode — simulation only' : active.has('OneDrive.exe') ? 'Already applied' : undefined, simulated: true },
      { id: 'memory:memory_priority:7712', area: 'memory', target: { pid: LEAK_PID, name: 'LeakyHelper.exe' }, workload: 'Background', action: 'memory_priority', value: 2,
        reason: 'Growing background process while memory load is above baseline; lower memory priority makes Windows trim it before foreground apps.',
        evidence: ['LeakyHelper.exe private bytes +2.4 GB in 3 h'],
        expectedEffect: 'Fewer hard faults in foreground apps under pressure. Does not fix the leak.',
        risk: 2, cost: 0.2, wouldApply: false, blockedBy: monitor ? 'Monitor mode — simulation only' : 'Memory pressure not sustained long enough (hysteresis: 10 min above threshold, now 6 min)', simulated: true },
      { id: 'cpu:cpu_sets:6408', area: 'cpu', target: { pid: 6408, name: 'Teams.exe' }, workload: 'Interactive', action: 'cpu_sets', value: 0,
        reason: 'Considered moving work to efficiency cores.', evidence: [], expectedEffect: '—', risk: 3, cost: 0.5, wouldApply: false,
        blockedBy: 'CPU is not hybrid (no efficiency cores); interactive workloads are never moved', simulated: true },
    ];
  };

  const tx = (o) => ({ evidence: [], preconditions: [], requiredAuth: 'none', verification: '', verificationResult: '', rollback: '', cooldownUntil: 0, attempt: 1, maxAttempts: 1, audit: [], updatedMs: o.createdMs, simulated: true, ...o });
  const journal = [
    tx({ txId: 'tx-1046', createdMs: ago(9 * 60e3), kind: 'recovery', action: 'restart_collector', target: { pid: 0, name: 'kernel trace session', startMs: 0 }, value: 0, original: null, state: 'retained', trigger: 'The service\'s own ETW session stopped (events lost); restarted once.', verificationResult: 'Events flowing again after 2 s' }),
    tx({ txId: 'tx-1045', createdMs: ago(34 * 60e3), kind: 'policy', action: 'ecoqos', target: { pid: 9120, name: 'OneDrive.exe', startMs: 0 }, value: 1, original: -1, state: 'simulated', trigger: 'Would have applied: sustained background CPU while the foreground app waited (Monitor mode — nothing changed).' }),
    tx({ txId: 'tx-1044', createdMs: ago(2.9 * 3600e3), kind: 'policy', action: 'memory_priority', target: { pid: 5520, name: 'SearchIndexer.exe', startMs: 0 }, value: 2, original: 5, state: 'simulated', trigger: 'Considered, then rejected: system component on the protection list.' }),
    tx({ txId: 'tx-1043', createdMs: ago(3.2 * 3600e3), kind: 'recovery', action: 'reconnect_event_subscription', target: { pid: 0, name: 'System event log', startMs: 0 }, value: 0, original: null, state: 'retained', trigger: 'Event log subscription returned an error after resume from sleep; reconnected.', verificationResult: 'New events received' }),
    tx({ txId: 'tx-1042', createdMs: ago(3.3 * 3600e3), kind: 'config', action: 'set_mode', target: { pid: 0, name: 'mode', startMs: 0 }, value: 'monitor', original: null, state: 'applied', trigger: 'Default mode on first start.' }),
  ];
  const approvals = [{ id: 'ap-1795000000000-1', action: 'restart_service', target: CRASH_SVC, trigger: 'Crash loop (6 failures in 20 min). One restart with 5 min back-off; the circuit breaker stops after 2 failed attempts.', evidence: ['Service Control Manager 7031 × 6 in 20 min', 'Application Error 1000 × 6 (examplesync_core.dll)'], risk: 'medium', auth: 'admin', createdMs: ago(9 * 60e3), expiresMs: Date.now() + 50 * 60e3, simulated: true }];
  const breaker = { open: false, failures: 1, openedAt: null };

  const events = () => [
    ...[0, 1, 2, 3, 4, 5].map(i => ({ tsMs: ago((9 + i * 3.4) * 60e3), kind: 'service_failure', provider: 'Service Control Manager', eventId: 7031, level: 2, subject: CRASH_SVC, message: `The ${CRASH_SVC} service terminated unexpectedly. It has done this ${6 - i} time(s).`, value: 0 })),
    ...[0, 1, 2].map(i => ({ tsMs: ago((10 + i * 6.8) * 60e3), kind: 'app_crash', provider: 'Application Error', eventId: 1000, level: 2, subject: 'ExampleSync.exe', message: 'Faulting application ExampleSync.exe, faulting module examplesync_core.dll', value: 0 })),
    { tsMs: ago(70 * 60e3), kind: 'windows_update', provider: 'Microsoft-Windows-WindowsUpdateClient', eventId: 19, level: 4, subject: 'KB5048685', message: 'Installation successful: KB5048685', value: 0 },
    { tsMs: ago(3.3 * 3600e3), kind: 'opensweep_config', provider: 'OpenSweepCore', eventId: 0, level: 4, subject: 'mode', message: 'Mode set to monitor (default)', value: 0 },
    { tsMs: ago(3.35 * 3600e3), kind: 'boot_performance', provider: 'Microsoft-Windows-Diagnostics-Performance', eventId: 100, level: 4, subject: '', message: 'Windows started in 24.1 s', value: 24100 },
    { tsMs: ago(26 * 3600e3), kind: 'app_hang', provider: 'Application Hang', eventId: 1002, level: 2, subject: 'explorer.exe', message: 'explorer.exe stopped responding for 6 s', value: 0 },
  ].map(e => ({ ...e, simulated: true }));

  function sample() {
    const t = Date.now();
    const v = (m) => valueAt(m, t);
    return {
      simulated: true, tsMs: t, cpuPercent: v('cpuPercent'), cpuQueueLength: v('cpuQueueLength'), contextSwitchesPerSec: v('contextSwitchesPerSec'), dpcPercent: v('dpcPercent'), interruptPercent: v('interruptPercent'),
      memTotal: 32 * GB, memAvail: Math.round(32 * GB * (1 - v('memLoadPercent') / 100)), memLoadPercent: Math.round(v('memLoadPercent')), commitTotal: Math.round(v('commitTotal')), commitLimit: 37.9 * GB,
      systemCache: 6.2 * GB, kernelPaged: 610 * MB, kernelNonpaged: 280 * MB, compressedBytes: 820 * MB, hardFaultsPerSec: v('hardFaultsPerSec'), pageFaultsPerSec: v('pageFaultsPerSec'),
      diskReadLatencyMs: v('diskReadLatencyMs'), diskWriteLatencyMs: v('diskWriteLatencyMs'), diskQueueLength: v('diskQueueLength'), diskBytesPerSec: v('diskBytesPerSec'), cpuFreqPercent: v('cpuFreqPercent'),
      processes: rnd(238, 251), threads: rnd(3300, 3500), handles: rnd(118000, 124000), foregroundPid: 11840,
      top: [
        { pid: 11840, parentPid: 4012, name: 'Code.exe', cpuPercent: 4.2, privateBytes: 690 * MB, workingSet: 540 * MB, hardFaultsPerSec: 0, handles: 1830, threads: 61, sessionId: 1, priorityClass: 32, foreground: true, isSystemCritical: false, isSecuritySoftware: false, isService: false, hasAudio: false, signedByMicrosoft: true },
        { pid: 9120, parentPid: 4012, name: 'OneDrive.exe', cpuPercent: 3.1, privateBytes: 210 * MB, workingSet: 180 * MB, hardFaultsPerSec: 0, handles: 1620, threads: 44, sessionId: 1, priorityClass: 32, foreground: false, isSystemCritical: false, isSecuritySoftware: false, isService: false, hasAudio: false, signedByMicrosoft: true },
        { pid: LEAK_PID, parentPid: 4012, name: 'LeakyHelper.exe', cpuPercent: 0.4, privateBytes: leakBytes(t), workingSet: Math.round(leakBytes(t) * 0.6), hardFaultsPerSec: 3, handles: 410, threads: 12, sessionId: 1, priorityClass: 32, foreground: false, isSystemCritical: false, isSecuritySoftware: false, isService: false, hasAudio: false, signedByMicrosoft: false },
        { pid: 3324, parentPid: 812, name: 'MsMpEng.exe', cpuPercent: 0.9, privateBytes: 260 * MB, workingSet: 230 * MB, hardFaultsPerSec: 0, handles: 980, threads: 38, sessionId: 0, priorityClass: 32, foreground: false, isSystemCritical: true, isSecuritySoftware: true, isService: true, hasAudio: false, signedByMicrosoft: true },
      ],
    };
  }

  // Kernel tracing (docs/core/etw.md): DPC/ISR per driver over the last 10 min, histogram buckets
  // <50, <100, <250, <500, <1000, <2000, <4000, ≥4000 µs.
  const ETW_FLAGS = ['PROCESS', 'THREAD', 'IMAGE_LOAD', 'DPC', 'INTERRUPT', 'DISK_IO', 'HARD_FAULTS'];
  const MID = [25, 72, 160, 340, 680, 1350, 2700, 4600];
  const DRIVERS = [ // name, dpcCount, isrCount, maxUs, histogram
    ['Netwtw10.sys', 18400, 9100, 2400, [21800, 3900, 1300, 310, 140, 38, 12, 0]],
    ['ACPI.sys', 2100, 1300, 1150, [2900, 310, 120, 40, 22, 8, 0, 0]],
    ['nvlddmkm.sys', 42100, 21000, 610, [55200, 6400, 1310, 160, 30, 0, 0, 0]],
    ['ndis.sys', 30500, 0, 420, [28100, 1900, 460, 40, 0, 0, 0, 0]],
    ['ntoskrnl.exe', 51000, 0, 330, [49000, 1700, 270, 30, 0, 0, 0, 0]],
    ['storport.sys', 12800, 0, 310, [11900, 720, 170, 10, 0, 0, 0, 0]],
    ['Wdf01000.sys', 5600, 0, 260, [5200, 300, 90, 10, 0, 0, 0, 0]],
    ['usbxhci.sys', 7200, 7200, 210, [13800, 520, 80, 0, 0, 0, 0, 0]],
    ['dxgkrnl.sys', 9400, 0, 180, [9100, 260, 40, 0, 0, 0, 0, 0]],
    ['tcpip.sys', 8800, 0, 140, [8500, 260, 40, 0, 0, 0, 0, 0]],
    ['stornvme.sys', 0, 12900, 95, [12700, 200, 0, 0, 0, 0, 0, 0]],
    ['HDAudBus.sys', 3000, 3000, 70, [5900, 100, 0, 0, 0, 0, 0, 0]],
  ];
  const etwStatus = () => (sim.etw ? { running: true, flags: [...ETW_FLAGS], eventsLost: 0, costPercent: Math.round((0.35 + 0.1 * noise('etw', Date.now(), 10e3)) * 100) / 100 }
    : { running: false, flags: [], eventsLost: 0, costPercent: 0, reason: 'Turned off by an administrator' });
  function latency() {
    const drivers = !sim.etw ? [] : DRIVERS.map(([name, dpcCount, isrCount, maxUs, histogram]) => ({ name, dpcCount, isrCount,
      totalUs: Math.round(histogram.reduce((a, n, i) => a + n * Math.min(MID[i], maxUs), 0)), maxUs, histogram: [...histogram] }));
    return { simulated: true, status: etwStatus(), system: sim.etw ? { dpcPercent: Math.round(valueAt('dpcPercent', Date.now()) * 100) / 100, isrPercent: Math.round(valueAt('interruptPercent', Date.now()) * 100) / 100, worstDpcUs: 2400, worstIsrUs: 610 } : { dpcPercent: 0, isrPercent: 0, worstDpcUs: 0, worstIsrUs: 0 }, drivers };
  }
  function diskActivity() {
    if (!sim.etw) return { simulated: true, processes: [] };
    const t = Date.now(), j = (k, v) => Math.round(v * (1 + 0.15 * noise(k, t, 4e3)));
    return { simulated: true, processes: [
      { pid: 3324, name: 'MsMpEng.exe', readBps: j('d1', 31 * MB), writeBps: j('d2', 0.4 * MB), ios: j('d3', 38200), meanLatencyMs: 0.9, maxLatencyMs: 14.2 },
      { pid: 9120, name: 'OneDrive.exe', readBps: j('d4', 2.1 * MB), writeBps: j('d5', 8.7 * MB), ios: j('d6', 9100), meanLatencyMs: 1.4, maxLatencyMs: 22.8 },
      { pid: 5520, name: 'SearchIndexer.exe', readBps: j('d7', 3.4 * MB), writeBps: j('d8', 0.9 * MB), ios: j('d9', 6400), meanLatencyMs: 0.8, maxLatencyMs: 9.1 },
      { pid: 4410, name: 'chrome.exe', readBps: j('da', 0.6 * MB), writeBps: j('db', 1.8 * MB), ios: j('dc', 4100), meanLatencyMs: 0.6, maxLatencyMs: 6.3 },
      { pid: 11840, name: 'Code.exe', readBps: j('dd', 0.4 * MB), writeBps: j('de', 0.2 * MB), ios: j('df', 1300), meanLatencyMs: 0.5, maxLatencyMs: 4.0 },
      { pid: 4, name: 'System', readBps: j('dg', 0.3 * MB), writeBps: j('dh', 1.1 * MB), ios: j('di', 2900), meanLatencyMs: 0.7, maxLatencyMs: 11.5 },
      { pid: LEAK_PID, name: 'LeakyHelper.exe', readBps: j('dj', 0.9 * MB), writeBps: 0, ios: j('dk', 2200), meanLatencyMs: 1.1, maxLatencyMs: 8.7 },
    ].sort((a, b) => (b.readBps + b.writeBps) - (a.readBps + a.writeBps)) };
  }
  function hardFaults() {
    if (!sim.etw) return { simulated: true, processes: [] };
    const t = Date.now(), j = (k, v) => Math.max(0, Math.round(v * (1 + 0.25 * noise(k, t, 4e3)) * 10) / 10);
    return { simulated: true, processes: [
      { pid: LEAK_PID, name: 'LeakyHelper.exe', faultsPerSec: j('h1', 12 + 30 * leak(t)) },
      { pid: 4410, name: 'chrome.exe', faultsPerSec: j('h2', 6.1) },
      { pid: 3324, name: 'MsMpEng.exe', faultsPerSec: j('h3', 4.2) },
      { pid: 11840, name: 'Code.exe', faultsPerSec: j('h4', 1.3) },
      { pid: 2210, name: 'explorer.exe', faultsPerSec: j('h5', 0.6) },
    ].sort((a, b) => b.faultsPerSec - a.faultsPerSec) };
  }

  function status() {
    const open = incidents().filter(i => i.state === 'open').length;
    const ticks = Math.floor((Date.now() - sim.ticks0) / 2000);
    return { simulated: true, mode: sim.mode, simulation: sim.mode === 'monitor', engine: { running: true, ticks, lastTickMs: Date.now() - (Date.now() % 2000), tickCostUs: rnd(700, 1400) },
      stabilityScore: 74, scoreReasons: ['Service crash loop: ExampleSyncSvc (−14)', 'Steadily growing memory use: LeakyHelper.exe (−9)', 'No unexpected shutdowns, bugchecks or WHEA errors in 14 days (+0)', 'Storage latency within baseline (+0)'],
      counts: { incidentsOpen: open, policiesActive: sim.policies.length, recommendations: recommendations().length }, heal: { breakerOpen: breaker.open },
      etw: (({ reason, ...e }) => e)(etwStatus()) };
  }

  function nextTx() { return `tx-${++sim.txSeq}`; }
  function command(cmd, args = {}) {
    if (!state.core || !state.core.running) { const e = new Error('Simulated OpenSweep Core is stopped'); e.code = 'unavailable'; throw e; }
    const badArgs = (m) => { const e = new Error(m); e.code = 'bad_args'; return e; };
    if (args && args.limit !== undefined && !(Number.isInteger(args.limit) && args.limit > 0)) throw badArgs('limit must be a positive integer');
    const lim = (a) => Math.min(Number(args && args.limit) || 100, a.length);
    switch (cmd) {
      case 'status': return status();
      case 'sample': return sample();
      case 'history': {
        const d = METRICS[args.metric], r = RANGE[args.range];
        if (!d || !r) { const e = new Error('bad metric or range'); e.code = 'bad_args'; throw e; }
        const [span, step] = r, end = Math.floor(Date.now() / step) * step, points = [];
        for (let t = end - span + step; t <= end; t += step) points.push([t, Math.round(d.v(t, step) * 1000) / 1000]);
        return { simulated: true, metric: args.metric, unit: d.unit, points };
      }
      case 'baselines': {
        const metrics = {};
        for (const [m, [median, mad]] of Object.entries(BASE)) metrics[m] = { median, mad, ewma: median + mad * 0.3, samples: 6170, ready: true };
        metrics.hardFaultsPerSec = { median: 17, mad: 9, ewma: 19, samples: 87, ready: false }; // still learning (needs 120 samples)
        return { simulated: true, metrics };
      }
      case 'incidents': { const a = incidents().sort((x, y) => y.firstSeen - x.firstSeen); return { simulated: true, incidents: a.slice(0, lim(a)) }; }
      case 'events': { const a = events().sort((x, y) => y.tsMs - x.tsMs); return { simulated: true, events: a.slice(0, lim(a)) }; }
      case 'recommendations': return { simulated: true, items: recommendations() };
      case 'policies': return { simulated: true, active: sim.policies.map(p => ({ ...p })) };
      case 'journal': { const a = [...journal].sort((x, y) => y.createdMs - x.createdMs); return { simulated: true, entries: a.slice(0, lim(a)) }; }
      case 'heal': return { simulated: true, breaker: { ...breaker }, pending: approvals.map(a => ({ ...a })), history: journal.filter(j => j.kind === 'recovery').sort((x, y) => y.createdMs - x.createdMs) };
      case 'topology': return { simulated: true, logicalProcessors: 16, cores: 8, packages: 1, numaNodes: 1, groups: 1, hybrid: false, efficiencyClasses: [8], l3Caches: 1,
        cpuSets: [...Array(16)].map((_, i) => ({ id: 256 + i, group: 0, core: i >> 1, efficiencyClass: 0, numa: 0 })) };
      case 'reportForeground': return {};
      case 'latency': return latency();
      case 'diskActivity': return diskActivity();
      case 'hardFaults': return hardFaults();
      case 'setEtw': {
        if (typeof args.enabled !== 'boolean') throw badArgs('enabled must be a boolean');
        if (sim.etw !== args.enabled) {
          sim.etw = args.enabled;
          journal.push(tx({ txId: nextTx(), createdMs: Date.now(), kind: 'config', action: 'set_etw', target: { pid: 0, name: 'kernel tracing', startMs: 0 }, value: args.enabled ? 'on' : 'off', original: args.enabled ? 'off' : 'on', requiredAuth: 'admin', state: 'applied', trigger: 'Changed by an administrator from the OpenSweep app.' }));
        }
        return { simulated: true, enabled: sim.etw, running: sim.etw };
      }
      case 'setMode': {
        if (!['monitor', 'balanced', 'advanced'].includes(args.mode)) throw badArgs('mode must be monitor, balanced or advanced');
        const prev = sim.mode; sim.mode = args.mode;
        journal.push(tx({ txId: nextTx(), createdMs: Date.now(), kind: 'config', action: 'set_mode', target: { pid: 0, name: 'mode', startMs: 0 }, value: args.mode, original: prev, requiredAuth: 'admin', state: 'applied', trigger: 'Changed by an administrator from the OpenSweep app.' }));
        if (args.mode === 'monitor') command('rollbackAll');
        else if (!sim.policies.length) {
          // Simulate the engine applying the one low-risk recommendation, then verifying it.
          const txId = nextTx(), now = Date.now();
          sim.policies.push({ txId, kind: 'ecoqos', target: { pid: 9120, name: 'OneDrive.exe' }, value: 1, original: -1, appliedAt: now, verifyUntil: now + 60e3, state: 'verifying', simulated: true });
          journal.push(tx({ txId, createdMs: now, kind: 'policy', action: 'ecoqos', target: { pid: 9120, name: 'OneDrive.exe', startMs: 0 }, value: 1, original: -1, requiredAuth: 'user', state: 'verifying', trigger: 'Sustained background CPU while the foreground app waited. Verification window 60 s.', verification: 'cpu_queue must drop; rolled back otherwise' }));
        }
        return { simulated: true, mode: sim.mode };
      }
      case 'rollbackAll': {
        const n = sim.policies.length;
        for (const p of sim.policies) { const j = journal.find(t => t.txId === p.txId); if (j) Object.assign(j, { state: 'rolled_back', updatedMs: Date.now(), rollback: 'Rolled back by an administrator (Roll back everything).', verificationResult: 'Original value restored' }); }
        sim.policies = [];
        return { simulated: true, restored: n, failed: 0 };
      }
      case 'disablePolicy': {
        if (args.txId === undefined) throw badArgs('txId is required');
        const i = sim.policies.findIndex(p => p.txId === args.txId);
        if (i < 0) return { simulated: true, restored: false };
        const [p] = sim.policies.splice(i, 1);
        const j = journal.find(t => t.txId === p.txId); if (j) Object.assign(j, { state: 'rolled_back', updatedMs: Date.now(), rollback: 'Disabled by an administrator.', verificationResult: 'Original value restored' });
        return { simulated: true, restored: true };
      }
      case 'approve': {
        const i = approvals.findIndex(a => a.id === args.id);
        if (i < 0) { const e = new Error('No such pending approval'); e.code = 'bad_args'; throw e; }
        const [a] = approvals.splice(i, 1), txId = nextTx();
        journal.push(tx({ txId, createdMs: Date.now(), kind: 'recovery', action: a.action, target: { pid: 0, name: a.target, startMs: 0 }, value: 0, original: null, requiredAuth: 'admin', state: 'applied', trigger: `Approved by an administrator: ${a.trigger}`, verificationResult: 'Simulated — nothing was restarted' }));
        return { simulated: true, txId };
      }
      case 'exportDiagnostics': return { simulated: true, path: `C:\\ProgramData\\OpenSweep\\core\\diagnostics-${new Date().toISOString().replace(/[:.]/g, '-')}.json` };
      default: { const e = new Error(`unknown command ${cmd}`); e.code = 'unknown_cmd'; throw e; }
    }
  }
  return { command, status, get mode() { return sim.mode; } };
})();

module.exports = {
  demo: true,

  async systemInfo() {
    await wait(300);
    return {
      os: 'Microsoft Windows 11 Pro', displayVersion: '24H2', version: '10.0.26100', build: '26100', arch: '64-bit',
      installDate: '2025-03-14T10:22:00', lastBoot: new Date(Date.now() - 9 * 86400e3).toISOString().slice(0, 19),
      manufacturer: 'Demo Computers', model: 'Gaming Desktop', computerName: 'DEMO-PC', user: 'you',
      cpu: 'AMD Ryzen 7 7800X3D 8-Core Processor', cores: 8, threads: 16, cpuMHz: 4200, ramTotal: 32 * GB,
      gpu: ['NVIDIA GeForce RTX 4070 SUPER'], isAdmin: false, bios: 'American Megatrends 1.40', board: 'ASUS TUF GAMING B650-PLUS',
      disks: [{ name: 'C:', label: 'Windows', total: 953 * GB, free: 74 * GB, fs: 'NTFS' }, { name: 'D:', label: 'Games', total: 1863 * GB, free: 612 * GB, fs: 'NTFS' }],
      physicalDisks: [{ name: 'Samsung SSD 990 PRO 1TB', media: 'SSD', bus: 'NVMe', health: 'Healthy', size: 1000 * 1e9 }, { name: 'WD Blue SN580 2TB', media: 'SSD', bus: 'NVMe', health: 'Healthy', size: 2000 * 1e9 }],
      battery: null,
    };
  },

  async processes() {
    await wait(250);
    if (!state.procs) state.procs = PROC_NAMES.map(([name, company], i) => ({ pid: 1000 + i * 37, name, company, path: `C:\\Program Files\\${name}\\${name}.exe`, title: '' }));
    return state.procs.map(p => ({ ...p, memMB: rnd(20, p.name === 'chrome' ? 900 : 400), cpu: Math.round(Math.random() * (p.name === 'chrome' ? 12 : 3) * 10) / 10 }));
  },
  async killProcess({ pid }) { await wait(200); state.procs = state.procs.filter(p => p.pid !== pid); return { ok: true }; },

  async boost() { await wait(1500); return { memFreedMB: rnd(600, 2400), processesTrimmed: rnd(80, 160), tempFreed: rnd(50, 400) * MB }; },

  async cleanupScan({ ids } = {}) {
    await wait(1400);
    return C.CLEANUP.filter(c => !ids || ids.includes(c.id)).map(c => ({ id: c.id, size: state.junk[c.id], files: Math.round(state.junk[c.id] / (180 * 1024)) }));
  },
  async cleanupRun({ ids }) {
    await wait(1800);
    return ids.map(id => { const freed = state.junk[id] * 0.97; state.junk[id] = 0; return { id, freed, skipped: rnd(0, 6) }; });
  },

  async largeFiles() {
    await wait(1200);
    const f = [['Videos\\OBS\\2025-10-02 21-14-55.mkv', 7.8 * GB], ['Downloads\\Win11_24H2_English_x64.iso', 5.4 * GB], ['Downloads\\ubuntu-24.04-desktop-amd64.iso', 5.7 * GB], ['Documents\\Backups\\phone-backup-2024.zip', 3.1 * GB], ['AppData\\Local\\Docker\\wsl\\data\\ext4.vhdx', 22.4 * GB], ['Videos\\Captures\\Valorant 2025-09-12.mp4', 1.9 * GB], ['Downloads\\DaVinci_Resolve_19_Windows.zip', 3.4 * GB], ['Downloads\\setup_cyberpunk_patch.exe', 1.2 * GB], ['Pictures\\RAW\\DSC_0412.NEF', 0.11 * GB]];
    return f.map(([p, s], i) => ({ path: `C:\\Users\\you\\${p}`, size: s, modified: `2025-0${(i % 9) + 1}-1${i}T12:00:00`, accessed: '2025-10-01T12:00:00', ext: p.slice(p.lastIndexOf('.')).toLowerCase() })).sort((a, b) => b.size - a.size);
  },
  async duplicates() {
    await wait(1600);
    return [
      { hash: 'A1', size: 1.2 * GB, files: [{ path: 'C:\\Users\\you\\Downloads\\setup_cyberpunk_patch.exe', modified: '2025-05-01T10:00:00' }, { path: 'C:\\Users\\you\\Downloads\\setup_cyberpunk_patch (1).exe', modified: '2025-05-02T10:00:00' }] },
      { hash: 'B2', size: 240 * MB, files: [{ path: 'C:\\Users\\you\\Videos\\clip.mp4', modified: '2025-01-01T10:00:00' }, { path: 'C:\\Users\\you\\Desktop\\clip.mp4', modified: '2025-01-03T10:00:00' }, { path: 'C:\\Users\\you\\OneDrive\\clip - Copy.mp4', modified: '2025-02-01T10:00:00' }] },
      { hash: 'C3', size: 8.4 * MB, files: [{ path: 'C:\\Users\\you\\Pictures\\IMG_2231.JPG', modified: '2024-08-01T10:00:00' }, { path: 'C:\\Users\\you\\Pictures\\Phone\\IMG_2231.JPG', modified: '2024-08-01T10:00:00' }] },
    ];
  },
  async folderSizes() {
    await wait(900);
    return [['AppData', 61], ['Videos', 48], ['Downloads', 37], ['Documents', 12], ['Pictures', 9], ['OneDrive', 6], ['Music', 3], ['Desktop', 1.2]].map(([n, g]) => ({ name: n, path: `C:\\Users\\you\\${n}`, size: g * GB }));
  },

  async installedApps() {
    await wait(700);
    const d = [['Google Chrome', 'Google LLC', '131.0', 450], ['Discord', 'Discord Inc.', '1.0.9170', 380], ['Steam', 'Valve Corporation', '2.10', 900], ['Spotify', 'Spotify AB', '1.2.50', 320], ['Visual Studio Code', 'Microsoft Corporation', '1.95.3', 380], ['7-Zip 24.08 (x64)', 'Igor Pavlov', '24.08', 6], ['NVIDIA Graphics Driver', 'NVIDIA Corporation', '566.14', 1300], ['McAfee LiveSafe', 'McAfee, LLC', '16.0', 820], ['WinRAR 7.01', 'win.rar GmbH', '7.01', 9], ['Adobe Acrobat (64-bit)', 'Adobe', '24.4', 1100], ['ExpressVPN', 'ExpressVPN', '12.88', 210], ['Notion', 'Notion Labs, Inc', '4.2', 290]];
    const desktop = d.map(([name, publisher, version, mb], i) => ({ id: 'w:' + i, kind: 'desktop', name, publisher, version, sizeKB: mb * 1024, installDate: `2025${String((i % 9) + 1).padStart(2, '0')}15` }));
    const store = C.BLOAT_APPS.filter(a => state.installed.has(a.pkg)).map(a => ({ id: 's:' + a.pkg, kind: 'store', pkg: a.pkg, name: a.pkg, publisher: 'CN=Microsoft Corporation', version: '1.0.0.0', sizeKB: null }));
    return desktop.concat(store);
  },
  async uninstallApp(app) { await wait(600); if (app.kind === 'store') state.installed.delete(app.pkg); return { ok: true, interactive: app.kind !== 'store' }; },

  async startupItems() { await wait(400); return state.startup.map(s => ({ ...s })); },
  async setStartup(item, enabled) { await wait(300); const s = state.startup.find(x => x.id === item.id); if (s) s.enabled = enabled; return { ok: true }; },

  async security() {
    await wait(500);
    return {
      defender: { realtime: true, antivirus: true, signatureAge: 4, signatureVersion: '1.421.112.0', quickScanAge: 12, fullScanAge: 210, tamper: true, behavior: true, network: true },
      firewall: [{ name: 'Domain', enabled: true }, { name: 'Private', enabled: true }, { name: 'Public', enabled: false }],
      uac: true, lastUpdate: new Date(Date.now() - 41 * 86400e3).toISOString().slice(0, 19), antivirus: ['Windows Defender'], smartscreen: 'Warn', bitlocker: 'Off', rdpEnabled: false, smb1: false,
    };
  },
  async defenderAction({ action }) { await wait(action === 'quick' ? 4000 : 1200); return { ok: true }; },
  async threats() { await wait(300); return [{ name: 'PUA:Win32/Presenoker', severity: 2, when: '2025-09-30T14:02:11', resources: ['file:_C:\\Users\\you\\Downloads\\free_converter_setup.exe'], action: true }]; },

  async network() {
    await wait(400);
    return [{ index: 12, name: 'Ethernet', desc: 'Realtek PCIe 2.5GbE Family Controller', ipv4: ['192.168.1.42'], gateway: ['192.168.1.1'], dns: ['192.168.1.1'], speed: '2.5 Gbps', mac: '00-1A-2B-3C-4D-5E' },
      { index: 18, name: 'Wi-Fi', desc: 'Intel(R) Wi-Fi 6E AX211', ipv4: ['192.168.1.77'], gateway: ['192.168.1.1'], dns: ['1.1.1.1', '1.0.0.1'], speed: '1.2 Gbps', mac: '00-1A-2B-3C-4D-5F' }];
  },
  async networkAction() { await wait(700); return { ok: true }; },

  async tweakStates() { await wait(350); return { ...state.tweaks }; },
  async setTweaks(changes) {
    await wait(600);
    for (const c of changes) if (c.id !== 'ai.recall-off') state.tweaks[c.id] = !!c.on; // one sample failure so the verify UI can be seen
    return this.verifyTweaks(changes);
  },
  async verifyTweaks(changes) {
    const failed = changes.filter(c => !!state.tweaks[c.id] !== !!c.on).map(c => ({ id: c.id, title: (C.TWEAKS.find(t => t.id === c.id) || {}).title || c.id, error: 'Demo: pretend a policy is overriding this one.' }));
    return { ok: !failed.length, applied: changes.length - failed.length, failed, states: { ...state.tweaks } };
  },

  async bloatStatus() { await wait(400); return [...state.installed]; },
  async removeApps({ pkgs }) { await wait(1500); pkgs.forEach(p => state.installed.delete(p)); return pkgs.map(pkg => ({ pkg, ok: true })); },
  async servicesStatus() { await wait(300); return C.SERVICES.map(s => ({ name: s.name, status: state.services[s.name] === 'Disabled' ? 'Stopped' : 'Running', startType: state.services[s.name] })); },
  async setServices(changes) { await wait(800); changes.forEach(c => { state.services[c.name] = c.type; }); return changes.map(c => ({ name: c.name, ok: true, error: null })); },
  async tasksStatus() { await wait(300); return C.SCHEDULED_TASKS.map(t => ({ id: t.path + t.name, state: state.tasks[t.path + t.name] })); },
  async setTasks(changes) { await wait(600); changes.forEach(c => { state.tasks[c.path + c.name] = c.enable ? 'Ready' : 'Disabled'; }); return { ok: true, results: changes.map(c => ({ id: c.path + c.name, ok: true, error: null })) }; },

  async updatesStatus() {
    await wait(400);
    const u = state.updates || (state.updates = { pausedUntil: null, activeStart: 8, activeEnd: 17, lockedVersion: null });
    return { ...u, displayVersion: '24H2', product: 'Windows 11', rebootPending: true,
      hotfixes: [['KB5046617', 'Security Update', 6], ['KB5046740', 'Update', 20], ['KB5044284', 'Security Update', 35], ['KB5043080', 'Security Update', 62], ['KB5042352', 'Update', 70]].map(([kb, desc, d]) => ({ kb, desc, installed: new Date(Date.now() - d * 86400e3).toISOString().slice(0, 19) })) };
  },
  async updatesAction({ action, days, start, end, version }) {
    await wait(500);
    const u = state.updates || (state.updates = {});
    if (action === 'pause') u.pausedUntil = new Date(Date.now() + days * 86400e3).toISOString().slice(0, 19) + 'Z';
    if (action === 'resume') u.pausedUntil = null;
    if (action === 'active-hours') { u.activeStart = start; u.activeEnd = end; }
    if (action === 'lock') u.lockedVersion = version;
    if (action === 'unlock') u.lockedVersion = null;
    return { ok: true };
  },
  async powerPlans() { await wait(200); return [{ guid: '381b4222-f694-41f0-9685-ff5bb260df2e', name: 'Balanced', active: true }, { guid: '8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c', name: 'High performance', active: false }, { guid: 'a1841308-3541-4fab-bc81-f71556f20b4a', name: 'Power saver', active: false }]; },
  async setPowerPlan() { await wait(200); return { ok: true }; },
  async runRepair() { await wait(800); return { ok: true }; },
  async launchTool() { await wait(150); return { ok: true }; },
  async isAdmin() { return true; },
  async stabilityCheck({ days = 7 } = {}) {
    await wait(900);
    const t = (h) => new Date(Date.now() - h * 3600e3).toISOString().slice(0, 19);
    return { checkedAt: t(0), days,
      devices: [{ id: 'USB\\VID_0BDA&PID_8771\\5&1A2B', name: 'Generic Bluetooth Adapter', cls: 'Bluetooth', code: 43, problem: 'Device reported a problem' }],
      events: [
        { key: 'Display|4101|9001', when: t(20), kind: 'Display driver reset', source: 'Display', eventId: 4101, message: 'Display driver nvlddmkm stopped responding and has successfully recovered.' },
        { key: 'Kernel-Power|41|8800', when: t(70), kind: 'Unexpected shutdown', source: 'Microsoft-Windows-Kernel-Power', eventId: 41, message: 'The system has rebooted without cleanly shutting down first.' },
      ],
      oldDrivers: [{ name: 'Realtek High Definition Audio', provider: 'Realtek', version: '6.0.1.7548', date: '2015-07-01' }],
      unsignedDrivers: [], thirdPartyDrivers: 41 };
  },
  async scanDevices() { await wait(800); return { ok: true }; },
  async setAutoStart() { await wait(200); return { ok: true }; },

  // ---------------- v0.3 features (sample data)
  async reliability({ days = 30 } = {}) {
    await wait(700);
    const { summarize } = require('./win-reliability');
    const recs = [];
    const day = (k, h = 12) => new Date(Date.now() - k * 86400e3 - h * 3600e3).toISOString().slice(0, 19);
    const add = (k, src, ev, product, msg) => recs.push({ id: String(recs.length), t: day(k, rnd(1, 20)), src, ev, product, msg });
    [1, 3, 3, 9, 15, 15, 22].forEach(k => add(k, 'Application Error', 1000, 'Discord', 'Faulting application name: Discord.exe, version: 1.0.9170'));
    [5, 12].forEach(k => add(k, 'Application Hang', 1002, 'Explorer', 'The program explorer.exe stopped interacting with Windows.'));
    add(8, 'Microsoft-Windows-WER-SystemErrorReporting', 1001, '', 'The computer has rebooted from a bugcheck. The bugcheck was: 0x00000116 (nvlddmkm.sys)');
    add(8, 'Microsoft-Windows-Kernel-Power', 41, '', 'The system has rebooted without cleanly shutting down first.');
    add(2, 'Microsoft-Windows-WindowsUpdateClient', 20, '2026-11 Cumulative Update for Windows 11 (KB5046617)', 'Installation Failure: 0x800f0922');
    for (let k = 0; k < days; k += 3) add(k, 'Microsoft-Windows-WindowsUpdateClient', 19, 'Security Intelligence Update for Microsoft Defender Antivirus', 'Installation Successful');
    add(6, 'MsiInstaller', 11707, 'Steam', 'Product: Steam -- Installation completed successfully.');
    const index = []; let v = 4.2;
    for (let k = days - 1; k >= 0; k--) { const hit = recs.filter(r => r.t.startsWith(day(k).slice(0, 10)) && /Error|Hang|WER|Kernel/.test(r.src)).length; v = Math.max(1, Math.min(10, v + 0.35 - hit * 1.1)); index.push({ t: day(k, 0), v: Math.round(v * 100) / 100 }); }
    return summarize({ index, records: recs }, days);
  },
  startProcessStream(onData) {
    const tick = async () => { const procs = await this.processes(); onData({ t: new Date().toISOString(), procs: procs.map(p => ({ ...p, mem: p.memMB, io: rnd(0, p.name === 'chrome' ? 4e6 : 2e5), threads: rnd(8, 90), desc: p.name, sig: p.name === 'Notion' ? 'NotSigned' : 'Valid', pri: 'Normal', path: p.name === 'Notion' ? 'C:\\Users\\you\\AppData\\Local\\Temp\\notion\\Notion.exe' : p.path })) }); };
    tick(); const t = setInterval(tick, 2000); return () => clearInterval(t);
  },
  async setPriority() { await wait(150); return { ok: true }; },
  async oldDrivers() { await wait(900); return [{ inf: 'oem41.inf', name: 'nv_dispi.inf', provider: 'NVIDIA', cls: 'Display', version: '32.0.15.6094', date: '2024-08-14', size: 1.1 * GB, newest: '32.0.15.6614' }, { inf: 'oem12.inf', name: 'nv_dispi.inf', provider: 'NVIDIA', cls: 'Display', version: '31.0.15.5222', date: '2024-03-02', size: 1.05 * GB, newest: '32.0.15.6614' }, { inf: 'oem7.inf', name: 'rt640x64.inf', provider: 'Realtek', cls: 'Net', version: '10.68.0', date: '2023-10-01', size: 4 * MB, newest: '10.72.0' }]; },
  async removeDrivers(infs) { await wait(1200); return infs.map((inf, i) => ({ inf, ok: i !== 1, msg: '' })); },
  async windowsOldSize() { await wait(300); return { exists: true, size: 18.6 * GB }; },
  async removeWindowsOld() { await wait(2000); return { ok: true }; },
  async componentCleanup() { await wait(2500); return { ok: true }; },
  oldInstallers() { return [['Win11_24H2_English_x64.iso', 5.4 * GB], ['DaVinci_Resolve_19_Windows.zip', 3.4 * GB], ['SteamSetup.exe', 2.3 * MB], ['ChromeSetup.exe', 1.4 * MB], ['vlc-3.0.21-win64.exe', 44 * MB]].map(([n, s], i) => ({ path: `C:\\Users\\you\\Downloads\\${n}`, name: n, size: s, modified: new Date(Date.now() - (60 + i * 30) * 86400e3).toISOString() })); },
  emptyFolders() { return ['Documents\\New folder', 'Documents\\New folder (2)', 'Pictures\\Screenshots\\old', 'Desktop\\stuff'].map(p => `C:\\Users\\you\\${p}`); },
  async compressPaths(paths) { await wait(800); return paths.map(p => ({ path: p, ok: true })); },
  async setCleanSchedule() { await wait(200); return { ok: true }; },
  async programs() {
    await wait(800);
    const d = [['Google Chrome', 'Google LLC', '131.0', 450, 0], ['Discord', 'Discord Inc.', '1.0.9170', 380, 1], ['Steam', 'Valve Corporation', '2.10', 900, 3], ['Spotify', 'Spotify AB', '1.2.50', 320, 2], ['Visual Studio Code', 'Microsoft Corporation', '1.95.3', 380, 0],
      ['7-Zip 24.08 (x64)', 'Igor Pavlov', '24.08', 6, 40], ['NVIDIA Graphics Driver', 'NVIDIA Corporation', '566.14', 1300, null], ['McAfee LiveSafe', 'McAfee, LLC', '16.0', 820, 400], ['WinRAR 7.01', 'win.rar GmbH', '7.01', 9, 220], ['Adobe Acrobat (64-bit)', 'Adobe', '24.4', 1100, 95],
      ['ExpressVPN', 'ExpressVPN', '12.88', 210, 260], ['Notion', 'Notion Labs, Inc', '4.2', 290, 5], ['OneLaunch', 'OneLaunch', '5.32', 160, 500], ['Driver Booster 11', 'IObit', '11.6', 95, 300]];
    const desktop = d.map(([name, publisher, version, mb, used], i) => ({ id: 'w:' + i, kind: 'desktop', name, publisher, version, sizeKB: mb * 1024, installDate: `2025${String((i % 9) + 1).padStart(2, '0')}15`, uninstall: i % 3 ? `"C:\\Program Files\\${name}\\unins000.exe"` : `MsiExec.exe /X{${'0'.repeat(8)}-0000-0000-0000-${String(i).padStart(12, '0')}}`, location: `C:\\Program Files\\${name}`, regKey: `HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\${name}`, lastUsed: used === null ? null : new Date(Date.now() - used * 86400e3).toISOString().slice(0, 19) }));
    const store = C.BLOAT_APPS.filter(a => state.installed.has(a.pkg)).map(a => ({ id: 's:' + a.pkg, kind: 'store', pkg: a.pkg, name: a.name, publisher: 'Microsoft Corporation', version: '1.0.0.0', sizeKB: null, lastUsed: null }));
    return desktop.filter(a => !(state.removed || new Set()).has(a.id)).concat(store);
  },
  silentCommand(a) { return a.kind === 'store' || /unins|MsiExec/i.test(a.uninstall || '') ? 'silent' : null; },
  async runUninstall(a) { await wait(1500); (state.removed = state.removed || new Set()).add(a.id); if (a.kind === 'store') state.installed.delete(a.pkg); return { ok: true, gone: true }; },
  async stillInstalled(a) { return !(state.removed || new Set()).has(a.id); },
  async leftoverScan(a) {
    await wait(1400);
    const n = a.name.replace(/\s*\d.*$/, '');
    const items = [
      { type: 'dir', path: `C:\\Program Files\\${n}`, level: 'safe', detail: 'Install folder', size: 48 * MB },
      { type: 'dir', path: `C:\\Users\\you\\AppData\\Roaming\\${n}`, level: 'safe', detail: 'Named after the app', size: 212 * MB },
      { type: 'dir', path: `C:\\Users\\you\\AppData\\Local\\${n}`, level: 'safe', detail: 'Named after the app', size: 640 * MB },
      { type: 'reg', path: `HKEY_CURRENT_USER\\Software\\${n}`, level: 'safe', detail: 'Settings key named after the app', size: 0 },
      { type: 'run', path: `HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run|${n}`, level: 'safe', detail: `Starts with Windows: "C:\\Users\\you\\AppData\\Local\\${n}\\Update.exe" --processStart`, size: 0 },
      { type: 'task', path: `\\${n}UpdateTaskMachine`, level: 'likely', detail: `Scheduled task: ${n}UpdateTaskMachine`, size: 0 },
      { type: 'dir', path: `C:\\ProgramData\\${(a.publisher || n).split(/[ ,]/)[0]}`, level: 'careful', detail: 'Publisher folder — may be shared with other apps', size: 3 * MB },
    ];
    return items.map((x, i) => ({ ...x, id: `${x.type}:${i}:${x.path}` }));
  },
  async removeLeftovers(items) { await wait(900); return { backup: 'C:\\ProgramData\\OpenSweep\\backups\\demo', results: items.map(i => ({ id: i.id, ok: true })) }; },
  async restoreBackup() { await wait(500); return { ok: true, restored: 2 }; },
  BACKUP_ROOT: () => 'C:\\ProgramData\\OpenSweep\\backups',
  async orphanScan() {
    await wait(1800);
    return [['C:\\Users\\you\\AppData\\Roaming\\Zoom', 412, 'Last changed 2024-02-11'], ['C:\\Program Files (x86)\\Origin', 1900, 'Last changed 2023-06-30'], ['C:\\Users\\you\\AppData\\Local\\Battle.net', 96, 'Last changed 2024-09-02'], ['C:\\ProgramData\\Epic', 64, 'Last changed 2025-01-12']]
      .map(([p, mb, d], i) => ({ id: `orphan:${i}:${p}`, type: 'dir', path: p, size: mb * MB, level: 'careful', detail: d }));
  },
  async browserExtensions() {
    await wait(500);
    return [['Chrome', 'uBlock Origin', '1.61.2', ['storage', 'tabs', '<all_urls>'], false], ['Chrome', 'Google Docs Offline', '1.84', ['storage'], false], ['Edge', 'Coupon Finder Pro', '3.2', ['tabs', 'webRequest', '<all_urls>', 'history'], true], ['Firefox', 'Bitwarden', '2024.11', ['storage', 'clipboardWrite'], false]]
      .map(([b, n, v, perm, forced], i) => ({ id: `${b}|Default|ext${i}`, browser: b, profile: 'Default', extId: `ext${i}`, name: n, version: v, desc: '', permissions: perm, forced, path: forced ? null : 'x' }));
  },
  async removeForcedExtension() { await wait(400); return { ok: true }; },
  async optionalFeatures() { await wait(900); return Object.keys(C.FEATURE_NAMES).map((name, i) => ({ name, state: [0, 3, 6, 9].includes(i) ? 'Enabled' : 'Disabled' })); },
  async setFeature({ enable }) { await wait(1500); return { ok: true, state: enable ? 'Enabled' : 'Disabled', restart: true }; },
  async trackStart(label) { await wait(1200); state.track = { id: 'demo-track', label, started: new Date().toISOString() }; return state.track; },
  async trackFinish() { await wait(1200); const t = { ...state.track, finished: new Date().toISOString(), diff: { dirs: ['C:\\Program Files\\CoolApp', 'C:\\Users\\you\\AppData\\Roaming\\CoolApp'], files: ['C:\\Users\\Public\\Desktop\\CoolApp.lnk'], reg: ['HKEY_CURRENT_USER\\Software\\CoolApp'], run: ['HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run|CoolAppHelper'], services: ['CoolAppUpdater'], tasks: [], firewall: [] } }; state.tracks = [t]; return t; },
  tracks() { return state.tracks || []; },
  trackItems(t) { return require('./win-uninstall').trackItems(t); },
  async hunt() { await wait(1500); return { pid: 1037, name: 'Discord', path: 'C:\\Program Files\\Discord\\Discord.exe', title: 'Discord', company: 'Discord Inc.' }; },
  async wingetUpgrades() { await wait(1200); return [{ name: 'Google Chrome', id: 'Google.Chrome', version: '130.0.6723.117', available: '131.0.6778.86' }, { name: '7-Zip', id: '7zip.7zip', version: '23.01', available: '24.08' }, { name: 'Discord', id: 'Discord.Discord', version: '1.0.9160', available: '1.0.9170' }]; },
  async wingetUpgrade() { await wait(2500); return { ok: true }; },

  // ---------------- OpenSweep Core (SIMULATED — no native service exists in demo mode)
  async coreStatus() {
    await wait(200);
    const c = state.core || (state.core = { installed: true, running: true, startedAt: Date.now() - 3 * 3600e3 - 17 * 60e3, requests: 1240, cpuMs: 4100 });
    const up = Math.round((Date.now() - c.startedAt) / 1000);
    if (c.running) { c.requests += rnd(1, 4); c.cpuMs += rnd(5, 30); }
    return {
      available: false, demo: true, simulated: true, exePresent: true, installed: c.installed, running: c.running,
      serviceState: !c.installed ? 'not_installed' : c.running ? 'running' : 'stopped',
      hello: c.running ? { service: 'OpenSweepCore', version: '0.1.0', protocol: 1, pid: 4312, mode: coreSim.mode, startedAt: new Date(c.startedAt).toISOString(), uptimeSec: up } : null,
      health: c.running ? { status: 'running', mode: coreSim.mode, uptimeSec: up, workingSetBytes: 9.4 * MB, privateBytes: 5.2 * MB, cpuTimeMs: c.cpuMs, clients: 1, requests: c.requests, errors: 0 } : null,
      status: c.running ? coreSim.status() : null,
    };
  },
  async coreMetrics() {
    if (!state.core || !state.core.running) { const e = new Error('Simulated OpenSweep Core is stopped'); e.code = 'unavailable'; throw e; }
    const total = 32 * GB, load = rnd(38, 52), limit = 37.9 * GB, commit = Math.round((14 + Math.random() * 3) * GB);
    return { simulated: true, timestamp: new Date().toISOString(), cpu: { percent: Math.round(Math.random() * 180) / 10 + 3 },
      memory: { totalPhys: total, availPhys: Math.round(total * (1 - load / 100)), loadPercent: load, commitTotal: commit, commitLimit: limit, commitPeak: 22.4 * GB, systemCache: 6.2 * GB, kernelPaged: 610 * MB, kernelNonpaged: 280 * MB },
      system: { processes: rnd(238, 251), threads: rnd(3300, 3500), handles: rnd(118000, 124000) }, pageSize: 4096 };
  },
  async coreTopology() {
    await wait(100);
    return coreSim.command('topology'); // Phase 1 shape + cpuSets
  },
  async coreCommand(cmd, args) { await wait(80); return coreSim.command(cmd, args || {}); },
  async coreAction(action) {
    await wait(900);
    const c = state.core || (state.core = { installed: true, running: true, startedAt: Date.now(), requests: 0, cpuMs: 0 });
    if (action === 'install') { c.installed = true; }
    if (action === 'start') { if (!c.installed) throw new Error('Simulated: the service is not installed'); if (!c.running) { c.running = true; c.startedAt = Date.now(); c.requests = 0; c.cpuMs = 0; } }
    if (action === 'stop') c.running = false;
    if (action === 'uninstall') { c.installed = false; c.running = false; }
    return { ok: true, simulated: true };
  },
};
