'use strict';
// Windows integration test for OpenSweepCore.exe: named-pipe protocol v1 (docs/core/ipc-protocol.md)
// and the Phase 2–8 commands (docs/core/phases-2-8.md) in --console mode, then the Windows service
// lifecycle with a 60 s engine soak and the admin commands. Needs Windows and an elevated shell
// (GitHub's windows-latest runners are). Prints ✓/✗ per step, exits 1 on any failure.
// Nothing is skipped silently: anything not tested is printed as "not tested: …".
//
//   node test/core-ipc.js [path\to\OpenSweepCore.exe]
const { spawn, spawnSync } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');
const net = require('net');
const cc = require('../src/core-client');

const PIPE_NAME = 'OpenSweepCore.v1';
const PIPE = cc.PIPE_PATH;
const SERVICE = 'OpenSweepCore';
const exe = path.resolve(process.argv[2] || path.join(__dirname, '..', 'core', 'build', 'Release', 'OpenSweepCore.exe'));

let failed = 0;
const ok = (name, cond, extra = '') => { console.log(`${cond ? '✓' : '✗'} ${name}${extra !== '' ? ' — ' + extra : ''}`); if (!cond) failed++; return !!cond; };
const note = (msg) => console.log(`  · ${msg}`);
const section = (t) => console.log(`\n== ${t}`);
const sleep = (ms) => new Promise(r => setTimeout(r, ms));
const num = (x) => typeof x === 'number' && Number.isFinite(x);
async function until(fn, ms, every = 200) { const end = Date.now() + ms; for (;;) { const v = await fn(); if (v) return v; if (Date.now() > end) return v; await sleep(every); } }

const pipeExists = () => { try { return fs.readdirSync('\\\\.\\pipe\\').includes(PIPE_NAME); } catch { return false; } };
const run = (file, args, timeout = 30000) => { const r = spawnSync(file, args, { encoding: 'utf8', timeout, windowsHide: true }); return { code: r.status, out: `${r.stdout || ''}${r.stderr || ''}`.trim(), error: r.error }; };
function serviceState() {
  const r = run('sc.exe', ['queryex', SERVICE]);
  if (/1060/.test(r.out)) return { state: 'NOT_INSTALLED' };
  const m = /STATE\s*:\s*\d+\s+(\w+)/.exec(r.out), p = /PID\s*:\s*(\d+)/.exec(r.out);
  return { state: m ? m[1] : 'UNKNOWN', pid: p ? Number(p[1]) : 0, raw: r.out };
}

// Raw protocol connection: send arbitrary bytes, read frames, see when the server closes.
function rawConnect(timeout = 5000) {
  return new Promise((resolve, reject) => {
    const s = net.connect(PIPE);
    const t = setTimeout(() => { s.destroy(); reject(Object.assign(new Error('connect timeout'), { code: 'timeout' })); }, timeout);
    s.once('error', (e) => { clearTimeout(t); reject(e); });
    s.once('connect', () => { clearTimeout(t); resolve(wrapRaw(s)); });
  });
}
function wrapRaw(s) {
  const dec = new cc.FrameDecoder(cc.MAX_RESPONSE);
  const frames = [], waiters = [];
  const r = { sock: s, closed: false, closedAt: 0, decodeError: null };
  const wake = () => { while (waiters.length) waiters.shift()(); };
  s.on('data', (c) => { try { frames.push(...dec.push(c)); } catch (e) { r.decodeError = e; } wake(); });
  s.on('error', () => {});
  s.on('close', () => { r.closed = true; r.closedAt = Date.now(); wake(); });
  r.send = (buf) => new Promise((res) => { if (r.closed) return res(false); s.write(buf, (e) => res(!e)); });
  r.sendJson = (o) => r.send(cc.encodeFrame(o, 1 << 20));
  r.next = async (ms = 3000) => {
    const end = Date.now() + ms;
    while (!frames.length) {
      if (r.closed) return null;
      const left = end - Date.now(); if (left <= 0) return undefined;
      await new Promise(res => { const t = setTimeout(res, left); waiters.push(() => { clearTimeout(t); res(); }); });
    }
    return frames.shift();
  };
  r.waitClosed = async (ms) => { await until(() => r.closed, ms, 50); return r.closed; };
  r.close = () => s.destroy();
  return r;
}
// One-shot helpers that never leave a connection open (open connections count against the 8-client limit).
async function probeOnce() { const x = cc.createCoreClient({ timeout: 3000 }); try { return await x.probe(); } finally { x.close(); } }
async function callOnce(cmd) { const x = cc.createCoreClient({ timeout: 3000 }); try { return await x.call(cmd); } finally { x.close(); } }
const frameOf = (len, body = Buffer.alloc(0)) => { const h = Buffer.alloc(4); h.writeUInt32LE(len >>> 0); return Buffer.concat([h, body]); };

// ---------------------------------------------------------------- Phases 2–8 (docs/core/phases-2-8.md)
const MODES = ['monitor', 'balanced', 'advanced'];
const RANGES = ['30m', '24h', '14d'];
const DASH_METRICS = ['cpuPercent', 'memLoadPercent', 'commitTotal', 'diskReadLatencyMs', 'hardFaultsPerSec'];
const isInt = (x) => Number.isInteger(x);
const isStr = (x) => typeof x === 'string';
// Game boost + scorecard (docs/core/alpha4.md). Timestamps may be epoch ms or ISO strings.
const isTs = (x) => num(x) || (isStr(x) && !Number.isNaN(Date.parse(x)));
const SCORE_METRICS = ['bootMs', 'idleCpuPercent', 'idleRamUsedBytes', 'diskLatencyMs', 'hardFaultsPerSec', 'stutterPerHour', 'crashesPerWeek'];
const boostShape = (b) => !!b && typeof b.enabled === 'boolean' && typeof b.active === 'boolean' && (b.since === null || b.since === undefined || isTs(b.since))
  && (b.foregroundPid === null || b.foregroundPid === undefined || isInt(b.foregroundPid)) && (b.foregroundName === null || b.foregroundName === undefined || isStr(b.foregroundName))
  && Array.isArray(b.throttled) && b.throttled.length <= 16 && b.throttled.every(t => isInt(t.pid) && isStr(t.name) && !/[\\/]/.test(t.name) && isStr(t.kind))
  && (b.lastSession === null || b.lastSession === undefined || (isTs(b.lastSession.start) && isTs(b.lastSession.end) && isInt(b.lastSession.throttledCount)));
const metricsShape = (m) => !!m && typeof m === 'object' && SCORE_METRICS.every(k => m[k] === null || m[k] === undefined || num(m[k]));
const scorecardShape = (s) => !!s && (s.baseline === null || (isTs(s.baseline.capturedAt) && metricsShape(s.baseline.metrics))) && s.current && metricsShape(s.current.metrics) && num(s.hoursMeasured) && s.hoursMeasured >= 0;
const tsOf = (x) => Number(x && (x.tsMs ?? x.ts ?? x.createdMs ?? x.firstSeen ?? x.appliedAt)) || 0;
// "newest first": true if the list is descending by any of the given timestamp fields.
const newestFirst = (list, keys) => keys.some(k => list.every((x, i) => i === 0 || Number(x[k]) <= Number(list[i - 1][k])));
const MB = 1048576;
async function expectErr(name, p, code) {
  try { const r = await p; return ok(name, false, `accepted: ${JSON.stringify(r).slice(0, 160)}`); }
  catch (e) { return ok(name, Array.isArray(code) ? code.includes(e.code) : e.code === code, `code=${e.code} (${e.message})`); }
}
// Privacy (phases-2-8 safety rules): no window titles, command lines or document contents in results.
function privacyKeys(obj) {
  const bad = [];
  const walk = (o, p) => { if (!o || typeof o !== 'object') return; for (const [k, v] of Object.entries(o)) { if (/^(commandline|cmdline|windowtitle|keystrokes?|documentpath)$/i.test(k) || (/^title$/i.test(k) && !/incident/i.test(p))) bad.push(p + k); walk(v, `${p}${k}.`); } };
  walk(obj, '');
  return bad;
}
// ---------------------------------------------------------------- kernel tracing (docs/core/etw.md)
const ETW_SESSION = 'OpenSweepCoreKernel';
const etwSessionExists = () => run('logman.exe', ['query', ETW_SESSION, '-ets']).code === 0;
const plainName = (x) => isStr(x) && x.length > 0 && !/[\\/]/.test(x);
const etwStatusShape = (e) => !!e && typeof e.running === 'boolean' && Array.isArray(e.flags) && e.flags.every(isStr) && num(e.eventsLost) && e.eventsLost >= 0 && num(e.costPercent) && e.costPercent >= 0;
const driverShape = (d) => plainName(d.name) && isInt(d.dpcCount) && d.dpcCount >= 0 && isInt(d.isrCount) && d.isrCount >= 0 && num(d.totalUs) && d.totalUs >= 0 && num(d.maxUs) && d.maxUs >= 0
  && Array.isArray(d.histogram) && d.histogram.length === 8 && d.histogram.every(n => isInt(n) && n >= 0);
