'use strict';
// Space analyzer manager (main process): runs the scan worker, keeps the latest result,
// answers tree/treemap/search queries, and keeps scan snapshots for growth history.
const { Worker } = require('worker_threads');
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');

let scan = null;      // { root, tree, types, age, top, insights, stats, at }
let worker = null;

function start(root, onProgress) {
  if (worker) worker.postMessage('cancel');
  return new Promise((resolve, reject) => {
    const w = new Worker(path.join(__dirname, 'diskscan-worker.js'), { workerData: { root } });
    worker = w;
    w.on('message', m => {
      if (m.type === 'progress') onProgress && onProgress(m);
      else if (m.type === 'done') {
        if (worker === w) worker = null;
        scan = { root, ...m, at: new Date().toISOString() };
        resolve(summary());
        w.terminate();
      }
    });
    w.on('error', e => { if (worker === w) worker = null; reject(e); });
  });
}
function cancel() { if (worker) worker.postMessage('cancel'); }

function summary() {
  if (!scan) return null;
  const { root, tree, types, age, insights, stats, at, cancelled } = scan;
  return { root, at, cancelled, total: tree.s, files: tree.f, dirs: tree.d, stats, types: types.slice(0, 60), age, insights: insights.map(i => ({ ...i, paths: i.paths.slice(0, 50) })) };
}

function find(segs) {
  if (!scan) throw new Error('Scan a drive or folder first');
  let n = scan.tree;
  for (const s of segs || []) { n = n.c.find(c => c.n === s); if (!n) throw new Error('Folder not found in the last scan — scan again'); }
  return n;
}
const brief = (c) => ({ name: c.n, size: c.s, files: c.f, dirs: c.d, mtime: c.m, denied: !!c.denied, hasKids: c.c.length > 0 || c.fl.length > 0 });
function node(segs, { kids = 12 } = {}) {
  const n = find(segs);
  const full = path.join(scan.root, ...(segs || []));
  return {
    path: full, segs: segs || [], name: segs && segs.length ? n.n : scan.root, size: n.s, files: n.f, dirs: n.d, mtime: n.m,
    children: n.c.map(c => ({ ...brief(c), kids: c.c.slice(0, kids).map(brief).concat(c.fl.slice(0, Math.max(0, kids - c.c.length)).map(f => ({ name: f[0], size: f[1], mtime: f[2], file: true }))) })),
    fileItems: n.fl.map(f => ({ name: f[0], size: f[1], mtime: f[2], file: true })),
    rest: n.rest ? { count: n.rest[0], size: n.rest[1] } : null,
  };
}
function top(limit = 300) { return scan ? scan.top.slice(0, limit).map(([p, s, m]) => ({ path: p, size: s, mtime: m })) : []; }

function search({ q = '', minMB = 0, olderDays = 0, cat = '' } = {}) {
  if (!scan) return [];
  const out = [];
  const needle = String(q).toLowerCase();
  const cutoff = olderDays ? Date.now() - olderDays * 86400e3 : Infinity;
  const exts = cat ? new Set(scan.types.filter(t => t.cat === cat).map(t => t.ext)) : null;
  (function walk(n, dir) {
    for (const f of n.fl) {
      if (f[1] < minMB * 1024 * 1024 || f[2] > cutoff) continue;
      if (needle && !f[0].toLowerCase().includes(needle)) continue;
      if (exts && !exts.has(path.extname(f[0]).toLowerCase() || '(none)')) continue;
      out.push({ path: path.join(dir, f[0]), size: f[1], mtime: f[2] });
    }
    for (const c of n.c) walk(c, path.join(dir, c.n));
  })(scan.tree, scan.root);
  return out.sort((a, b) => b.size - a.size).slice(0, 1000);
}

