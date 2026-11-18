'use strict';
// Windows integration test for OpenSweepCore.exe: named-pipe protocol v1 (docs/core/ipc-protocol.md)
// in --console mode, then the Windows service lifecycle. Needs Windows and an elevated shell
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