const diskShape = (p) => isInt(p.pid) && p.pid >= 0 && plainName(p.name) && ['readBps', 'writeBps', 'ios', 'meanLatencyMs', 'maxLatencyMs'].every(k => num(p[k]) && p[k] >= 0);
const faultShape = (p) => isInt(p.pid) && p.pid >= 0 && plainName(p.name) && num(p.faultsPerSec) && p.faultsPerSec >= 0;
// Disk workload from this process: write (with fsync, so the writes are issued now, not later by the
// lazy writer) and read back a 32 MB temp file, `rounds` times. Returns bytes moved.
function diskWorkload(rounds = 3) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'osc-disk-')), chunk = Buffer.alloc(MB, 0x5a), buf = Buffer.alloc(MB);
  let bytes = 0;
  try {
    for (let r = 0; r < rounds; r++) {
      const f = path.join(dir, `w${r}.bin`);
      let fd = fs.openSync(f, 'w');
      for (let i = 0; i < 32; i++) { chunk[0] = (i + r) & 0xff; fs.writeSync(fd, chunk); bytes += MB; }
      fs.fsyncSync(fd); fs.closeSync(fd);
      fd = fs.openSync(f, 'r');
      while (fs.readSync(fd, buf, 0, MB, null) > 0) bytes += MB;
      fs.closeSync(fd);
      fs.unlinkSync(f);
    }
  } finally { try { fs.rmSync(dir, { recursive: true, force: true }); } catch { /* ignore */ } }
  return bytes;
}
// latency / diskActivity / hardFaults: shapes, ETW running soon after start, a disk workload from
// this process (attribution reported as a note: it depends on caching), top drivers printed.
async function latencyChecks(c, label, { workload = false } = {}) {
  section(`kernel tracing (${label})`);
  let lat;
  try { lat = await c.call('latency'); } catch (e) { ok('latency answers', false, `${e.code} ${e.message}`); return; }
  // ETW must be running within ~10 s of the service start (uptime from hello).
  const up = Number((await c.call('hello')).uptimeSec) || 0;
  const wait = Math.max(2000, (12 - up) * 1000);
  if (!(lat.status && lat.status.running)) lat = await until(async () => { const l = await c.call('latency'); return l.status && l.status.running ? l : null; }, wait, 500) || await c.call('latency');
  const st = lat.status || {}, sys = lat.system || {};
  ok('latency.status shape', etwStatusShape(st), JSON.stringify(st));
  ok('latency.status.running within ~10 s of start (elevated)', st.running === true, `running=${st.running} at uptime ≈ ${(Number((await c.call('hello')).uptimeSec) || 0).toFixed(0)} s${st.reason ? ` reason: ${st.reason}` : ''}`);
  note(`ETW flags [${(st.flags || []).join(', ')}] · cost ${st.costPercent} % · ${st.eventsLost} events lost`);
  ok('latency.status.costPercent plausible (0–100)', num(st.costPercent) && st.costPercent <= 100, String(st.costPercent));
  ok('latency.system shape', ['dpcPercent', 'isrPercent', 'worstDpcUs', 'worstIsrUs'].every(k => num(sys[k]) && sys[k] >= 0) && sys.dpcPercent <= 100 && sys.isrPercent <= 100, JSON.stringify(sys));
  ok('latency.drivers shape (≤ 20, histogram of 8, plain driver names)', Array.isArray(lat.drivers) && lat.drivers.length <= 20 && lat.drivers.every(driverShape), `${(lat.drivers || []).length} driver(s) ${JSON.stringify((lat.drivers || []).find(d => !driverShape(d)) || '').slice(0, 200)}`);
  const st2 = await c.call('status');
  ok('status.etw present with the latency.status shape', etwStatusShape(st2.etw), JSON.stringify(st2.etw));

  let bytes = 0;
  const pid = process.pid;
  let disk = await c.call('diskActivity'), mine = null;
  if (workload) {
    const t0 = Date.now();
    bytes += diskWorkload(3);
    note(`disk workload: ${(bytes / MB).toFixed(0)} MB written (fsync) and read by pid ${pid} in ${Date.now() - t0} ms`);
    // Aggregation uses 1-minute windows: poll for up to ~70 s, repeating the workload every ~15 s.
    let lastLoad = Date.now();
    const end = Date.now() + 70000;
    for (;;) {
      disk = await c.call('diskActivity');
      mine = (disk.processes || []).find(p => p.pid === pid);
      lat = await c.call('latency');
      if ((mine && (lat.drivers || []).length) || Date.now() > end) break;
      if (Date.now() - lastLoad > 15000) { bytes += diskWorkload(2); lastLoad = Date.now(); }
      await sleep(2000);
    }
  }
  ok('diskActivity shape (≤ 20 processes, image names only)', Array.isArray(disk.processes) && disk.processes.length <= 20 && disk.processes.every(diskShape), `${(disk.processes || []).length} process(es) ${JSON.stringify((disk.processes || []).find(p => !diskShape(p)) || '').slice(0, 200)}`);
  ok('diskActivity: no file paths / titles / command lines', privacyKeys(disk).length === 0 && !/[a-z]:\\/i.test(JSON.stringify(disk)), privacyKeys(disk).join(', '));
  if (workload) {
    if (mine) console.log(`  note: attribution OK: diskActivity has this test's pid ${pid} (${mine.name}): read ${(mine.readBps / MB).toFixed(1)} MB/s, write ${(mine.writeBps / MB).toFixed(1)} MB/s, ${mine.ios} I/Os, mean ${mine.meanLatencyMs} ms, max ${mine.maxLatencyMs} ms`);
    else console.log(`  note: attribution NOT observed: no diskActivity entry for this test's pid ${pid} within ~70 s after ${(bytes / MB).toFixed(0)} MB of I/O (reads may be served from the file cache and writes flushed by the System process). Top entries: ${(disk.processes || []).slice(0, 5).map(p => `${p.name}(${p.pid}) r=${(p.readBps / MB).toFixed(1)} w=${(p.writeBps / MB).toFixed(1)} MB/s`).join(', ') || 'none'}`);
    ok('latency: DPC/ISR seen for the workload (drivers non-empty)', (lat.drivers || []).length > 0, `${(lat.drivers || []).length} driver(s)`);
  }
  const hf = await c.call('hardFaults');
  ok('hardFaults shape (≤ 20 processes, image names only)', Array.isArray(hf.processes) && hf.processes.length <= 20 && hf.processes.every(faultShape), `${(hf.processes || []).length} process(es) ${JSON.stringify((hf.processes || []).find(p => !faultShape(p)) || '').slice(0, 200)}`);
  const top = [...(lat.drivers || [])].sort((a, b) => b.maxUs - a.maxUs).slice(0, 5);
  console.log(`  top drivers by max µs (${label}): ${top.length ? '' : 'none'}`);
  for (const d of top) console.log(`    ${d.name.padEnd(20)} max ${String(Math.round(d.maxUs)).padStart(6)} µs · DPC ${d.dpcCount} · ISR ${d.isrCount} · total ${(d.totalUs / 1000).toFixed(1)} ms · histogram [${d.histogram.join(', ')}]`);
  note(`system: DPC ${sys.dpcPercent} % · ISR ${sys.isrPercent} % · worst DPC ${sys.worstDpcUs} µs · worst ISR ${sys.worstIsrUs} µs · top hard faults: ${(hf.processes || []).slice(0, 3).map(p => `${p.name} ${p.faultsPerSec}/s`).join(', ') || 'none'}`);
}

async function callTimed(c, cmd, args) { const t = Date.now(); const r = await c.call(cmd, args); return { r, ms: Date.now() - t }; }

