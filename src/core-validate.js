'use strict';
// Validation of renderer-supplied arguments for OpenSweep Core IPC methods (pure, no Electron, no I/O).
// The renderer only names a method from CORE_METHODS; this module maps it to exactly one Core command
// (docs/core/phases-2-8.md, "IPC additions") and rebuilds the args from scratch so nothing the renderer
// sent is forwarded unless it passed a strict check. Anything else throws an Error with code 'bad_args'.

const MODES = Object.freeze(['monitor', 'balanced', 'advanced']);
const RANGES = Object.freeze(['30m', '24h', '14d']);
// History metrics: the numeric fields of telemetry::Sample (camelCase), see core/src/portable/telemetry.hpp.
const METRICS = Object.freeze([
  'cpuPercent', 'cpuQueueLength', 'contextSwitchesPerSec', 'dpcPercent', 'interruptPercent',
  'memTotal', 'memAvail', 'memLoadPercent', 'commitTotal', 'commitLimit', 'systemCache', 'kernelPaged', 'kernelNonpaged',
  'compressedBytes', 'hardFaultsPerSec', 'pageFaultsPerSec', 'diskReadLatencyMs', 'diskWriteLatencyMs', 'diskQueueLength',
  'diskBytesPerSec', 'cpuFreqPercent', 'processes', 'threads', 'handles',
]);
const ID_RE = /^[A-Za-z0-9][A-Za-z0-9_.:-]{0,63}$/;
const LIMIT_MAX = 500;
const PID_MAX = 0xffffffff;

function badArgs(message) { const e = new Error(message); e.code = 'bad_args'; return e; }
const isObj = (x) => x !== null && typeof x === 'object' && !Array.isArray(x);

function oneOf(x, list, what) { if (typeof x !== 'string' || !list.includes(x)) throw badArgs(`Invalid ${what}`); return x; }
// Optional positive integer limit (1..500). Absent / undefined → not sent.
function limit(x) {
  if (x === undefined || x === null) return undefined;
  if (!Number.isSafeInteger(x) || x < 1 || x > LIMIT_MAX) throw badArgs('Invalid limit');
  return x;
}
// Transaction / approval ids: a positive safe integer or a short token of [A-Za-z0-9_.:-].
function id(x, what = 'id') {
  if (Number.isSafeInteger(x) && x > 0) return x;
  if (typeof x === 'string' && ID_RE.test(x)) return x;
  throw badArgs(`Invalid ${what}`);
}
function bool(x, what) { if (typeof x !== 'boolean') throw badArgs(`Invalid ${what}`); return x; }
function pid(x) {
  if (!Number.isSafeInteger(x) || x <= 4 || x > PID_MAX) throw badArgs('Invalid process id');
  return x;
}

// cap: 'read' | 'admin'. args(a) returns the exact object sent to Core (or undefined for none).
// confirm(clean) (admin only) returns the text of the confirmation dialog shown by the main process.
const CORE_METHODS = Object.freeze({
  coreSample: { cmd: 'sample', cap: 'read' },
  coreHistory: { cmd: 'history', cap: 'read', args: (a) => ({ metric: oneOf(a.metric, METRICS, 'metric'), range: oneOf(a.range, RANGES, 'range') }) },
  coreBaselines: { cmd: 'baselines', cap: 'read' },
  coreIncidents: { cmd: 'incidents', cap: 'read', args: (a) => ({ limit: limit(a.limit) }) },
  coreEvents: { cmd: 'events', cap: 'read', args: (a) => ({ limit: limit(a.limit) }) },
  coreRecommendations: { cmd: 'recommendations', cap: 'read' },
  corePolicies: { cmd: 'policies', cap: 'read' },
  coreJournal: { cmd: 'journal', cap: 'read', args: (a) => ({ limit: limit(a.limit) }) },
  coreHeal: { cmd: 'heal', cap: 'read' },
  // Kernel tracing (docs/core/etw.md, "IPC additions").
  coreLatency: { cmd: 'latency', cap: 'read' },
  coreDiskActivity: { cmd: 'diskActivity', cap: 'read' },
  coreHardFaults: { cmd: 'hardFaults', cap: 'read' },
  coreSetMode: { cmd: 'setMode', cap: 'admin', args: (a) => ({ mode: oneOf(a.mode, MODES, 'mode') }) },
  coreRollbackAll: { cmd: 'rollbackAll', cap: 'admin' },
  coreDisablePolicy: { cmd: 'disablePolicy', cap: 'admin', args: (a) => ({ txId: id(a.txId, 'transaction id') }) },
  coreApprove: { cmd: 'approve', cap: 'admin', args: (a) => ({ id: id(a.id, 'approval id') }) },
  coreExportDiagnostics: { cmd: 'exportDiagnostics', cap: 'admin' },
  coreSetEtw: { cmd: 'setEtw', cap: 'admin', args: (a) => ({ enabled: bool(a.enabled, 'enabled') }) },
});

