'use strict';
// OpenSweep benchmark analysis: reads result files written by bench/run.ps1 and prints a Markdown report
// with run count, median, p95, p99 and a bootstrap 95 % confidence interval for each workload and
// configuration, plus the difference of medians against "stock" with its own bootstrap CI.
// No dependencies. Methodology: bench/README.md. This file never contains or invents numbers.
//
//   node bench/analyze.js results/*.json [--out report.md] [--iterations 2000] [--seed 1]
const fs = require('fs');
const path = require('path');

const SCHEMA = 1;
const CONFIG_ORDER = ['stock', 'monitor', 'active'];
const MIN_RUNS = 5;

// ---------------------------------------------------------------- statistics (pure)
const finite = (a) => (a || []).filter(x => typeof x === 'number' && Number.isFinite(x));
function sorted(values) { return Float64Array.from(finite(values)).sort(); }
// Quantile with linear interpolation between order statistics (Hyndman & Fan type 7, as R/NumPy default).
function quantileSorted(s, q) {
  const n = s.length;
  if (!n || !(q >= 0 && q <= 1)) return NaN;
  if (n === 1) return s[0];
  const h = (n - 1) * q, lo = Math.floor(h), hi = Math.ceil(h);
  return s[lo] + (h - lo) * (s[hi] - s[lo]);
}
const quantile = (values, q) => quantileSorted(sorted(values), q);
const median = (values) => quantile(values, 0.5);
function mean(values) { const a = finite(values); return a.length ? a.reduce((x, y) => x + y, 0) / a.length : NaN; }

// Small seeded PRNG (mulberry32) so a report is reproducible for the same input and seed.
function rng(seed) {
  let t = seed >>> 0;
  return () => { t = (t + 0x6D2B79F5) >>> 0; let r = Math.imul(t ^ (t >>> 15), 1 | t); r = (r + Math.imul(r ^ (r >>> 7), 61 | r)) ^ r; return ((r ^ (r >>> 14)) >>> 0) / 4294967296; };
}
function resampleStat(src, rand, q) {
  const n = src.length, buf = new Float64Array(n);
  for (let i = 0; i < n; i++) buf[i] = src[Math.floor(rand() * n)];
  buf.sort();
  return quantileSorted(buf, q);
}
// Percentile bootstrap CI of a quantile (default: the median).
function bootstrapCI(values, { q = 0.5, iterations = 2000, alpha = 0.05, seed = 1 } = {}) {
  const src = Float64Array.from(finite(values));
  if (src.length < 2) return [NaN, NaN];
  const rand = rng(seed), stats = new Float64Array(iterations);
  for (let i = 0; i < iterations; i++) stats[i] = resampleStat(src, rand, q);
  stats.sort();
  return [quantileSorted(stats, alpha / 2), quantileSorted(stats, 1 - alpha / 2)];
}
// Percentile bootstrap CI of quantile(b) − quantile(a) (independent resampling of both groups).
function bootstrapDiffCI(a, b, { q = 0.5, iterations = 2000, alpha = 0.05, seed = 1 } = {}) {
  const A = Float64Array.from(finite(a)), B = Float64Array.from(finite(b));
  if (A.length < 2 || B.length < 2) return [NaN, NaN];
  const rand = rng(seed), stats = new Float64Array(iterations);
  for (let i = 0; i < iterations; i++) stats[i] = resampleStat(B, rand, q) - resampleStat(A, rand, q);
  stats.sort();
  return [quantileSorted(stats, alpha / 2), quantileSorted(stats, 1 - alpha / 2)];
}
function summarize(values, opts = {}) {
  const s = sorted(values);
  return { n: s.length, median: quantileSorted(s, 0.5), p95: quantileSorted(s, 0.95), p99: quantileSorted(s, 0.99), mean: mean(s), min: s.length ? s[0] : NaN, max: s.length ? s[s.length - 1] : NaN, ci95: bootstrapCI(s, opts) };
}

// ---------------------------------------------------------------- result files
// Validates one result object from run.ps1; returns a list of problems (empty = valid).
function validateResult(r) {
  const errs = [];
  if (!r || typeof r !== 'object') return ['not an object'];
  if (r.schema !== SCHEMA) errs.push(`schema must be ${SCHEMA}`);
  if (!CONFIG_ORDER.includes(r.config)) errs.push(`config must be one of ${CONFIG_ORDER.join(', ')}`);
  if (!r.system || typeof r.system !== 'object') errs.push('system info missing');
  if (!Array.isArray(r.workloads)) errs.push('workloads must be an array');
  else r.workloads.forEach((w, i) => { if (!w || typeof w.id !== 'string' || !Array.isArray(w.samples)) errs.push(`workloads[${i}] needs id and samples[]`); });
  return errs;
}
function loadResults(files) {
  const out = [];
  for (const f of files) {
    let r;
    try { r = JSON.parse(fs.readFileSync(f, 'utf8').replace(/^﻿/, '')); } catch (e) { throw new Error(`${f}: ${e.message}`); }
    const errs = validateResult(r);
    if (errs.length) throw new Error(`${f}: ${errs.join('; ')}`);
    out.push({ file: path.basename(f), ...r });
  }
  return out;
}
// { workloadId: { config: { unit, samples[], runs, files[] } } }
function group(results) {
  const g = {};
  for (const r of results) for (const w of r.workloads) {
    const c = ((g[w.id] = g[w.id] || {})[r.config] = g[w.id][r.config] || { unit: w.unit || '', samples: [], runs: 0, files: [], better: w.better || 'lower' });
    c.samples.push(...finite(w.samples)); c.runs += Number.isInteger(w.runs) ? w.runs : 1; c.files.push(r.file);
  }
  return g;
}