// Read commands: result shapes per the contract. `label` = 'console' | 'service'.
async function engineReadChecks(c, label, serverPid) {
  section(`engine read commands (${label})`);
  let st;
  try { st = await c.call('status'); } catch (e) { ok('status answers', false, `${e.code} ${e.message}`); return false; }
  const eng = st.engine || {}, counts = st.counts || {};
  ok('status shape', MODES.includes(st.mode) && typeof st.simulation === 'boolean' && typeof eng.running === 'boolean' && isInt(eng.ticks) && num(eng.lastTickMs) && num(eng.tickCostUs)
    && num(st.stabilityScore) && Array.isArray(st.scoreReasons) && st.scoreReasons.every(isStr) && ['incidentsOpen', 'policiesActive', 'recommendations'].every(k => isInt(counts[k]))
    && st.heal && typeof st.heal.breakerOpen === 'boolean', JSON.stringify(st).slice(0, 400));
  ok('status: default mode is monitor and decisions are simulated', st.mode === 'monitor' && st.simulation === true, `mode=${st.mode} simulation=${st.simulation}`);
  ok('status: stabilityScore within 0–100', st.stabilityScore >= 0 && st.stabilityScore <= 100, String(st.stabilityScore));
  ok('status: engine running', eng.running === true);
  const t0 = eng.ticks;
  await sleep(4500);
  const st2 = await c.call('status');
  ok('status: engine ticks advance (2 s interval)', st2.engine.ticks >= t0 + 1, `${t0} → ${st2.engine.ticks} in 4.5 s`);
  ok('status: lastTickMs is recent', Math.abs(Date.now() - st2.engine.lastTickMs) < 10000, `${Date.now() - st2.engine.lastTickMs} ms ago`);
  note(`engine tick cost ${(st2.engine.tickCostUs / 1000).toFixed(2)} ms, stability score ${st2.stabilityScore}, reasons: ${JSON.stringify(st2.scoreReasons).slice(0, 200)}`);

  const { r: smp, ms: smpMs } = await callTimed(c, 'sample');
  ok('sample shape', num(smp.tsMs) && num(smp.cpuPercent) && num(smp.memTotal) && num(smp.memAvail) && num(smp.memLoadPercent) && num(smp.commitTotal) && num(smp.commitLimit)
    && num(smp.hardFaultsPerSec) && num(smp.diskReadLatencyMs) && num(smp.diskWriteLatencyMs) && isInt(smp.processes) && Array.isArray(smp.top), JSON.stringify({ ...smp, top: `[${(smp.top || []).length}]` }).slice(0, 400));
  ok('sample: recent, cpu 0–100, memTotal ≈ os.totalmem()', Math.abs(Date.now() - smp.tsMs) < 10000 && smp.cpuPercent >= 0 && smp.cpuPercent <= 100 && Math.abs(smp.memTotal - os.totalmem()) <= os.totalmem() * 0.01, `age ${Date.now() - smp.tsMs} ms, ${smpMs} ms to answer`);
  ok('sample: commitLimit >= commitTotal > 0', smp.commitTotal > 0 && smp.commitLimit >= smp.commitTotal);
  ok('sample: top bounded (<= 40) with image names only', smp.top.length <= 40 && smp.top.every(p => isInt(p.pid) && isStr(p.name) && !/[\\/]/.test(p.name) && num(p.cpuPercent)), `${smp.top.length} processes`);
  const pk = privacyKeys(smp);
  ok('sample: no window titles / command lines', pk.length === 0, pk.join(', '));
  if (serverPid) ok('sample: the service itself appears with a plausible name or not at all', smp.top.every(p => p.pid !== serverPid || /opensweepcore/i.test(p.name)));

  for (const range of RANGES) {
    const res = await Promise.allSettled(DASH_METRICS.map(metric => c.call('history', { metric, range })));
    const bad = [];
    res.forEach((x, i) => {
      if (x.status === 'rejected') { bad.push(`${DASH_METRICS[i]}: ${x.reason.code}`); return; }
      const h = x.value, pts = h.points;
      const span = { '30m': 30 * 60e3, '24h': 24 * 3600e3, '14d': 14 * 86400e3 }[range];
      const shape = h.metric === DASH_METRICS[i] && isStr(h.unit) && Array.isArray(pts) && pts.length <= 900 && pts.every(p => Array.isArray(p) && p.length === 2 && num(p[0]) && num(p[1]));
      const sorted = pts.every((p, j) => j === 0 || p[0] >= pts[j - 1][0]);
      const inWindow = pts.every(p => p[0] >= Date.now() - span - 30 * 60e3 && p[0] <= Date.now() + 10000);
      if (!shape || !sorted || !inWindow) bad.push(`${DASH_METRICS[i]}: shape=${shape} sorted=${sorted} inWindow=${inWindow}`);
    });
    const n30 = res[0].status === 'fulfilled' ? res[0].value.points.length : 0;
    ok(`history ${range}: shape for ${DASH_METRICS.length} metrics (≤ 900 points, ascending, in range)`, bad.length === 0, bad.join('; ') || `cpuPercent: ${n30} points`);
    if (range === '30m') ok('history 30m has points after a few seconds', n30 >= 1, `${n30} points`);
  }
  await expectErr('history: unknown metric -> bad_args', c.call('history', { metric: 'not_a_metric', range: '30m' }), 'bad_args');
  await expectErr('history: bad range -> bad_args', c.call('history', { metric: 'cpuPercent', range: '7d' }), 'bad_args');
  await expectErr('history: missing args -> bad_args', c.call('history'), 'bad_args');

  const bl = await c.call('baselines');
  const blv = Object.entries(bl.metrics || {});
  ok('baselines shape', bl.metrics && typeof bl.metrics === 'object' && blv.every(([, b]) => num(b.median) && num(b.mad) && b.mad >= 0 && num(b.ewma) && isInt(b.samples) && b.samples >= 0 && typeof b.ready === 'boolean'), `${blv.length} metrics: ${blv.slice(0, 3).map(([k, b]) => `${k} n=${b.samples} ready=${b.ready}`).join(', ')}`);

  const inc = await c.call('incidents', { limit: 50 });
  ok('incidents shape', Array.isArray(inc.incidents) && inc.incidents.length <= 50 && inc.incidents.every(x => x.id !== undefined && Array.isArray(x.symptoms) && Array.isArray(x.evidence) && num(x.confidence) && x.confidence >= 0 && x.confidence <= 1 && isStr(x.risk) && typeof x.autoRecoveryAllowed === 'boolean'), `${inc.incidents && inc.incidents.length} incident(s)`);
  ok('incidents newest first (by firstSeen or lastSeen)', newestFirst(inc.incidents, ['firstSeen', 'lastSeen']));
  if (inc.incidents.length) ok('incident fields used by the dashboard', inc.incidents.every(x => isStr(x.title) && ['open', 'resolved'].includes(x.state) && isStr(x.confidenceLevel) && Array.isArray(x.contributors) && Array.isArray(x.recommendedActions)), JSON.stringify(inc.incidents[0]).slice(0, 300));
  else note('no incidents yet: incident field names not checked');
  ok('incidents limit 1 respected', (await c.call('incidents', { limit: 1 })).incidents.length <= 1);
  await expectErr('incidents: limit as string -> bad_args', c.call('incidents', { limit: 'x' }), 'bad_args');

  const ev = await c.call('events', { limit: 100 });
  ok('events shape', Array.isArray(ev.events) && ev.events.length <= 100 && ev.events.every(e => num(e.tsMs) && isStr(e.kind) && isStr(e.provider) && isInt(e.eventId) && isInt(e.level) && e.level >= 1 && e.level <= 4 && isStr(e.message) && e.message.length <= 200), `${ev.events && ev.events.length} event(s)`);
  ok('events newest first', ev.events.every((x, i) => i === 0 || x.tsMs <= ev.events[i - 1].tsMs));
  ok('events: no window titles / command lines', privacyKeys(ev).length === 0, privacyKeys(ev).join(', '));

  const rec = await c.call('recommendations');
  ok('recommendations shape', Array.isArray(rec.items) && rec.items.every(x => x.id !== undefined && ['memory', 'cpu'].includes(x.area) && x.target !== undefined && isStr(x.action) && isStr(x.reason) && (isStr(x.risk) || num(x.risk)) && typeof x.wouldApply === 'boolean'), `${rec.items && rec.items.length} item(s)`);
  note(`recommendations: ${(rec.items || []).map(x => `${x.action}→${(x.target && x.target.name) || x.target} wouldApply=${x.wouldApply}${x.blockedBy ? ` (${x.blockedBy})` : ''}`).join('; ') || 'none'}`);

  const pol = await c.call('policies');
  ok('policies shape', Array.isArray(pol.active) && pol.active.every(p => p.txId !== undefined && isStr(p.kind) && p.target !== undefined && isStr(p.state)), `${pol.active && pol.active.length} active`);
  ok('policies: none active in monitor mode', st2.mode !== 'monitor' || pol.active.length === 0, JSON.stringify(pol.active).slice(0, 200));

  const jr = await c.call('journal', { limit: 100 });
  ok('journal shape', Array.isArray(jr.entries) && jr.entries.length <= 100 && jr.entries.every(t => t.txId !== undefined && isStr(t.state)), `${jr.entries && jr.entries.length} entr(ies)`);
  ok('journal newest first (by createdMs or updatedMs)', newestFirst(jr.entries, ['createdMs', 'updatedMs']));

  const heal = await c.call('heal');
  ok('heal shape', heal.breaker && typeof heal.breaker.open === 'boolean' && isInt(heal.breaker.failures) && Array.isArray(heal.pending) && Array.isArray(heal.history), JSON.stringify(heal.breaker));

  const topo = await c.call('topology');
  const sets = topo.cpuSets;
  ok('topology.cpuSets shape', Array.isArray(sets) && sets.length > 0 && sets.every(x => ['id', 'group', 'core', 'efficiencyClass', 'numa'].every(k => isInt(x[k]))), `${sets && sets.length} CPU sets`);
  ok('topology.cpuSets: one per logical processor, unique ids', Array.isArray(sets) && sets.length === topo.logicalProcessors && new Set(sets.map(x => x.id)).size === sets.length, `${sets && sets.length} vs ${topo.logicalProcessors}`);
  ok('topology.cpuSets: efficiency classes consistent with hybrid flag', Array.isArray(sets) && (topo.hybrid ? new Set(sets.map(x => x.efficiencyClass)).size > 1 : new Set(sets.map(x => x.efficiencyClass)).size === 1));

  await latencyChecks(c, label, { workload: label === 'console' });

  // Game boost + scorecard reads (docs/core/alpha4.md).
  try { const b = await c.call('boost'); ok(`boost shape (${label})`, boostShape(b), JSON.stringify(b).slice(0, 300)); ok(`boost: never active while disabled (${label})`, b.active === false || b.enabled === true, JSON.stringify({ enabled: b.enabled, active: b.active })); }
  catch (e) { ok(`boost answers (${label})`, false, `${e.code} ${e.message}`); }
  try { const sc = await c.call('scorecard'); ok(`scorecard shape (${label})`, scorecardShape(sc), JSON.stringify(sc).slice(0, 400)); }
  catch (e) { ok(`scorecard answers (${label})`, false, `${e.code} ${e.message}`); }

  // Foreground hint: own pid accepted, bogus pids rejected.
  try { const r = await c.call('reportForeground', { pid: process.pid }); ok(`reportForeground with own pid accepted (${label})`, r && typeof r === 'object', JSON.stringify(r)); }
  catch (e) { ok(`reportForeground with own pid accepted (${label})`, false, `${e.code} ${e.message}`); }
  await expectErr('reportForeground with a pid that does not exist -> bad_args', c.call('reportForeground', { pid: 0x7ffffff0 }), 'bad_args');
  await expectErr('reportForeground with pid 0 -> bad_args', c.call('reportForeground', { pid: 0 }), 'bad_args');
  await expectErr('reportForeground with pid as string -> bad_args', c.call('reportForeground', { pid: String(process.pid) }), 'bad_args');
  await expectErr('reportForeground without pid -> bad_args', c.call('reportForeground', {}), 'bad_args');
  for (const fs of [true, false]) {
    try { const r = await c.call('reportForeground', { pid: process.pid, fullscreen: fs }); ok(`reportForeground with fullscreen ${fs} accepted (${label})`, r && typeof r === 'object', JSON.stringify(r)); }
    catch (e) { ok(`reportForeground with fullscreen ${fs} accepted (${label})`, false, `${e.code} ${e.message}`); }
  }
  await expectErr('reportForeground with fullscreen "yes" -> bad_args', c.call('reportForeground', { pid: process.pid, fullscreen: 'yes' }), 'bad_args');
  await expectErr('reportForeground with fullscreen 1 -> bad_args', c.call('reportForeground', { pid: process.pid, fullscreen: 1 }), 'bad_args');
  try { await c.call('reportForeground', { pid: process.pid, fullscreen: false }); } catch { /* leave Core with a non-full-screen foreground */ }
  if (label === 'console') console.log('  not tested: reportForeground with a pid from another session (needs a second interactive session)');
  return true;
}