// Is this path part of the last scan? (Used to validate delete/move/compress requests.)
function known(p) {
  if (!scan) return null;
  const rel = path.relative(scan.root, p);
  if (!rel || rel.startsWith('..') || path.isAbsolute(rel)) return null;
  const segs = rel.split(path.sep);
  const name = segs.pop();
  let n = scan.tree;
  const chain = [n];
  for (const s of segs) { n = n.c.find(c => c.n === s); if (!n) return null; chain.push(n); }
  const dir = n.c.find(c => c.n === name);
  if (dir) return { kind: 'dir', size: dir.s, files: dir.f, chain, parent: n, entry: dir };
  const file = n.fl.find(f => f[0] === name);
  if (file) return { kind: 'file', size: file[1], files: 1, chain, parent: n, entry: file };
  // A file that was folded into "smaller files" but appears in insights/top lists
  if (scan.top.some(t => t[0] === p) || scan.insights.some(i => i.paths.some(x => x.path === p))) return { kind: 'file', size: 0, files: 1, chain, parent: n, entry: null };
  return null;
}
// After a delete/move: take the item out of the tree and shrink every parent.
function forget(p) {
  const k = known(p);
  if (!k) return;
  for (const n of k.chain) { n.s -= k.size; n.f -= k.files; }
  if (k.kind === 'dir') k.parent.c = k.parent.c.filter(c => c !== k.entry);
  else if (k.entry) k.parent.fl = k.parent.fl.filter(f => f !== k.entry);
  scan.top = scan.top.filter(t => !(t[0] === p || t[0].startsWith(p + path.sep)));
  for (const i of scan.insights) { const before = i.paths.length; i.paths = i.paths.filter(x => !(x.path === p || x.path.startsWith(p + path.sep))); if (i.paths.length !== before) { i.size = i.paths.reduce((a, x) => a + x.size, 0); i.count = i.paths.length; } }
}

function toCsv(segs) {
  const n = node(segs);
  const esc = (s) => `"${String(s).replace(/"/g, '""')}"`;
  const rows = [['Type', 'Name', 'Size (bytes)', 'Files', 'Folders', 'Last modified']];
  n.children.forEach(c => rows.push(['Folder', c.name, c.size, c.files, c.dirs, c.mtime ? new Date(c.mtime).toISOString() : '']));
  n.fileItems.forEach(f => rows.push(['File', f.name, f.size, 1, 0, new Date(f.mtime).toISOString()]));
  if (n.rest) rows.push(['Files', `(${n.rest.count} smaller files)`, n.rest.size, n.rest.count, 0, '']);
  return { name: `${(n.name.split(/[\\/]/).filter(Boolean).pop() || 'drive').replace(/[^\w.-]+/g, '_')}-sizes.csv`, text: rows.map(r => r.map(esc).join(',')).join('\r\n') };
}

// ---- snapshots: folder sizes (two levels deep) saved after each scan, to show growth over time
function snapDir(userData) { return path.join(userData, 'scans'); }
function snapFile(userData, root) { return path.join(snapDir(userData), crypto.createHash('sha1').update(root.toLowerCase()).digest('hex').slice(0, 16) + '.json'); }
function saveSnapshot(userData) {
  if (!scan || scan.cancelled) return;
  const folders = {};
  for (const a of scan.tree.c) { folders[a.n] = a.s; for (const b of a.c.slice(0, 40)) folders[path.join(a.n, b.n)] = b.s; }
  const file = snapFile(userData, scan.root);
  let list = []; try { list = JSON.parse(fs.readFileSync(file, 'utf8')); } catch { /* first */ }
  list.unshift({ at: scan.at, total: scan.tree.s, folders });
  fs.mkdirSync(snapDir(userData), { recursive: true });
  fs.writeFileSync(file, JSON.stringify(list.slice(0, 12)));
}
function history(userData) {
  if (!scan) return { snapshots: [], growth: [] };
  let list = []; try { list = JSON.parse(fs.readFileSync(snapFile(userData, scan.root), 'utf8')); } catch { /* none */ }
  const [cur, prev] = list;
  const growth = cur && prev ? Object.keys({ ...cur.folders, ...prev.folders }).map(k => ({ path: k, before: prev.folders[k] || 0, now: cur.folders[k] || 0, change: (cur.folders[k] || 0) - (prev.folders[k] || 0) }))
    .filter(x => Math.abs(x.change) >= 50 * 1024 * 1024).sort((a, b) => Math.abs(b.change) - Math.abs(a.change)).slice(0, 25) : [];
  return { snapshots: list.map(s => ({ at: s.at, total: s.total })), since: prev ? prev.at : null, growth };
}

module.exports = { start, cancel, summary, node, top, search, known, forget, toCsv, saveSnapshot, history, has: () => !!scan, root: () => scan && scan.root };
