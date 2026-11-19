'use strict';
// Unit test for bench/analyze.js statistics on fixed data (any OS). Exits 1 on any failure.
// The data below is synthetic test input, not benchmark results.
const fs = require('fs');
const os = require('os');
const path = require('path');
const a = require('../bench/analyze');

let failed = 0;
const ok = (name, cond, extra = '') => { console.log(`${cond ? '✓' : '✗'} ${name}${extra ? ' — ' + extra : ''}`); if (!cond) failed++; };
const near = (x, y, eps = 1e-9) => Math.abs(x - y) <= eps;
const range = (n, f = (i) => i + 1) => Array.from({ length: n }, (_, i) => f(i));

// ---- quantiles (type 7, same as R's default and numpy.percentile)
ok('median of 1..5 = 3', a.median([5, 1, 4, 2, 3]) === 3);
ok('median of 1..4 = 2.5', a.median([4, 1, 3, 2]) === 2.5);
ok('median of one value', a.median([7]) === 7);
ok('median of empty = NaN', Number.isNaN(a.median([])));
ok('non-numeric values are ignored', a.median([1, 'x', null, 3, NaN, Infinity]) === 2);
ok('p95 of 1..100 = 95.05', near(a.quantile(range(100), 0.95), 95.05));
ok('p99 of 1..100 = 99.01', near(a.quantile(range(100), 0.99), 99.01));
ok('p0 = min, p100 = max', a.quantile([3, 9, 1], 0) === 1 && a.quantile([3, 9, 1], 1) === 9);
ok('p25 of [1,2,3,4] = 1.75', near(a.quantile([1, 2, 3, 4], 0.25), 1.75));
ok('quantile outside 0..1 = NaN', Number.isNaN(a.quantile([1, 2], 1.5)));
ok('mean of 1..4 = 2.5', a.mean([1, 2, 3, 4]) === 2.5);
ok('input array is not modified', (() => { const x = [3, 1, 2]; a.median(x); return x.join() === '3,1,2'; })());

// ---- PRNG
const r1 = a.rng(42), r2 = a.rng(42), r3 = a.rng(43);
const s1 = range(5, () => r1()), s2 = range(5, () => r2()), s3 = range(5, () => r3());
ok('rng is deterministic for a seed', s1.join() === s2.join());
ok('rng differs across seeds', s1.join() !== s3.join());
ok('rng in [0, 1)', range(10000, () => r1()).every(x => x >= 0 && x < 1));

// ---- bootstrap CI
const sym = range(101, (i) => i); // 0..100, median 50
const ci = a.bootstrapCI(sym, { iterations: 2000, seed: 7 });
ok('bootstrap CI contains the sample median', ci[0] <= 50 && ci[1] >= 50, ci.join(' … '));
ok('bootstrap CI is ordered and within the data range', ci[0] <= ci[1] && ci[0] >= 0 && ci[1] <= 100);
ok('bootstrap CI is reproducible with the same seed', a.bootstrapCI(sym, { seed: 7 }).join() === a.bootstrapCI(sym, { seed: 7 }).join());
ok('bootstrap CI of constant data is a point', a.bootstrapCI([4, 4, 4, 4, 4]).join() === '4,4');
ok('bootstrap CI needs at least 2 values', a.bootstrapCI([1]).every(Number.isNaN));
const narrow = a.bootstrapCI(range(1001, (i) => (i % 101)), { seed: 3 }), wide = a.bootstrapCI(range(11, (i) => i * 10), { seed: 3 });
ok('bootstrap CI narrows with more data', narrow[1] - narrow[0] < wide[1] - wide[0], `${(narrow[1] - narrow[0]).toFixed(2)} vs ${(wide[1] - wide[0]).toFixed(2)}`);
const ci99 = a.bootstrapCI(sym, { q: 0.99, seed: 7 });
ok('bootstrap CI of p99 lies in the upper tail', ci99[0] >= 90 && ci99[1] <= 100, ci99.join(' … '));

