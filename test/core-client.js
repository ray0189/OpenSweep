'use strict';
// Unit test for src/core-client.js against an in-process mock server (any OS).
// On Windows the mock listens on a private named pipe, elsewhere on a Unix socket.
// Prints ✓/✗ per check and exits 1 on any failure.
const net = require('net');
const os = require('os');
const path = require('path');
const fs = require('fs');
const cc = require('../src/core-client');

let failed = 0;
const ok = (name, cond, extra = '') => { console.log(`${cond ? '✓' : '✗'} ${name}${extra ? ' — ' + extra : ''}`); if (!cond) failed++; };
const expectCode = async (name, p, code) => {
  try { await p; ok(name, false, 'resolved, expected error ' + code); }
  catch (e) { ok(name, e.code === code, `code=${e.code} (${e.message})`); }
};

const sockPath = process.platform === 'win32' ? `\\\\.\\pipe\\OpenSweepCoreTest-${process.pid}` : path.join(os.tmpdir(), `osc-test-${process.pid}.sock`);

// Mock server: speaks protocol v1. Special commands drive the edge cases.
function startServer() {
  const conns = new Set();
  const server = net.createServer((s) => {
    conns.add(s); s.on('close', () => conns.delete(s)); s.on('error', () => {});
    const dec = new cc.FrameDecoder(64 * 1024);
    const held = [];
    s.on('data', (chunk) => {
      let msgs; try { msgs = dec.push(chunk); } catch { s.destroy(); return; }
      for (const m of msgs) {
        const reply = (o) => s.write(cc.encodeFrame({ v: 1, id: m.id, ...o }, 2 * 1024 * 1024));
        if (m.v !== 1) reply({ ok: false, error: { code: 'bad_version', message: 'v must be 1' } });
        else if (m.cmd === 'hello') reply({ ok: true, result: { service: 'OpenSweepCore', version: '0.1.0', protocol: 1, pid: process.pid, mode: 'monitor' } });
        else if (m.cmd === 'echo') reply({ ok: true, result: { args: m.args === undefined ? null : m.args } });
        else if (m.cmd === 'slow') { /* never answers */ }
        else if (m.cmd === 'hold') held.push(m); // answered in reverse order on 'flush'
        else if (m.cmd === 'flush') { for (const x of held.reverse()) s.write(cc.encodeFrame({ v: 1, id: x.id, ok: true, result: { id: x.id } })); held.length = 0; reply({ ok: true, result: {} }); }
        else if (m.cmd === 'drop') s.destroy();
        else if (m.cmd === 'huge') { const head = Buffer.alloc(4); head.writeUInt32LE(2 * 1024 * 1024); s.write(head); }
        else reply({ ok: false, error: { code: 'unknown_cmd', message: `unknown command ${m.cmd}` } });
      }
    });
  });
  return new Promise((res) => server.listen(sockPath, () => res({ server, conns })));
}

(async () => {
  // framing
  const f = cc.encodeFrame({ a: 1 });
  ok('encodeFrame writes little-endian length', f.readUInt32LE(0) === f.length - 4);
  const d = new cc.FrameDecoder();
  const two = Buffer.concat([cc.encodeFrame({ x: 1 }), cc.encodeFrame({ y: 'é' })]);
  const got = [...d.push(two.subarray(0, 3)), ...d.push(two.subarray(3, 9)), ...d.push(two.subarray(9))];
  ok('FrameDecoder reassembles split frames', got.length === 2 && got[0].x === 1 && got[1].y === 'é');
  try { new cc.FrameDecoder().push(Buffer.from([0, 0, 0, 0])); ok('zero-length frame rejected', false); } catch (e) { ok('zero-length frame rejected', e.code === 'bad_frame'); }
  try { cc.encodeFrame('x'.repeat(70000)); ok('oversized request rejected locally', false); } catch (e) { ok('oversized request rejected locally', e.code === 'too_large'); }

  // unavailable
  if (process.platform !== 'win32') await expectCode('default pipe on non-Windows reports unavailable', cc.createCoreClient().call('hello'), 'unavailable');
  await expectCode('missing pipe reports unavailable', cc.createCoreClient({ pipePath: sockPath + '-missing' }).call('hello'), 'unavailable');

  if (process.platform !== 'win32') { try { fs.unlinkSync(sockPath); } catch { /* none */ } }
  const { server, conns } = await startServer();
  const c = cc.createCoreClient({ pipePath: sockPath, timeout: 800 });
  try {
    const hello = await c.call('hello');
    ok('hello resolves with result', hello && hello.service === 'OpenSweepCore' && hello.protocol === 1);
    const e = await c.call('echo', { a: [1, 2] });
    ok('args are sent as an object', e.args && e.args.a.length === 2);
    await expectCode('server error becomes Error with .code', c.call('nope'), 'unknown_cmd');
    await expectCode('args must be an object', c.call('echo', [1]), 'bad_args');
    const t0 = Date.now();
    await expectCode('per-request timeout', c.call('slow'), 'timeout');
    ok('timeout fires near the configured 800 ms', Date.now() - t0 >= 750 && Date.now() - t0 < 2000, `${Date.now() - t0} ms`);
    ok('connection survives a timeout', (await c.call('hello')).service === 'OpenSweepCore');

    const ps = [1, 2, 3, 4, 5].map(() => c.call('hold'));
    await c.call('flush');
    const ids = await Promise.all(ps);
    ok('out-of-order replies matched by id', ids.every((r, i, a) => i === 0 || r.id > a[i - 1].id));

    const par = await Promise.all([...Array(50)].map((_, i) => c.call('echo', { i })));
    ok('50 pipelined requests all answered correctly', par.every((r, i) => r.args.i === i));

    await expectCode('server closing the connection fails the request', c.call('drop'), 'disconnected');
    ok('next call reconnects automatically', (await c.call('hello')).service === 'OpenSweepCore');
    await expectCode('oversized response rejected', c.call('huge'), 'too_large');
    ok('reconnects after an oversized response', (await c.call('hello')).service === 'OpenSweepCore');

    const p = await c.probe();
    ok('probe() reports available', p.available === true && p.hello.service === 'OpenSweepCore');
    for (const s of conns) s.destroy();
    server.close();
    await new Promise(r => setTimeout(r, 100));
    const p2 = await c.probe();
    ok('probe() never throws when the server is gone', p2.available === false && !!p2.error.code, p2.error && p2.error.code);
  } finally {
    c.close(); server.close();
    if (process.platform !== 'win32') { try { fs.unlinkSync(sockPath); } catch { /* gone */ } }
  }
  console.log(failed ? `\n${failed} check(s) FAILED` : '\nAll core-client checks passed');
  process.exit(failed ? 1 : 0);
})().catch((e) => { console.error('✗ unexpected error:', e); process.exit(1); });