// ---------------------------------------------------------------- report
const fmtN = (x) => { if (!Number.isFinite(x)) return '—'; const a = Math.abs(x); return a >= 1000 ? x.toFixed(0) : a >= 100 ? x.toFixed(1) : a >= 1 ? x.toFixed(2) : x.toFixed(3); };
const esc = (s) => String(s === undefined || s === null ? '' : s).replace(/\|/g, '\\|').replace(/\r?\n/g, ' ');
function systemKey(sys) { return [sys.osCaption, sys.osBuild, sys.cpu, sys.logicalProcessors, sys.ramBytes].join(' / '); }

function report(results, opts = {}) {
  const g = group(results);
  const lines = [];
  lines.push('# OpenSweep benchmark report', '');
  lines.push(`Generated ${new Date(opts.now || Date.now()).toISOString()} by bench/analyze.js from ${results.length} result file(s). Bootstrap: ${opts.iterations || 2000} resamples, seed ${opts.seed || 1}, percentile 95 % CI.`, '');
  lines.push('## Machines and configurations', '');
  lines.push('| File | Config | Windows | Build | CPU | Logical CPUs | RAM | Core version | Core mode | Started |', '|---|---|---|---|---|---|---|---|---|---|');
  for (const r of results) {
    const s = r.system || {};
    lines.push(`| ${esc(r.file)} | ${esc(r.config)} | ${esc(s.osCaption)} | ${esc(s.osBuild)}${s.osUbr !== undefined ? '.' + esc(s.osUbr) : ''} | ${esc(s.cpu)} | ${esc(s.logicalProcessors)} | ${s.ramBytes ? (s.ramBytes / 1073741824).toFixed(1) + ' GB' : '—'} | ${esc(s.coreVersion || '—')} | ${esc(s.coreMode || '—')} | ${esc(r.startedAt)} |`);
  }
  const keys = new Set(results.map(r => systemKey(r.system || {})));
  lines.push('');
  if (keys.size > 1) lines.push('> **Warning:** the result files come from different machines or Windows builds. Differences between configurations are not comparable.', '');
  const skipped = results.flatMap(r => (r.skipped || []).map(x => ({ ...x, config: r.config, file: r.file })));

  lines.push('## Results', '');
  lines.push('Lower is better unless marked "higher is better". Δ = median(config) − median(stock); a CI that includes 0 means no measurable difference.', '');
  lines.push('| Workload | Config | Runs | n | Median | p95 | p99 | 95 % CI (median) | Δ vs stock | 95 % CI (Δ) |', '|---|---|---|---|---|---|---|---|---|---|');
  const warnings = [];
  for (const id of Object.keys(g).sort()) {
    const byCfg = g[id], stock = byCfg.stock;
    for (const cfg of CONFIG_ORDER) {
      const c = byCfg[cfg]; if (!c) continue;
      const s = summarize(c.samples, opts);
      let d = '—', dci = '—';
      if (stock && cfg !== 'stock' && stock.samples.length && c.samples.length) {
        d = fmtN(s.median - median(stock.samples));
        const ci = bootstrapDiffCI(stock.samples, c.samples, opts);
        dci = Number.isFinite(ci[0]) ? `${fmtN(ci[0])} … ${fmtN(ci[1])}${ci[0] > 0 || ci[1] < 0 ? ' *' : ''}` : '—';
      }
      if (c.runs < MIN_RUNS) warnings.push(`${id} / ${cfg}: only ${c.runs} run(s) (methodology asks for ≥ ${MIN_RUNS}).`);
      lines.push(`| ${esc(id)}${c.better === 'higher' ? ' (higher is better)' : ''} | ${cfg} | ${c.runs} | ${s.n} | ${fmtN(s.median)} ${esc(c.unit)} | ${fmtN(s.p95)} | ${fmtN(s.p99)} | ${Number.isFinite(s.ci95[0]) ? `${fmtN(s.ci95[0])} … ${fmtN(s.ci95[1])}` : '—'} | ${d} | ${dci} |`);
    }
  }
  lines.push('', '`*` = the 95 % CI of the difference excludes 0.', '');
  if (warnings.length) lines.push('## Warnings', '', ...warnings.map(w => `- ${w}`), '');
  lines.push('## Not measured / skipped', '');
  if (skipped.length) lines.push(...skipped.map(x => `- ${esc(x.id)} (${x.config}, ${esc(x.file)}): ${esc(x.reason)}`));
  else lines.push('- Nothing was skipped in these result files.');
  lines.push('- Gaming FPS, 1 % lows and frame-time variance: not captured by run.ps1 (see bench/README.md, PresentMon).');
  lines.push('- Process Lasso: not compared unless a result file with that configuration exists (none is produced by run.ps1).', '');
  return lines.join('\n');
}

module.exports = { quantile, quantileSorted, median, mean, rng, bootstrapCI, bootstrapDiffCI, summarize, validateResult, loadResults, group, report, SCHEMA, CONFIG_ORDER, MIN_RUNS };

if (require.main === module) {
  const argv = process.argv.slice(2);
  const opt = (name, def) => { const i = argv.indexOf(name); if (i < 0) return def; const v = argv[i + 1]; argv.splice(i, 2); return v; };
  const out = opt('--out', null), iterations = Number(opt('--iterations', 2000)), seed = Number(opt('--seed', 1));
  if (!argv.length) { console.error('usage: node bench/analyze.js <result.json>... [--out report.md] [--iterations 2000] [--seed 1]'); process.exit(2); }
  try {
    const md = report(loadResults(argv), { iterations, seed });
    if (out) { fs.writeFileSync(out, md); console.log(`Report written to ${out}`); } else console.log(md);
  } catch (e) { console.error(e.message); process.exit(1); }
}