// Admin commands from a NON-elevated client must be refused with `forbidden`. The runner is elevated,
// so a de-elevated child is started with `runas /trustlevel:0x20000` (SAFER "Basic User": the
// Administrators group becomes deny-only). If that is not possible here, it is reported, not skipped.
async function nonAdminChecks() {
  section('admin commands from a non-elevated client');
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'osc-nonadmin-'));
  const child = path.join(dir, 'client.js'), out = path.join(dir, 'out.json');
  fs.writeFileSync(child, `'use strict';
const cc = require(${JSON.stringify(path.join(__dirname, '..', 'src', 'core-client.js'))});
const { spawnSync } = require('child_process');
(async () => {
  const res = { elevated: spawnSync('net.exe', ['session'], { windowsHide: true }).status === 0, calls: {} };
  const c = cc.createCoreClient({ timeout: 5000 });
  for (const [cmd, args] of [['status'], ['latency'], ['setMode', { mode: 'balanced' }], ['rollbackAll'], ['approve', { id: 'test-approval' }], ['disablePolicy', { txId: 1 }], ['exportDiagnostics'], ['setEtw', { enabled: false }], ['boost'], ['scorecard'], ['setBoost', { enabled: true }], ['captureBaseline']]) {
    try { const r = await c.call(cmd, args); res.calls[cmd] = { ok: true, result: cmd === 'status' ? { mode: r.mode } : cmd === 'latency' ? { running: r.status && r.status.running } : cmd === 'boost' ? { enabled: r.enabled } : cmd === 'scorecard' ? { baseline: !!r.baseline } : r }; }
    catch (e) { res.calls[cmd] = { ok: false, code: e.code, message: e.message }; }
  }
  c.close();
  require('fs').writeFileSync(${JSON.stringify(out)}, JSON.stringify(res));
})().catch(e => require('fs').writeFileSync(${JSON.stringify(out)}, JSON.stringify({ error: String(e && e.stack || e) })));
`);
  const cmdline = `"\\"${process.execPath}\\" \\"${child}\\""`;
  const r = spawnSync('runas.exe', ['/trustlevel:0x20000', cmdline], { encoding: 'utf8', timeout: 15000, windowsHide: true, windowsVerbatimArguments: true });
  const got = await until(() => fs.existsSync(out), 30000, 250);
  let res = null;
  if (got) { await sleep(200); try { res = JSON.parse(fs.readFileSync(out, 'utf8')); } catch (e) { res = { error: e.message }; } }
  try { fs.rmSync(dir, { recursive: true, force: true }); } catch { /* ignore */ }
  if (!res) { console.log(`  not tested: non-admin rejection (could not start a de-elevated client with runas /trustlevel: exit ${r.status} ${String(r.stdout || '').trim()} ${String(r.stderr || '').trim()} ${r.error ? r.error.message : ''})`); return; }
  if (res.error) { console.log(`  not tested: non-admin rejection (de-elevated client failed: ${res.error.slice(0, 300)})`); return; }
  if (res.elevated) { console.log('  not tested: non-admin rejection (runner is elevated and the runas /trustlevel client was still elevated)'); return; }
  note('de-elevated client started (net session refused => not elevated)');
  ok('non-admin client can still read (status)', res.calls.status && res.calls.status.ok === true, JSON.stringify(res.calls.status));
  ok('non-admin client can read latency', res.calls.latency && res.calls.latency.ok === true, JSON.stringify(res.calls.latency));
  ok('non-admin client can read boost and scorecard', res.calls.boost && res.calls.boost.ok === true && res.calls.scorecard && res.calls.scorecard.ok === true, JSON.stringify([res.calls.boost, res.calls.scorecard]));
  for (const cmd of ['setMode', 'rollbackAll', 'approve', 'disablePolicy', 'exportDiagnostics', 'setEtw', 'setBoost', 'captureBaseline']) ok(`non-admin ${cmd} -> forbidden`, res.calls[cmd] && res.calls[cmd].ok === false && res.calls[cmd].code === 'forbidden', JSON.stringify(res.calls[cmd]));
}

