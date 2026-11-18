'use strict';
// Reliability monitor: the same data Windows' Reliability Monitor uses
// (Win32_ReliabilityStabilityMetrics + Win32_ReliabilityRecords), classified into
// app failures, Windows failures, other failures, warnings and information.
const { ps } = require('./pscore');

async function reliability({ days = 30 } = {}) {
  const r = await ps(`
try { (Get-Process -Id $PID).PriorityClass = 'BelowNormal' } catch {}
$since = (Get-Date).AddDays(-[int]$a.days)
$wmiSince = [Management.ManagementDateTimeConverter]::ToDmtfDateTime($since)
$idx = @(Get-CimInstance Win32_ReliabilityStabilityMetrics -Filter "TimeGenerated > '$wmiSince'" -ErrorAction SilentlyContinue | ForEach-Object {
  [pscustomobject]@{ t=$_.TimeGenerated.ToString('s'); v=[math]::Round([double]$_.SystemStabilityIndex, 2) } })
$recs = @(Get-CimInstance Win32_ReliabilityRecords -Filter "TimeGenerated > '$wmiSince'" -ErrorAction SilentlyContinue | ForEach-Object {
  $m = [string]$_.Message; if ($m.Length -gt 600) { $m = $m.Substring(0,600) + '...' }
  [pscustomobject]@{ id=[string]$_.RecordNumber; t=$_.TimeGenerated.ToString('s'); src=[string]$_.SourceName; ev=[int]$_.EventIdentifier; product=[string]$_.ProductName; msg=$m } })
Out-Json ([pscustomobject]@{ index=$idx; records=$recs })`, { days }, { timeout: 5 * 60 * 1000 });
  return summarize(r || {}, days);
}

// Turn raw records into the categories Reliability Monitor shows, with plain-English titles.
function classify(rec) {
  const src = rec.src || '', ev = rec.ev, product = rec.product || '';
  const app = product || (rec.msg.match(/Faulting application name: ([^,\s]+)/) || [])[1] || 'An app';
  if (/WER-SystemErrorReporting/i.test(src) || (/BugCheck/i.test(src))) return ['windows', 'Windows stopped working (blue screen)'];
  if (/Kernel-Power/i.test(src) && ev === 41) return ['windows', 'Windows shut down unexpectedly'];
  if (src === 'EventLog' && ev === 6008) return ['windows', 'Windows shut down unexpectedly'];
  if (/Application Hang/i.test(src)) return ['app', `${app} stopped responding`];
  if (/Application Error|Windows Error Reporting|Application Crash/i.test(src)) return ['app', `${app} stopped working`];
  if (/WindowsUpdateClient/i.test(src) && ev === 20) return ['warning', `Update failed: ${product || shortMsg(rec.msg)}`];
  if (/WindowsUpdateClient/i.test(src) && ev === 19) return ['info', `Update installed: ${product || shortMsg(rec.msg)}`];
  if (/MsiInstaller/i.test(src) && ev === 11708) return ['warning', `Install failed: ${product}`];
  if (/MsiInstaller/i.test(src) && (ev === 11707 || ev === 1033)) return ['info', `Installed: ${product}`];
  if (/MsiInstaller/i.test(src) && (ev === 11724 || ev === 1034)) return ['info', `Removed: ${product}`];
  if (/UserPnp|DriverFrameworks/i.test(src)) return ['info', `Driver installed: ${product || shortMsg(rec.msg)}`];
  if (/Install|Update/i.test(src)) return ['info', product || shortMsg(rec.msg)];
  return ['misc', `${product || src}: ${shortMsg(rec.msg)}`];
}
function shortMsg(m) { return String(m || '').split(/\r?\n/)[0].slice(0, 120); }

function summarize(raw, days) {
  const recs = (raw.records || []).map(r => { const [kind, title] = classify(r); return { ...r, kind, title, day: r.t.slice(0, 10) }; })
    .sort((a, b) => (a.t < b.t ? 1 : -1));
  // Stability index: last value of each day.
  const byDay = {};
  for (const i of (raw.index || [])) { const d = i.t.slice(0, 10); if (!byDay[d] || byDay[d].t < i.t) byDay[d] = i; }
  const out = [];
  for (let k = days - 1; k >= 0; k--) {
    const d = new Date(Date.now() - k * 86400e3).toISOString().slice(0, 10);
    const dayRecs = recs.filter(r => r.day === d);
    const count = (kind) => dayRecs.filter(r => r.kind === kind).length;
    out.push({ day: d, index: byDay[d] ? byDay[d].v : null, app: count('app'), windows: count('windows'), misc: count('misc'), warning: count('warning'), info: count('info') });
  }
  // Fill gaps in the index line with the previous value so the chart reads like Windows'.
  let last = null;
  for (const d of out) { if (d.index === null) d.index = last; else last = d.index; }
  const crashers = {};
  for (const r of recs.filter(r => r.kind === 'app')) {
    const name = r.product || r.title.replace(/ stopped (working|responding)$/, '');
    (crashers[name] = crashers[name] || { name, count: 0, last: r.t, hangs: 0 }).count++;
    if (/responding/.test(r.title)) crashers[name].hangs++;
  }
  const current = [...(raw.index || [])].sort((a, b) => (a.t < b.t ? 1 : -1))[0];
  return { days: out, records: recs.slice(0, 500), crashers: Object.values(crashers).sort((a, b) => b.count - a.count).slice(0, 15), current: current ? current.v : null, checkedAt: new Date().toISOString() };
}

module.exports = { reliability, summarize };
