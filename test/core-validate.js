'use strict';
// Unit test for src/core-validate.js (pure; any OS). Prints ✓/✗ per check and exits 1 on any failure.
const v = require('../src/core-validate');

let failed = 0;
const ok = (name, cond, extra = '') => { console.log(`${cond ? '✓' : '✗'} ${name}${extra ? ' — ' + extra : ''}`); if (!cond) failed++; };
const rejects = (name, fn) => { try { const r = fn(); ok(name, false, `accepted: ${JSON.stringify(r)}`); } catch (e) { ok(name, e.code === 'bad_args', `code=${e.code} (${e.message})`); } };
const eq = (a, b) => JSON.stringify(a) === JSON.stringify(b);

// ---- method table matches the contract (docs/core/phases-2-8.md)
const READ = { coreSample: 'sample', coreHistory: 'history', coreBaselines: 'baselines', coreIncidents: 'incidents', coreEvents: 'events', coreRecommendations: 'recommendations', corePolicies: 'policies', coreJournal: 'journal', coreHeal: 'heal' };
const ADMIN = { coreSetMode: 'setMode', coreRollbackAll: 'rollbackAll', coreDisablePolicy: 'disablePolicy', coreApprove: 'approve', coreExportDiagnostics: 'exportDiagnostics' };
ok('method table has exactly the contract methods', eq(Object.keys(v.CORE_METHODS).sort(), [...Object.keys(READ), ...Object.keys(ADMIN)].sort()), Object.keys(v.CORE_METHODS).join(','));
for (const [m, cmd] of Object.entries(READ)) ok(`${m} → ${cmd} (read)`, v.CORE_METHODS[m].cmd === cmd && v.CORE_METHODS[m].cap === 'read');
for (const [m, cmd] of Object.entries(ADMIN)) ok(`${m} → ${cmd} (admin)`, v.CORE_METHODS[m].cmd === cmd && v.CORE_METHODS[m].cap === 'admin');
ok('method table is frozen', Object.isFrozen(v.CORE_METHODS));
ok('no method maps to a Phase 1 / lifecycle / arbitrary command', Object.values(v.CORE_METHODS).every(s => !['hello', 'health', 'metrics', 'reportForeground'].includes(s.cmd)));

// ---- unknown methods / arg containers
rejects('unknown method rejected', () => v.prepare('coreShell', {}));
rejects('prototype key rejected', () => v.prepare('constructor', {}));
rejects('__proto__ rejected', () => v.prepare('__proto__', {}));
rejects('non-string method rejected', () => v.prepare(42, {}));
rejects('args array rejected', () => v.prepare('coreSample', [1]));
rejects('args string rejected', () => v.prepare('coreSample', 'x'));
rejects('args number rejected', () => v.prepare('coreIncidents', 5));

// ---- no-arg commands send no args, even if the renderer sends junk
for (const m of ['coreSample', 'coreBaselines', 'coreRecommendations', 'corePolicies', 'coreHeal', 'coreRollbackAll', 'coreExportDiagnostics']) {
  const p = v.prepare(m, { cmd: 'setMode', mode: 'advanced', __proto__: { x: 1 }, args: { a: 1 } });
  ok(`${m}: extra fields dropped, no args sent`, p.args === undefined && p.cmd === v.CORE_METHODS[m].cmd);
}
ok('null/undefined args accepted for no-arg commands', v.prepare('coreSample', null).args === undefined && v.prepare('coreSample').args === undefined);

// ---- history
for (const r of v.RANGES) ok(`history range ${r} accepted`, eq(v.prepare('coreHistory', { metric: 'cpuPercent', range: r }).args, { metric: 'cpuPercent', range: r }));
for (const m of v.METRICS) ok(`history metric ${m} accepted`, v.prepare('coreHistory', { metric: m, range: '30m' }).args.metric === m);
ok('history drops extra keys', eq(v.prepare('coreHistory', { metric: 'cpuPercent', range: '24h', limit: 5, evil: '../' }).args, { metric: 'cpuPercent', range: '24h' }));
rejects('history: unknown metric', () => v.prepare('coreHistory', { metric: 'top', range: '30m' }));
rejects('history: metric with different case', () => v.prepare('coreHistory', { metric: 'CPUPERCENT', range: '30m' }));
rejects('history: metric as array', () => v.prepare('coreHistory', { metric: ['cpuPercent'], range: '30m' }));
rejects('history: metric __proto__', () => v.prepare('coreHistory', { metric: '__proto__', range: '30m' }));
rejects('history: missing metric', () => v.prepare('coreHistory', { range: '30m' }));
rejects('history: range 1h', () => v.prepare('coreHistory', { metric: 'cpuPercent', range: '1h' }));
rejects('history: range 30M', () => v.prepare('coreHistory', { metric: 'cpuPercent', range: '30M' }));
rejects('history: range as number', () => v.prepare('coreHistory', { metric: 'cpuPercent', range: 30 }));
rejects('history: missing range', () => v.prepare('coreHistory', { metric: 'cpuPercent' }));
ok('every METRICS entry is camelCase', v.METRICS.every(m => /^[a-z][A-Za-z]+$/.test(m)));