// 60 s soak in Monitor mode: service working set and tick cost, and no applied policy.
async function soak(c, label) {
  section(`60 s engine soak in Monitor mode (${label})`);
  const st0 = await c.call('status');
  if (!ok('soak starts in monitor mode', st0.mode === 'monitor', st0.mode)) return;
  const start = Date.now(), ws = [], cost = [], polCounts = [];
  while (Date.now() - start < 60000) {
    const [h, s, p] = await Promise.all([c.call('health'), c.call('status'), c.call('policies')]);
    ws.push(h.workingSetBytes); cost.push(s.engine.tickCostUs); polCounts.push(p.active.length);
    await sleep(2000);
  }
  const sorted = (a) => [...a].sort((x, y) => x - y), med = (a) => sorted(a)[Math.floor(a.length / 2)];
  const head = med(ws.slice(0, 5)), tail = med(ws.slice(-5)), growth = (tail - head) / head;
  console.log(`  working set: min ${(Math.min(...ws) / MB).toFixed(1)} MB · median ${(med(ws) / MB).toFixed(1)} MB · max ${(Math.max(...ws) / MB).toFixed(1)} MB · first-5 median ${(head / MB).toFixed(1)} MB → last-5 median ${(tail / MB).toFixed(1)} MB (${(growth * 100).toFixed(1)} %) · ${ws.length} samples`);
  console.log(`  tick cost: min ${(Math.min(...cost) / 1000).toFixed(2)} ms · median ${(med(cost) / 1000).toFixed(2)} ms · max ${(Math.max(...cost) / 1000).toFixed(2)} ms`);
  ok('soak: working set < 64 MB throughout', Math.max(...ws) < 64 * MB, `max ${(Math.max(...ws) / MB).toFixed(1)} MB`);
  ok('soak: working set growth <= 20 %', growth <= 0.2, `${(growth * 100).toFixed(1)} %`);
  ok('soak: no active policy at any sample (monitor)', polCounts.every(n => n === 0), polCounts.join(','));
  const jr = await c.call('journal', { limit: 500 });
  const isModeOrRecovery = (t) => /mode|recover|config/i.test(String(t.kind)) || /mode/i.test(String(t.action || ''));
  const applied = jr.entries.filter(t => tsOf(t) >= start - 2000 && ['applied', 'verified', 'verifying'].includes(t.state) && !isModeOrRecovery(t));
  ok('soak: monitor mode applied zero policies (journal)', applied.length === 0, applied.length ? JSON.stringify(applied.slice(0, 3)) : `${jr.entries.filter(t => tsOf(t) >= start - 2000).length} journal entries in the window, none applied`);
}

// Admin commands from this (elevated) runner.
async function adminChecks(c) {
  section('admin commands (elevated runner)');
  await expectErr('setMode with an unknown mode -> bad_args', c.call('setMode', { mode: 'turbo' }), 'bad_args');
  await expectErr('setMode without args -> bad_args', c.call('setMode'), 'bad_args');
  for (const mode of ['balanced', 'monitor']) {
    try {
      const r = await c.call('setMode', { mode });
      const s = await c.call('status');
      ok(`setMode ${mode}`, r.mode === mode && s.mode === mode && s.simulation === (mode === 'monitor'), `result=${JSON.stringify(r)} status.mode=${s.mode} simulation=${s.simulation}`);
      const h = await c.call('hello');
      ok(`hello.mode follows setMode (${mode})`, h.mode === mode, h.mode);
    } catch (e) { ok(`setMode ${mode}`, false, `${e.code} ${e.message}`); }
  }
  const pol = await c.call('policies');
  ok('policies empty after monitor → balanced → monitor', pol.active.length === 0, JSON.stringify(pol.active).slice(0, 200));
  const jr = await c.call('journal', { limit: 50 });
  ok('mode changes are journaled (>= 2 entries mentioning the mode)', jr.entries.filter(t => /mode/i.test(JSON.stringify(t))).length >= 2, `${jr.entries.length} entries`);
  try { const r = await c.call('rollbackAll'); ok('rollbackAll (nothing active) -> { restored: 0, failed: 0 }', r.restored === 0 && r.failed === 0, JSON.stringify(r)); }
  catch (e) { ok('rollbackAll', false, `${e.code} ${e.message}`); }
  try { const r = await c.call('disablePolicy', { txId: 999999999 }); ok('disablePolicy of an unknown txId restores nothing', r.restored === false, JSON.stringify(r)); }
  catch (e) { ok('disablePolicy of an unknown txId is refused', e.code === 'bad_args', `${e.code} ${e.message}`); }
  await expectErr('approve of an unknown id is refused', c.call('approve', { id: 'no-such-approval' }), 'bad_args');
  await expectErr('disablePolicy without txId -> bad_args', c.call('disablePolicy', {}), 'bad_args');
  try {
    const r = await c.call('exportDiagnostics', undefined, { timeout: 15000 });
    const p = r && r.path;
    const pd = (process.env.ProgramData || 'C:\\ProgramData').toLowerCase();
    ok('exportDiagnostics returns a path under %ProgramData%\\OpenSweep', isStr(p) && p.toLowerCase().startsWith(path.join(pd, 'OpenSweep').toLowerCase()), p);
    let j = null; try { j = JSON.parse(fs.readFileSync(p, 'utf8')); } catch { /* checked below */ }
    ok('exportDiagnostics wrote valid JSON', !!j, p && fs.existsSync(p) ? `${fs.statSync(p).size} bytes` : 'missing');
    if (j) ok('diagnostics bundle has no window titles / command lines', privacyKeys(j).length === 0, privacyKeys(j).slice(0, 5).join(', '));
  } catch (e) { ok('exportDiagnostics', false, `${e.code} ${e.message}`); }
  // Kernel tracing off → on (docs/core/etw.md `setEtw`).
  await expectErr('setEtw without enabled -> bad_args', c.call('setEtw', {}), 'bad_args');
  await expectErr('setEtw with enabled as string -> bad_args', c.call('setEtw', { enabled: 'false' }), 'bad_args');
  try {
    const off = await c.call('setEtw', { enabled: false }, { timeout: 15000 });
    ok('setEtw false -> { enabled: false, running: false }', off.enabled === false && off.running === false, JSON.stringify(off));
    const stopped = await until(async () => { const l = await c.call('latency'); return l.status && l.status.running === false ? l : null; }, 5000, 250);
    ok('latency.status.running false after setEtw false', !!stopped, stopped ? (stopped.status.reason || '') : JSON.stringify((await c.call('latency')).status));
    const se = (await c.call('status')).etw;
    ok('status.etw.running false after setEtw false', se && se.running === false, JSON.stringify(se));
    ok('kernel session gone after setEtw false (logman)', !etwSessionExists());
    const on = await c.call('setEtw', { enabled: true }, { timeout: 15000 });
    ok('setEtw true -> { enabled: true }', on.enabled === true && typeof on.running === 'boolean', JSON.stringify(on));
    const t = Date.now();
    const back = await until(async () => { const l = await c.call('latency'); return l.status && l.status.running ? l : null; }, 10000, 250);
    ok('latency.status.running true again within 10 s of setEtw true', !!back, `${Date.now() - t} ms`);
  } catch (e) { ok('setEtw false → true', false, `${e.code} ${e.message}`); try { await c.call('setEtw', { enabled: true }); } catch { /* reported above */ } }
  // Game boost on → off (docs/core/alpha4.md `setBoost`), then the original setting is put back.
  await expectErr('setBoost without enabled -> bad_args', c.call('setBoost', {}), 'bad_args');
  await expectErr('setBoost with enabled as string -> bad_args', c.call('setBoost', { enabled: 'true' }), 'bad_args');
  let boostWas = null;
  try {
    boostWas = (await c.call('boost')).enabled;
    const on = await c.call('setBoost', { enabled: true }, { timeout: 15000 });
    const b1 = await c.call('boost');
    ok('setBoost true -> boost.enabled true', b1.enabled === true && (!on || on.enabled === undefined || on.enabled === true), JSON.stringify({ result: on, enabled: b1.enabled }));
    ok('boost shape after setBoost true', boostShape(b1), JSON.stringify(b1).slice(0, 300));
    const off = await c.call('setBoost', { enabled: false }, { timeout: 15000 });
    const b2 = await c.call('boost');
    ok('setBoost false -> boost.enabled false, not active, nothing throttled', b2.enabled === false && b2.active === false && b2.throttled.length === 0, JSON.stringify({ result: off, boost: b2 }).slice(0, 300));
    const jr2 = await c.call('journal', { limit: 50 });
    ok('setBoost is journaled', jr2.entries.some(t => /boost/i.test(JSON.stringify(t))), `${jr2.entries.length} entries`);
  } catch (e) { ok('setBoost true → false', false, `${e.code} ${e.message}`); }
  if (boostWas === true) { try { await c.call('setBoost', { enabled: true }); note('game boost switched back on (it was on before the test)'); } catch (e) { ok('restore original game boost setting', false, `${e.code} ${e.message}`); } }
  // Scorecard baseline capture.
  try {
    const r = await c.call('captureBaseline', undefined, { timeout: 15000 });
    ok('captureBaseline answers', r && typeof r === 'object', JSON.stringify(r).slice(0, 200));
    const sc = await c.call('scorecard');
    ok('scorecard.baseline non-null after captureBaseline', !!sc.baseline && isTs(sc.baseline.capturedAt) && metricsShape(sc.baseline.metrics), JSON.stringify(sc.baseline).slice(0, 300));
    ok('scorecard shape after captureBaseline', scorecardShape(sc));
  } catch (e) { ok('captureBaseline → scorecard', false, `${e.code} ${e.message}`); }
  const s = await c.call('status');
  ok('ends in monitor mode', s.mode === 'monitor', s.mode);
}