// ---- difference CI
const base = range(200, (i) => 100 + ((i * 37) % 50)); // deterministic spread 100..149
const same = base.map(x => x), shifted = base.map(x => x + 10);
const d0 = a.bootstrapDiffCI(base, same, { seed: 11 });
ok('diff CI of identical groups contains 0', d0[0] <= 0 && d0[1] >= 0, d0.join(' … '));
const d10 = a.bootstrapDiffCI(base, shifted, { seed: 11 });
ok('diff CI of a +10 shift excludes 0 and contains 10', d10[0] > 0 && d10[0] <= 10 && d10[1] >= 10, d10.join(' … '));
ok('diff CI sign follows the order (b − a)', a.bootstrapDiffCI(shifted, base, { seed: 11 })[1] < 0);

// ---- summarize
const s = a.summarize(range(100), { seed: 1, iterations: 500 });
ok('summarize: n, median, p95, p99, min, max', s.n === 100 && s.median === 50.5 && near(s.p95, 95.05) && near(s.p99, 99.01) && s.min === 1 && s.max === 100);

// ---- result validation + report
const sys = { osCaption: 'Test OS', osBuild: '1', osUbr: 0, cpu: 'Test CPU', logicalProcessors: 4, ramBytes: 8 * 1073741824 };
const mk = (config, samples, extra = {}) => ({ schema: 1, tool: 'opensweep-bench', config, startedAt: '2026-01-01T00:00:00Z', system: sys, workloads: [{ id: 'test.workload', unit: 'ms', runs: 5, samples }], skipped: [], ...extra });
ok('validateResult accepts a well-formed file', a.validateResult(mk('stock', [1, 2])).length === 0);
ok('validateResult rejects a wrong schema / config / missing workloads', a.validateResult({ schema: 2, config: 'turbo' }).length >= 3);
ok('validateResult rejects a workload without samples', a.validateResult(mk('stock', null)).length === 1);

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'osb-'));
const fA = path.join(dir, 'stock.json'), fB = path.join(dir, 'monitor.json'), fC = path.join(dir, 'bad.json');
fs.writeFileSync(fA, '﻿' + JSON.stringify(mk('stock', base))); // run.ps1 writes UTF-8 with BOM
fs.writeFileSync(fB, JSON.stringify(mk('monitor', shifted, { skipped: [{ id: 'disk.diskspd', reason: 'diskspd.exe not found' }], workloads: [{ id: 'test.workload', unit: 'ms', runs: 3, samples: shifted }] })));
fs.writeFileSync(fC, JSON.stringify({ schema: 1, config: 'stock' }));
const res = a.loadResults([fA, fB]);
ok('loadResults reads files (BOM tolerated)', res.length === 2 && res[0].config === 'stock');
let threw = null; try { a.loadResults([fC]); } catch (e) { threw = e.message; }
ok('loadResults rejects an invalid file with its name', threw && threw.includes('bad.json'), threw);
const md = a.report(res, { seed: 1, iterations: 500, now: 0 });
ok('report has a results row per config', /\| test\.workload \| stock \| 5 \| 200 \|/.test(md) && /\| test\.workload \| monitor \| 3 \| 200 \|/.test(md));
ok('report shows Δ vs stock = +10 with a CI marked as excluding 0', /\| 10\.00 \| [\d.]+ … [\d.]+ \* \|/.test(md));
ok('report warns about fewer than 5 runs', /only 3 run\(s\)/.test(md));
ok('report lists skipped workloads with the reason', /disk\.diskspd \(monitor, monitor\.json\): diskspd\.exe not found/.test(md));
ok('report states that FPS and Process Lasso were not measured', /Gaming FPS/.test(md) && /Process Lasso: not compared/.test(md));
const md2 = a.report([res[0], { ...res[1], system: { ...sys, osBuild: '2' } }], { iterations: 200 });
ok('report warns when machines/builds differ', /different machines or Windows builds/.test(md2));
ok('report escapes table pipes', a.report([{ ...res[0], workloads: [{ id: 'a|b', unit: 'ms', samples: [1, 2] }] }], { iterations: 50 }).includes('a\\|b'));
fs.rmSync(dir, { recursive: true, force: true });

console.log(failed ? `\n${failed} check(s) FAILED` : '\nAll bench-analyze checks passed');
process.exit(failed ? 1 : 0);
