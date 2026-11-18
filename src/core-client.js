'use strict';
// Node client for the OpenSweep Core named pipe (protocol v1, see docs/core/ipc-protocol.md).
// No dependencies. Frames are a 4-byte little-endian length followed by UTF-8 JSON.
// One connection is kept open and reused; if it drops, the next call reconnects.
const net = require('net');

const PIPE_PATH = '\\\\.\\pipe\\OpenSweepCore.v1';
const PROTOCOL_VERSION = 1;
const MAX_REQUEST = 64 * 1024;        // server rejects larger frames with too_large
const MAX_RESPONSE = 1024 * 1024;     // protocol limit for responses
const DEFAULT_TIMEOUT = 3000;

const isSupported = () => process.platform === 'win32';

function coreError(code, message) {
  const e = new Error(message || code);
  e.code = code;
  return e;
}

// Encode one frame (Buffer). Throws too_large when the JSON exceeds `max` bytes.
function encodeFrame(obj, max = MAX_REQUEST) {
  const body = Buffer.from(typeof obj === 'string' ? obj : JSON.stringify(obj), 'utf8');
  if (body.length > max) throw coreError('too_large', `Frame of ${body.length} bytes exceeds ${max}`);
  const head = Buffer.alloc(4);
  head.writeUInt32LE(body.length, 0);
  return Buffer.concat([head, body]);
}

// Incremental frame decoder. push(chunk) returns an array of parsed JSON values.
// Throws (with .code) on a zero-length frame, an oversized frame or invalid JSON.
class FrameDecoder {
  constructor(max = MAX_RESPONSE) { this.max = max; this.buf = Buffer.alloc(0); }
  push(chunk) {
    this.buf = this.buf.length ? Buffer.concat([this.buf, chunk]) : chunk;
    const out = [];
    while (this.buf.length >= 4) {
      const n = this.buf.readUInt32LE(0);
      if (n === 0) throw coreError('bad_frame', 'Zero-length frame from server');
      if (n > this.max) throw coreError('too_large', `Response of ${n} bytes exceeds ${this.max}`);
      if (this.buf.length < 4 + n) break;
      const text = this.buf.subarray(4, 4 + n).toString('utf8');
      this.buf = this.buf.subarray(4 + n);
      try { out.push(JSON.parse(text)); } catch { throw coreError('bad_response', 'Server sent invalid JSON'); }
    }
    return out;
  }
}

function createCoreClient({ pipePath = PIPE_PATH, timeout = DEFAULT_TIMEOUT, maxResponse = MAX_RESPONSE } = {}) {
  let sock = null;          // current socket (or null)
  let connecting = null;    // Promise<socket> while a connect is in flight
  let nextId = 1;
  const pending = new Map(); // id -> { resolve, reject, timer }

  function failAll(err) {
    for (const [id, p] of pending) { clearTimeout(p.timer); pending.delete(id); p.reject(err); }
  }
  function drop(err) {
    const s = sock; sock = null; connecting = null;
    if (s) { s.removeAllListeners('data'); s.destroy(); }
    failAll(err || coreError('disconnected', 'Connection to OpenSweep Core was closed'));
  }

  function connect() {
    if (sock) return Promise.resolve(sock);
    if (connecting) return connecting;
    if (!isSupported() && pipePath === PIPE_PATH) return Promise.reject(coreError('unavailable', 'OpenSweep Core is only available on Windows'));
    connecting = new Promise((resolve, reject) => {
      const s = net.connect(pipePath);
      const decoder = new FrameDecoder(maxResponse);
      const onConnectError = (e) => {
        connecting = null;
        reject(coreError(e.code === 'ENOENT' ? 'unavailable' : 'connect_failed', e.code === 'ENOENT' ? 'OpenSweep Core is not running (pipe not found)' : `Could not connect to OpenSweep Core: ${e.message}`));
      };
      s.once('error', onConnectError);
      s.once('connect', () => {
        s.removeListener('error', onConnectError);
        sock = s; connecting = null;
        s.on('data', (chunk) => {
          let msgs;
          try { msgs = decoder.push(chunk); } catch (e) { drop(e); return; }
          for (const m of msgs) {
            const p = m && pending.get(m.id);
            if (!p) continue; // late reply to a request that already timed out
            pending.delete(m.id); clearTimeout(p.timer);
            if (m.ok === true) p.resolve(m.result);
            else if (m.ok === false) p.reject(coreError((m.error && m.error.code) || 'internal', (m.error && m.error.message) || 'OpenSweep Core returned an error'));
            else p.reject(coreError('bad_response', 'Malformed response from OpenSweep Core'));
          }
          // A reply with id 0 means the server could not parse a request: fail everything in flight.
          if (msgs.some(m => m && m.id === 0 && m.ok === false)) {
            const m = msgs.find(x => x && x.id === 0);
            failAll(coreError((m.error && m.error.code) || 'bad_response', (m.error && m.error.message) || 'Request rejected'));
          }
        });
        s.on('error', (e) => { if (sock === s) drop(coreError('disconnected', `OpenSweep Core connection error: ${e.message}`)); });
        s.on('close', () => { if (sock === s) drop(); });
        resolve(s);
      });
    });
    return connecting;
  }

  // Send one command; resolves with `result` or rejects with Error(.code).
  async function call(cmd, args, opts = {}) {
    if (typeof cmd !== 'string' || !cmd) throw coreError('bad_args', 'cmd must be a non-empty string');
    if (args !== undefined && (args === null || typeof args !== 'object' || Array.isArray(args))) throw coreError('bad_args', 'args must be an object');
    const ms = opts.timeout || timeout;
    const id = nextId; nextId = nextId >= 0x7fffffff ? 1 : nextId + 1;
    const frame = encodeFrame(args === undefined ? { v: PROTOCOL_VERSION, id, cmd } : { v: PROTOCOL_VERSION, id, cmd, args });
    const deadline = Date.now() + ms;
    // The timeout covers connecting too (a busy pipe can make connect wait).
    let timer;
    const timed = new Promise((_, reject) => { timer = setTimeout(() => reject(coreError('timeout', `OpenSweep Core did not answer "${cmd}" within ${ms} ms`)), ms); });
    let s;
    try { s = await Promise.race([connect(), timed]); }
    catch (e) { clearTimeout(timer); throw e; }
    clearTimeout(timer);
    return new Promise((resolve, reject) => {
      const left = Math.max(1, deadline - Date.now());
      const t = setTimeout(() => { pending.delete(id); reject(coreError('timeout', `OpenSweep Core did not answer "${cmd}" within ${ms} ms`)); }, left);
      pending.set(id, { resolve, reject, timer: t });
      s.write(frame, (err) => { if (err && pending.has(id)) { pending.delete(id); clearTimeout(t); reject(coreError('disconnected', `Write failed: ${err.message}`)); } });
    });
  }

  // Never throws: { available, hello, error }
  async function probe(opts) {
    try { return { available: true, hello: await call('hello', undefined, opts) }; }
    catch (e) { return { available: false, hello: null, error: { code: e.code || 'internal', message: e.message } }; }
  }

  function close() { drop(coreError('closed', 'Client closed')); }

  return { call, probe, close, get connected() { return !!sock; }, pipePath };
}

module.exports = { createCoreClient, encodeFrame, FrameDecoder, coreError, isSupported, PIPE_PATH, PROTOCOL_VERSION, MAX_REQUEST, MAX_RESPONSE, DEFAULT_TIMEOUT };