let child = null, childOut = '';
async function cleanup() {
  if (child && child.exitCode === null) { try { child.kill(); } catch { /* gone */ } }
  const st = serviceState();
  if (st.state !== 'NOT_INSTALLED' && st.state !== 'UNKNOWN') {
    note(`cleanup: removing leftover ${SERVICE} service (state ${st.state})`);
    run('sc.exe', ['stop', SERVICE]);
    await until(() => serviceState().state !== 'RUNNING', 10000);
    const u = fs.existsSync(exe) ? run(exe, ['--uninstall']) : run('sc.exe', ['delete', SERVICE]);
    if (u.code !== 0) run('sc.exe', ['delete', SERVICE]);
  }
  if (process.platform === 'win32' && etwSessionExists()) { note(`cleanup: stopping leftover ${ETW_SESSION} session`); run('logman.exe', ['stop', ETW_SESSION, '-ets']); }
}

async function consoleTests() {
  section('console mode (--console)');
  const v = run(exe, ['--version']);
  ok('--version exits 0 and prints a version', v.code === 0 && /\d+\.\d+\.\d+/.test(v.out), JSON.stringify(v.out.slice(0, 120)));
  if (!ok('pipe is free before start', !pipeExists(), pipeExists() ? 'another OpenSweepCore instance owns the pipe' : '')) return false;

  child = spawn(exe, ['--console'], { windowsHide: true, stdio: ['ignore', 'pipe', 'pipe'] });
  child.stdout.on('data', d => { childOut = (childOut + d).slice(-8000); });
  child.stderr.on('data', d => { childOut = (childOut + d).slice(-8000); });
  const t0 = Date.now();
  const up = await until(() => pipeExists() || child.exitCode !== null, 10000, 100);
  if (!ok('pipe appears after starting --console', up && pipeExists(), `${Date.now() - t0} ms`)) { note(`console output:\n${childOut}`); return false; }

  const c = cc.createCoreClient({ timeout: 3000 });
  try {
    // ---------------- hello / health / metrics / topology
    const hello = await c.call('hello');
    ok('hello shape', hello.service === 'OpenSweepCore' && typeof hello.version === 'string' && hello.protocol === 1 && hello.mode === 'monitor' && num(hello.uptimeSec) && hello.uptimeSec >= 0, JSON.stringify(hello));
    ok('hello.pid is the console process', hello.pid === child.pid, `hello.pid=${hello.pid} child.pid=${child.pid}`);
    ok('hello.startedAt is ISO 8601 UTC in the past', typeof hello.startedAt === 'string' && /Z$/.test(hello.startedAt) && !isNaN(Date.parse(hello.startedAt)) && Date.parse(hello.startedAt) <= Date.now() + 2000, hello.startedAt);
    ok('--version matches hello.version', v.out.includes(hello.version), `${hello.version}`);

    const health = await c.call('health');
    ok('health shape', health.status === 'running' && health.mode === 'monitor' && ['uptimeSec', 'workingSetBytes', 'privateBytes', 'cpuTimeMs', 'clients', 'requests', 'errors'].every(k => num(health[k])), JSON.stringify(health));
    ok('health values sane', health.workingSetBytes > 0 && health.privateBytes > 0 && health.clients >= 1 && health.requests >= 1 && health.cpuTimeMs >= 0,
      `ws=${(health.workingSetBytes / 1048576).toFixed(1)} MB private=${(health.privateBytes / 1048576).toFixed(1)} MB`);

    await sleep(1200); // let the service take a second CPU sample
    const m = await c.call('metrics');
    const mem = m.memory || {}, sys = m.system || {};
    ok('metrics shape', typeof m.timestamp === 'string' && m.cpu && num(m.cpu.percent) && ['totalPhys', 'availPhys', 'loadPercent', 'commitTotal', 'commitLimit', 'commitPeak', 'systemCache', 'kernelPaged', 'kernelNonpaged'].every(k => num(mem[k])) && ['processes', 'threads', 'handles'].every(k => num(sys[k])) && num(m.pageSize), JSON.stringify(m));
    ok('metrics: cpu.percent within 0–100', m.cpu.percent >= 0 && m.cpu.percent <= 100, String(m.cpu.percent));
    ok('metrics: memory totals > 0', mem.totalPhys > 0 && mem.availPhys > 0 && mem.availPhys <= mem.totalPhys && mem.commitTotal > 0 && mem.commitLimit > 0);
    ok('metrics: totalPhys matches os.totalmem() (±1%)', Math.abs(mem.totalPhys - os.totalmem()) <= os.totalmem() * 0.01, `${mem.totalPhys} vs ${os.totalmem()}`);
    ok('metrics: commitLimit >= commitTotal, commitPeak >= commitTotal', mem.commitLimit >= mem.commitTotal && mem.commitPeak >= mem.commitTotal, `total=${mem.commitTotal} limit=${mem.commitLimit} peak=${mem.commitPeak}`);
    ok('metrics: loadPercent within 0–100', mem.loadPercent >= 0 && mem.loadPercent <= 100, String(mem.loadPercent));
    ok('metrics: processes/threads/handles > 0', sys.processes > 0 && sys.threads >= sys.processes && sys.handles > 0, JSON.stringify(sys));
    ok('metrics: pageSize is a power of two >= 4096', m.pageSize >= 4096 && (m.pageSize & (m.pageSize - 1)) === 0, String(m.pageSize));
    const cpuSamples = [];
    for (let i = 0; i < 3; i++) { await sleep(600); cpuSamples.push((await c.call('metrics')).cpu.percent); }
    ok('metrics: repeated cpu samples stay within 0–100', cpuSamples.every(x => x >= 0 && x <= 100), cpuSamples.join(', '));

    const t = await c.call('topology');
    ok('topology shape', ['logicalProcessors', 'cores', 'packages', 'numaNodes', 'groups', 'l3Caches'].every(k => Number.isInteger(t[k])) && typeof t.hybrid === 'boolean' && Array.isArray(t.efficiencyClasses), JSON.stringify(t));
    ok('topology: logicalProcessors == os.cpus().length', t.logicalProcessors === os.cpus().length, `${t.logicalProcessors} vs ${os.cpus().length}`);
    ok('topology: 1 <= cores <= logicalProcessors, packages/numaNodes/groups >= 1', t.cores >= 1 && t.cores <= t.logicalProcessors && t.packages >= 1 && t.numaNodes >= 1 && t.groups >= 1);
    ok('topology: hybrid implies >1 efficiency class', !t.hybrid || t.efficiencyClasses.length > 1, JSON.stringify(t.efficiencyClasses));

    await engineReadChecks(c, 'console', child.pid);
    await nonAdminChecks();

    // ---------------- protocol errors (raw frames)
    section('protocol errors');
    let r = await rawConnect();
    await r.sendJson({ v: 1, id: 5, cmd: 'definitely_not_a_command' });
    let f = await r.next();
    ok('unknown command -> unknown_cmd (id echoed)', f && f.ok === false && f.error && f.error.code === 'unknown_cmd' && f.id === 5 && f.v === 1, JSON.stringify(f));
    await r.sendJson({ v: 2, id: 6, cmd: 'hello' });
    f = await r.next();
    ok('v=2 -> bad_version', f && f.ok === false && f.error.code === 'bad_version', JSON.stringify(f));
    ok('bad_version echoes the request id', f && f.id === 6, `id=${f && f.id}`);
    await r.sendJson({ id: 7, cmd: 'hello' });
    f = await r.next();
    ok('missing v -> bad_version', f && f.ok === false && f.error.code === 'bad_version', JSON.stringify(f));
    await r.send(frameOf(9, Buffer.from('not json{')));
    f = await r.next();
    ok('invalid JSON -> bad_json with id 0', f && f.ok === false && f.error.code === 'bad_json' && f.id === 0, JSON.stringify(f));
    await r.send(frameOf(2, Buffer.from('[]')));
    f = await r.next();
    ok('JSON that is not an object is rejected', f && f.ok === false, JSON.stringify(f));
    await r.sendJson({ v: 1, id: 8, cmd: 'hello', args: [1, 2] });
    f = await r.next();
    ok('args as array -> bad_args', f && f.ok === false && f.error.code === 'bad_args' && f.id === 8, JSON.stringify(f));
    await r.sendJson({ v: 1, id: 9, cmd: 'hello', args: 'x' });
    f = await r.next();
    ok('args as string -> bad_args', f && f.ok === false && f.error.code === 'bad_args', JSON.stringify(f));
    await r.sendJson({ v: 1, id: 10, cmd: 'hello', args: {}, extra: 'ignored' });
    f = await r.next();
    ok('unknown top-level fields are ignored', f && f.ok === true && f.id === 10, JSON.stringify(f).slice(0, 120));
    ok('connection still usable after error replies', !r.closed);
    r.close();

    // exactly 64 KiB request is accepted
    r = await rawConnect();
    const base = JSON.stringify({ v: 1, id: 11, cmd: 'hello', pad: '' });
    const big = Buffer.from(JSON.stringify({ v: 1, id: 11, cmd: 'hello', pad: 'x'.repeat(65536 - Buffer.byteLength(base)) }));
    await r.send(frameOf(big.length, big));
    f = await r.next(5000);
    ok('request of exactly 65 536 bytes is accepted', big.length === 65536 && f && f.ok === true && f.id === 11, `len=${big.length} ${f ? JSON.stringify(f).slice(0, 80) : String(f)}`);
    r.close();

    // oversized frame
    r = await rawConnect();
    await r.send(frameOf(65537, Buffer.alloc(1024, 0x20)));
    f = await r.next(3000);
    ok('frame > 64 KiB -> too_large', f && f.ok === false && f.error.code === 'too_large', JSON.stringify(f));
    ok('server closes the connection after too_large', await r.waitClosed(3000));
    r.close();

    // huge declared length (4 GiB - 1) must not allocate / hang
    r = await rawConnect();
    await r.send(frameOf(0xffffffff));
    f = await r.next(3000);
    ok('declared length 0xFFFFFFFF -> too_large and close', f && f.ok === false && f.error.code === 'too_large' && await r.waitClosed(3000), JSON.stringify(f));
    r.close();

    // zero-length frame
    r = await rawConnect();
    await r.send(frameOf(0));
    f = await r.next(3000);
    ok('zero-length frame -> bad_frame', f && f.ok === false && f.error.code === 'bad_frame', JSON.stringify(f));
    note(`after bad_frame the server ${await r.waitClosed(1000) ? 'closed' : 'kept open'} the connection`);
    r.close();

    // garbage bytes (fuzz-ish): server must survive
    for (let i = 0; i < 20; i++) {
      try {
        const g = await rawConnect();
        const len = Math.floor(Math.random() * 200) + 1, body = Buffer.from([...Array(len)].map(() => Math.floor(Math.random() * 256)));
        await g.send(frameOf(len, body)); await g.next(500); g.close();
      } catch { /* the server may refuse; checked below */ }
    }
    c.close();
    ok('service still answers after 20 random-garbage frames', (await probeOnce()).available === true);

    // ---------------- concurrency
    section('concurrency');
    c.close();
    await sleep(300);
    const held = [];
    try {
      const opened = await Promise.allSettled([...Array(8)].map(() => rawConnect(5000)));
      opened.forEach(o => { if (o.status === 'fulfilled') held.push(o.value); });
      ok('8 simultaneous connections accepted', held.length === 8, `${held.length}/8 (${opened.filter(o => o.status === 'rejected').map(o => o.reason.code || o.reason.message).join(', ')})`);
      const hs = await Promise.all(held.map(async (x, i) => { await x.sendJson({ v: 1, id: 100 + i, cmd: 'hello' }); return x.next(3000); }));
      ok('all 8 connections served concurrently', hs.every((x, i) => x && x.ok && x.id === 100 + i));

      // 9th: libuv waits on ERROR_PIPE_BUSY (queued); a server that accepts-and-closes means refused.
      const ninth = { state: 'pending', conn: null, err: null };
      const s9 = net.connect(PIPE);
      s9.on('error', (e) => { ninth.state = 'error'; ninth.err = e; });
      s9.on('connect', () => { ninth.state = 'connected'; ninth.conn = wrapRaw(s9); });
      await sleep(2000);
      let verdict;
      if (ninth.state === 'pending') verdict = 'queued (client waits for a free pipe instance)';
      else if (ninth.state === 'error') verdict = `refused (${ninth.err.code || ninth.err.message})`;
      else {
        await ninth.conn.sendJson({ v: 1, id: 999, cmd: 'hello' });
        const a = await ninth.conn.next(1500);
        verdict = a && a.ok ? 'SERVED (limit not enforced)' : ninth.conn.closed ? 'refused (accepted then closed)' : 'queued (connected, no reply while 8 are busy)';
      }
      ok('9th concurrent connection is refused or queued', !/SERVED/.test(verdict), verdict);
      // Free one slot: a queued 9th should now get through.
      held.shift().close();
      if (verdict.startsWith('queued')) {
        await until(() => ninth.state !== 'pending', 5000, 100);
        let a = null;
        if (ninth.conn) { await ninth.conn.sendJson({ v: 1, id: 998, cmd: 'hello' }); a = await ninth.conn.next(5000); }
        ok('queued 9th connection is served once a slot frees up', a && a.ok === true && a.id === 998, `state=${ninth.state}`);
      } else if (ninth.state !== 'error') note('9th connection was refused; not waiting for it');
      if (ninth.conn) ninth.conn.close(); else s9.destroy();
    } finally { held.forEach(x => x.close()); }
    await sleep(500);

    // 20 clients x 50 requests, at most 8 connected at a time.
    const before = (await callOnce('health')).requests;
    const CLIENTS = 20, PER = 50, POOL = 8;
    const t1 = Date.now();
    let next = 0, bad = 0, done = 0, mismatched = 0;
    const errors = new Map();
    await Promise.all([...Array(POOL)].map(async () => {
      while (next < CLIENTS) {
        next++;
        const cl = cc.createCoreClient({ timeout: 10000 });
        try {
          for (let i = 0; i < PER; i++) {
            const cmd = ['hello', 'health', 'metrics', 'topology'][i % 4];
            try {
              const res = await cl.call(cmd);
              if (cmd === 'hello' ? res.service !== 'OpenSweepCore' : cmd === 'health' ? res.status !== 'running' : cmd === 'metrics' ? !res.memory : !res.logicalProcessors) mismatched++;
              done++;
            } catch (e) { bad++; errors.set(e.code, (errors.get(e.code) || 0) + 1); }
          }
        } finally { cl.close(); }
      }
    }));
    const ms = Date.now() - t1;
    ok(`${CLIENTS} clients x ${PER} requests (max ${POOL} connected) all succeed`, bad === 0 && done === CLIENTS * PER, `${done} ok, ${bad} failed ${JSON.stringify(Object.fromEntries(errors))}, ${ms} ms, ${(done / (ms / 1000)).toFixed(0)} req/s`);
    ok('every reply matched its request', mismatched === 0, `${mismatched} mismatched`);
    // pipelined requests on one connection
    const pc = cc.createCoreClient({ timeout: 10000 });
    const pr = await Promise.allSettled([...Array(PER)].map(() => pc.call('hello')));
    ok(`${PER} pipelined requests on one connection`, pr.every(x => x.status === 'fulfilled'), `${pr.filter(x => x.status === 'rejected').length} failed`);
    const after = (await pc.call('health')).requests;
    ok('health.requests counted the load', after - before >= CLIENTS * PER, `+${after - before}`);
    pc.close();

    // ---------------- timeouts
    section('timeouts');
    r = await rawConnect();
    await r.send(Buffer.concat([frameOf(100), Buffer.from('{"v":1,"id":1,')]));
    const ts = Date.now();
    const closed = await r.waitClosed(9000);
    const el = r.closedAt ? r.closedAt - ts : Date.now() - ts;
    const tf = await r.next(1);
    ok('stalled partial frame is disconnected within ~6 s', closed && el <= 8000, `${closed ? 'closed' : 'still open'} after ${el} ms${tf ? `, last frame ${JSON.stringify(tf)}` : ''}`);
    ok('stalled partial frame is not cut off too early (>= 4 s)', !closed || el >= 4000, `${el} ms`);
    r.close();
    console.log('  not tested: idle timeout (60 s)');
    ok('service still answers after the stall test', (await probeOnce()).available === true);
  } catch (e) {
    ok('console-mode tests ran without an exception', false, `${e.code || ''} ${e.stack || e.message}`);
  } finally { c.close(); }

  // ---------------- shutdown
  section('console shutdown');
  child.kill();
  const exited = await until(() => child.exitCode !== null || child.signalCode !== null, 5000, 100);
  ok('console process terminated', !!exited);
  const gone = await until(() => !pipeExists(), 5000, 100);
  ok('pipe disappears after the process is killed', !!gone);
  note(`after killing the console process the ${ETW_SESSION} session ${etwSessionExists() ? 'is still present (expected after a hard kill; the service must stop it at start-up)' : 'is gone'}`);
  const p = await probeOnce();
  ok('client reports unavailable after shutdown', p.available === false && p.error.code === 'unavailable', p.error && p.error.code);
  return true;
}