// Returns { cmd, cap, args } for a renderer method, or throws bad_args. Unknown keys are dropped;
// optional keys that are absent are not sent.
function prepare(method, input) {
  if (typeof method !== 'string' || !Object.prototype.hasOwnProperty.call(CORE_METHODS, method)) throw badArgs('Unknown Core method');
  const spec = CORE_METHODS[method];
  const a = input === undefined || input === null ? {} : input;
  if (!isObj(a)) throw badArgs('Arguments must be an object');
  let args;
  if (spec.args) {
    args = {};
    for (const [k, v] of Object.entries(spec.args(a))) if (v !== undefined) args[k] = v;
    if (!Object.keys(args).length) args = undefined;
  }
  return { cmd: spec.cmd, cap: spec.cap, args };
}

const MODE_TEXT = Object.freeze({
  monitor: 'Monitor: OpenSweep Core only measures and records. It shows what it would do (simulated) and never changes Windows settings. Any policy that is currently applied is rolled back.',
  balanced: 'Balanced: OpenSweep Core may apply only low-risk, reversible actions from the allow-list (EcoQoS, lower memory priority, below-normal priority) to verified background processes. Every change is journaled first, measured for 60 seconds and rolled back automatically if it does not help. Foreground, system, security and audio processes are never touched.',
  advanced: 'Advanced: adds experimental actions (for example moving background work to efficiency cores on hybrid CPUs). Each such action still needs your separate approval, is journaled and is rolled back automatically. It does not bypass any Windows security protection. Only use this if you are comfortable reviewing each intervention.',
});

// Text for the native confirmation dialog of an admin method. `ctx` carries main-process lookups
// (e.g. the policy or approval being acted on) so the dialog states exactly what will happen.
function describeAdmin(method, args, ctx = {}) {
  const t = (s) => String(s === undefined || s === null ? '' : s).slice(0, 300);
  switch (method) {
    case 'coreSetMode':
      return { title: 'Change OpenSweep Core mode', message: `Switch OpenSweep Core to ${args.mode[0].toUpperCase()}${args.mode.slice(1)} mode?`, detail: MODE_TEXT[args.mode] + (ctx.current ? `\n\nCurrent mode: ${t(ctx.current)}.` : '') + '\n\nThe change is recorded in the Core journal.', ok: 'Switch mode', consent: args.mode === 'advanced' ? 'I understand that Advanced mode runs experimental, reversible actions after my approval.' : null };
    case 'coreRollbackAll':
      return { title: 'Roll back everything', message: 'Undo every policy OpenSweep Core has applied?', detail: `${ctx.count === undefined ? 'All' : ctx.count} active polic${ctx.count === 1 ? 'y' : 'ies'} will be restored to the original values recorded in the journal. Processes that have exited are skipped. Nothing else is changed.`, ok: 'Roll back everything' };
    case 'coreDisablePolicy': {
      const p = ctx.policy || {};
      return { title: 'Disable policy', message: `Roll back this policy now?`, detail: `${t(p.kind || 'Policy')} on ${t((p.target && (p.target.name || p.target)) || 'process')}: restore the original value ${t(p.original)} (currently ${t(p.value)}). Transaction ${t(args.txId)}.`, ok: 'Roll back' };
    }
    case 'coreApprove': {
      const a = ctx.approval || {};
      return { title: 'Approve action', message: `Allow OpenSweep Core to run this action once?`, detail: `${t(a.action || 'Action')}${a.target ? ` on ${t(a.target.name || a.target)}` : ''}.${a.reason ? `\nWhy: ${t(a.reason)}` : ''}${a.risk ? `\nRisk: ${t(a.risk)}.` : ''}\nIt is journaled and can be rolled back. Approval ${t(args.id)}.`, ok: 'Approve' };
    }
    case 'coreExportDiagnostics':
      return { title: 'Export diagnostics', message: 'Write a diagnostics bundle?', detail: 'OpenSweep Core writes one JSON file with its status, recent incidents, events, baselines and journal to its data folder (%ProgramData%\\OpenSweep\\core). It contains process image names but no window titles, command lines or document contents. Nothing is sent anywhere.', ok: 'Export' };
    case 'coreSetEtw': {
      const cur = ctx.etw ? `\n\nNow: kernel tracing is ${ctx.etw.running ? 'running' : 'off'}.` : '';
      return args.enabled
        ? { title: 'Turn kernel tracing on', message: 'Turn kernel tracing on?', detail: `Turn kernel tracing on/off: OpenSweep Core starts its own private Windows kernel trace session (ETW, no driver) to measure which drivers delay the system (DPC/ISR time), disk activity per app and hard page faults per app. Only driver and app names, byte counts and timings are kept — no file paths, contents or keystrokes. It normally costs well under 2 % of one CPU; if it costs more, Core reduces or stops it on its own.${cur}`, ok: 'Turn on' }
        : { title: 'Turn kernel tracing off', message: 'Turn kernel tracing off?', detail: `Turn kernel tracing on/off: OpenSweep Core stops its kernel trace session. The Latency tab and the driver-latency, disk and paging incidents stop updating; all other measurements continue. Nothing else is changed and you can turn it on again at any time.${cur}`, ok: 'Turn off' };
    }
    default:
      throw badArgs('Not an admin method');
  }
}

module.exports = { MODES, RANGES, METRICS, ID_RE, LIMIT_MAX, CORE_METHODS, prepare, describeAdmin, badArgs, MODE_TEXT, validPid: pid };