// ---- limit
for (const m of ['coreIncidents', 'coreEvents', 'coreJournal']) {
  ok(`${m}: no limit → no args`, v.prepare(m, {}).args === undefined);
  ok(`${m}: limit 1 and 500 accepted`, v.prepare(m, { limit: 1 }).args.limit === 1 && v.prepare(m, { limit: 500 }).args.limit === 500);
  for (const bad of [0, -1, 501, 1.5, '10', NaN, Infinity, true, {}, [], 2 ** 53]) rejects(`${m}: limit ${JSON.stringify(bad) ?? String(bad)} rejected`, () => v.prepare(m, { limit: bad }));
}

// ---- setMode
for (const m of v.MODES) ok(`setMode ${m} accepted`, eq(v.prepare('coreSetMode', { mode: m }).args, { mode: m }));
for (const bad of ['Monitor', 'aggressive', '', ' monitor', 'monitor\u0000', null, 1, ['monitor'], { mode: 'monitor' }]) rejects(`setMode ${JSON.stringify(bad)} rejected`, () => v.prepare('coreSetMode', { mode: bad }));
rejects('setMode without mode rejected', () => v.prepare('coreSetMode', {}));

// ---- ids
for (const good of [1, 1042, 2 ** 53 - 1, 'tx-1042', 'apr-0006', 'a', 'A1_b.c:d-e', 'x'.repeat(64)]) {
  ok(`disablePolicy txId ${JSON.stringify(good).slice(0, 20)} accepted`, v.prepare('coreDisablePolicy', { txId: good }).args.txId === good);
  ok(`approve id ${JSON.stringify(good).slice(0, 20)} accepted`, v.prepare('coreApprove', { id: good }).args.id === good);
}
for (const bad of [0, -5, 1.5, 2 ** 53, NaN, '', ' 1', '1 ', '-x', '.x', 'a/b', 'a\\b', '..', 'a b', 'x'.repeat(65), 'é', 'a\nb', null, true, [], {}, undefined]) {
  rejects(`disablePolicy txId ${JSON.stringify(bad) ?? 'undefined'} rejected`, () => v.prepare('coreDisablePolicy', { txId: bad }));
  rejects(`approve id ${JSON.stringify(bad) ?? 'undefined'} rejected`, () => v.prepare('coreApprove', { id: bad }));
}
ok('approve sends only { id }', eq(v.prepare('coreApprove', { id: 'apr-1', txId: 5, mode: 'advanced' }).args, { id: 'apr-1' }));

// ---- pid (foreground hint, main-process only)
ok('validPid accepts 5 and 0xFFFFFFFF', v.validPid(5) === 5 && v.validPid(0xffffffff) === 0xffffffff);
for (const bad of [0, 4, -1, 0x100000000, 1.2, '123', null]) rejects(`validPid ${JSON.stringify(bad)} rejected`, () => v.validPid(bad));

// ---- confirmation texts
const sm = v.describeAdmin('coreSetMode', { mode: 'advanced' }, { current: 'monitor' });
ok('advanced mode dialog asks for explicit consent', typeof sm.consent === 'string' && sm.consent.length > 20 && /experimental/i.test(sm.detail) && /does not bypass/i.test(sm.detail));
ok('monitor/balanced dialogs need no checkbox', v.describeAdmin('coreSetMode', { mode: 'balanced' }).consent === null && v.describeAdmin('coreSetMode', { mode: 'monitor' }).consent === null);
ok('monitor dialog says nothing is changed and policies roll back', /never changes/i.test(v.describeAdmin('coreSetMode', { mode: 'monitor' }).detail) && /rolled back/i.test(v.describeAdmin('coreSetMode', { mode: 'monitor' }).detail));
const dp = v.describeAdmin('coreDisablePolicy', { txId: 7 }, { policy: { kind: 'ecoqos', target: { name: 'OneDrive.exe' }, value: 1, original: -1 } });
ok('disable-policy dialog names target, original value and transaction', /OneDrive\.exe/.test(dp.detail) && /-1/.test(dp.detail) && /7/.test(dp.detail));
ok('rollback-all dialog states the count', /3 active policies/.test(v.describeAdmin('coreRollbackAll', {}, { count: 3 }).detail) && /1 active policy /.test(v.describeAdmin('coreRollbackAll', {}, { count: 1 }).detail));
ok('approve dialog names the action and target', /restartService on ExampleSyncSvc/.test(v.describeAdmin('coreApprove', { id: 'apr-1' }, { approval: { action: 'restartService', target: { name: 'ExampleSyncSvc' } } }).detail));
ok('export dialog says nothing leaves the machine', /Nothing is sent/.test(v.describeAdmin('coreExportDiagnostics', {}).detail));
ok('dialog text truncates long service-provided strings', v.describeAdmin('coreDisablePolicy', { txId: 1 }, { policy: { kind: 'x'.repeat(5000) } }).detail.length < 1000);
rejects('describeAdmin rejects read methods', () => v.describeAdmin('coreSample', {}));

console.log(failed ? `\n${failed} check(s) FAILED` : '\nAll core-validate checks passed');
process.exit(failed ? 1 : 0);