async function serviceTests() {
  section('Windows service lifecycle');
  const admin = run('net.exe', ['session']).code === 0;
  if (!ok('running elevated (needed to install a service)', admin, admin ? '' : 'run this test from an administrator shell')) {
    console.log('  not tested: service lifecycle (not elevated)');
    return;
  }
  let st = serviceState();
  if (st.state !== 'NOT_INSTALLED') { note(`service already present (${st.state}); removing it first`); await cleanup(); }
  ok('service absent before install', serviceState().state === 'NOT_INSTALLED');

  const ins = run(exe, ['--install']);
  ok('--install exits 0', ins.code === 0, ins.out.split(/\r?\n/).slice(-2).join(' | '));
  st = serviceState();
  ok('service registered after --install', st.state !== 'NOT_INSTALLED' && st.state !== 'UNKNOWN', st.state);
  const qc = run('sc.exe', ['qc', SERVICE]);
  note(`sc qc: ${(qc.out.match(/(START_TYPE|BINARY_PATH_NAME|SERVICE_START_NAME)\s*:\s*.*/g) || []).map(x => x.replace(/\s+/g, ' ')).join(' | ')}`);

  if (st.state !== 'RUNNING') {
    const s = run('sc.exe', ['start', SERVICE]);
    if (s.code !== 0 && !/1056/.test(s.out)) note(`sc start: ${s.out.split(/\r?\n/).filter(Boolean).slice(-1)[0]}`);
  } else note('--install already started the service');
  const t0 = Date.now();
  st = await until(() => { const x = serviceState(); return x.state === 'RUNNING' ? x : null; }, 15000, 300) || serviceState();
  ok('service RUNNING after sc start (≤ 15 s)', st.state === 'RUNNING', `${st.state} after ${Date.now() - t0} ms`);

  if (st.state === 'RUNNING') {
    await until(() => pipeExists(), 5000, 100);
    const c = cc.createCoreClient({ timeout: 3000 });
    try {
      const hello = await c.call('hello');
      ok('hello over the pipe from the service', hello.service === 'OpenSweepCore' && hello.protocol === 1 && hello.mode === 'monitor', JSON.stringify(hello));
      ok('hello.pid is the service process', hello.pid === st.pid, `hello.pid=${hello.pid} service pid=${st.pid}`);
      const m = await c.call('metrics');
      ok('metrics from the service', m.memory && m.memory.totalPhys > 0);
      if (await engineReadChecks(c, 'service', st.pid)) {
        await soak(c, 'service');
        await adminChecks(c);
      } else console.log('  not tested: soak and admin commands (engine read commands failed)');
    } catch (e) { ok('service answers over the pipe', false, `${e.code} ${e.message}`); }
    finally { c.close(); }

    section('pipe security descriptor (informational)');
    const ps = path.join(process.env.SystemRoot || 'C:\\Windows', 'System32', 'WindowsPowerShell', 'v1.0', 'powershell.exe');
    const acl = run(ps, ['-NoProfile', '-NonInteractive', '-Command',
      `$p='\\\\.\\pipe\\${PIPE_NAME}'; try { $a = [System.IO.Directory]::GetAccessControl($p) } catch { $a = Get-Acl -LiteralPath $p }; ` +
      `'SDDL: ' + $a.Sddl; $a.Access | Format-Table IdentityReference,AccessControlType,FileSystemRights -AutoSize | Out-String -Width 200`]);
    console.log(acl.out.split(/\r?\n/).map(l => '  ' + l).join('\n') || '  (no output)');
    note('expected per spec: D:P(D;;GA;;;NU)(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)');
  }

  const stop = run('sc.exe', ['stop', SERVICE]);
  if (stop.code !== 0) note(`sc stop: ${stop.out.split(/\r?\n/).filter(Boolean).slice(-1)[0]}`);
  const t1 = Date.now();
  const stopped = await until(() => serviceState().state === 'STOPPED', 10000, 300);
  ok('service STOPPED after sc stop (≤ 10 s)', !!stopped, `${serviceState().state} after ${Date.now() - t1} ms`);
  ok('pipe gone after service stop', !!await until(() => !pipeExists(), 5000, 100));
  const noOrphan = await until(() => !etwSessionExists(), 5000, 250);
  ok(`no orphan ${ETW_SESSION} session after service stop`, !!noOrphan, noOrphan ? '' : 'logman still lists it');

  const un = run(exe, ['--uninstall']);
  ok('--uninstall exits 0', un.code === 0, un.out.split(/\r?\n/).slice(-2).join(' | '));
  const goneSvc = await until(() => serviceState().state === 'NOT_INSTALLED', 10000, 300);
  ok('service removed after --uninstall', !!goneSvc, serviceState().state);
}

(async () => {
  console.log(`OpenSweep Core IPC integration test\nexe: ${exe}\nnode ${process.version} · ${os.cpus().length} logical CPUs · ${(os.totalmem() / 1073741824).toFixed(1)} GB RAM`);
  if (process.platform !== 'win32') { ok('running on Windows', false, 'this test drives a Windows named pipe and service; use test/core-client.js elsewhere'); process.exit(1); }
  if (!ok('exe exists', fs.existsSync(exe), exe)) process.exit(1);
  try {
    await consoleTests();
    await serviceTests();
  } catch (e) {
    ok('test run completed without an unexpected exception', false, e.stack || e.message);
  } finally {
    await cleanup().catch(() => {});
  }
  if (failed && childOut) console.log(`\n-- last console output --\n${childOut}`);
  console.log(failed ? `\n${failed} check(s) FAILED` : '\nAll OpenSweep Core IPC checks passed');
  process.exit(failed ? 1 : 0);
})();
