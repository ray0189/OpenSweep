'use strict';
// Space analyzer scanner (runs in a worker thread so the app never freezes).
// Builds a folder tree with sizes, keeps big files individually, and collects
// file-type, file-age, largest-file and "insight" statistics in one pass.
const { parentPort, workerData } = require('worker_threads');
const fs = require('fs');
const path = require('path');

const root = workerData.root;
const KEEP_MIN = 1024 * 1024;   // files >= 1 MB are kept individually
const KEEP_TOP = 20;            // plus the 20 largest of each folder
const LIMIT = 48;               // parallel file-system operations
let cancelled = false;
parentPort.on('message', m => { if (m === 'cancel') cancelled = true; });

// ---- tiny semaphore
let active = 0; const waiting = [];
const acquire = () => (active < LIMIT ? (active++, Promise.resolve()) : new Promise(r => waiting.push(r)));
const release = () => { const n = waiting.shift(); if (n) n(); else active--; };

const stats = { files: 0, dirs: 0, bytes: 0, denied: 0, locked: 0, current: root };
const ext = new Map();
const AGE = [['< 1 month', 30], ['1–6 months', 182], ['6–12 months', 365], ['1–2 years', 730], ['2–5 years', 1826], ['> 5 years', Infinity]];
const age = AGE.map(([label]) => ({ label, size: 0, count: 0 }));
let top = [];
const empty = [];
const now = Date.now();

// Folders worth flagging (only the top-most match of each kind is recorded).
const SPECIAL = {
  node_modules: ['dev', 'node_modules folders', 'JavaScript dependencies. Re-created by "npm install" when needed.'],
  '.npm': ['cache', 'npm cache', 'Safe to delete; npm downloads packages again when needed.'],
  '.gradle': ['cache', 'Gradle cache', 'Build cache; re-downloaded when needed.'],
  '.nuget': ['cache', 'NuGet cache', 'Package cache; re-downloaded when needed.'],
  __pycache__: ['dev', 'Python caches', 'Compiled Python files; re-created automatically.'],
  '.cache': ['cache', 'App caches (.cache)', 'Caches apps rebuild automatically.'],
  cache: ['cache', 'App cache folders', 'Caches apps rebuild automatically. Close the app first.'],
  'code cache': ['cache', 'Browser/app code caches', 'Rebuilt automatically.'],
  gpucache: ['cache', 'GPU shader caches', 'Rebuilt automatically.'],
  temp: ['temp', 'Temp folders', 'Temporary files left behind by apps and installers.'],
  tmp: ['temp', 'Temp folders', 'Temporary files left behind by apps and installers.'],
  'windows.old': ['system', 'Previous Windows installation', 'Only needed to roll back a Windows upgrade.'],
  '$recycle.bin': ['system', 'Recycle Bin', 'Files you already deleted.'],
  crashdumps: ['temp', 'Crash dumps', 'Memory dumps from app crashes.'],
};
const insights = new Map(); // key -> { kind, title, why, size, count, paths: [] }
const bigVideos = [], isos = [], oldInstallers = [];
const CAT = {
  video: ['.mp4', '.mkv', '.mov', '.avi', '.wmv', '.webm', '.m4v', '.flv', '.ts', '.mts'],
  image: ['.jpg', '.jpeg', '.png', '.gif', '.bmp', '.webp', '.heic', '.tif', '.tiff', '.raw', '.nef', '.cr2', '.arw', '.dng', '.psd', '.svg'],
  audio: ['.mp3', '.wav', '.flac', '.aac', '.m4a', '.ogg', '.wma', '.opus'],
  archive: ['.zip', '.rar', '.7z', '.tar', '.gz', '.bz2', '.xz', '.cab', '.zst'],
  diskimage: ['.iso', '.img', '.vhd', '.vhdx', '.vmdk', '.vdi', '.qcow2', '.wim', '.esd'],
  document: ['.pdf', '.doc', '.docx', '.xls', '.xlsx', '.ppt', '.pptx', '.txt', '.odt', '.rtf', '.md', '.csv', '.epub'],
  code: ['.js', '.ts', '.py', '.cs', '.cpp', '.c', '.h', '.java', '.go', '.rs', '.json', '.xml', '.html', '.css', '.map', '.pdb', '.obj', '.lib', '.pyc'],
  program: ['.exe', '.dll', '.msi', '.msix', '.appx', '.sys', '.so', '.jar', '.node'],
  system: ['.log', '.etl', '.dmp', '.tmp', '.dat', '.db', '.sqlite', '.ldb', '.bak', '.cache'],
};
const catOf = (e) => { for (const [k, v] of Object.entries(CAT)) if (v.includes(e)) return k; return 'other'; };

function addInsight(key, title, kind, why, size, p) {
  const x = insights.get(key) || { key, title, kind, why, size: 0, count: 0, paths: [] };
  x.size += size; x.count++; if (x.paths.length < 200) x.paths.push({ path: p, size });
  insights.set(key, x);
}

async function scanDir(full, node, depth, insideSpecial) {
  if (cancelled) return;
  stats.current = full;
  let ents;
  await acquire();
  try { ents = await fs.promises.readdir(full, { withFileTypes: true }); }
  catch { node.denied = 1; stats.denied++; return; }
  finally { release(); }
  const files = [], dirs = [];
  for (const e of ents) {
    if (e.isSymbolicLink()) continue; // junctions/symlinks: skip to avoid double counting
    if (e.isDirectory()) dirs.push(e.name); else if (e.isFile()) files.push(e.name);
  }
  const lowerFull = full.toLowerCase();
  const inDownloads = /[\\/]downloads([\\/]|$)/.test(lowerFull);
  const kept = [];
  let restN = 0, restS = 0;
  await Promise.all(files.map(async (name) => {
    let st;
    await acquire();
    try { st = await fs.promises.lstat(path.join(full, name)); } catch { stats.locked++; return; } finally { release(); }
    const size = st.size, m = st.mtimeMs;
    stats.files++; stats.bytes += size;
    node.s += size; node.f++; if (m > node.m) node.m = m;
    const e = path.extname(name).toLowerCase() || '(none)';
    const x = ext.get(e) || [0, 0]; x[0] += size; x[1]++; ext.set(e, x);
    const days = (now - m) / 86400e3;
    const ai = AGE.findIndex(([, d]) => days < d); age[ai].size += size; age[ai].count++;
    const fp = path.join(full, name);
    if (size >= KEEP_MIN) {
      top.push([fp, size, m]); if (top.length > 2000) { top.sort((a, b) => b[1] - a[1]); top = top.slice(0, 500); }
      const c = catOf(e);
      if (c === 'video' && size >= 1024 ** 3) bigVideos.push({ path: fp, size });
      if (c === 'diskimage' && size >= 100 * 1024 ** 2) isos.push({ path: fp, size });
      if (inDownloads && ['.exe', '.msi', '.msix', '.iso', '.zip'].includes(e) && days > 30) oldInstallers.push({ path: fp, size });
    }
    kept.push([name, size, m]);
  }));
  kept.sort((a, b) => b[1] - a[1]);
  node.fl = kept.filter((f, i) => i < KEEP_TOP || f[1] >= KEEP_MIN);
  for (const f of kept.slice(node.fl.length)) { restN++; restS += f[1]; }
  node.rest = restN ? [restN, restS] : null;
  node.c = dirs.map(n => ({ n, s: 0, f: 0, d: 0, m: 0, c: [], fl: [], rest: null }));
  stats.dirs += dirs.length;
  await Promise.all(node.c.map(ch => {
    const key = ch.n.toLowerCase();
    const special = !insideSpecial && SPECIAL[key] && depth > 0 ? key : null;
    return scanDir(path.join(full, ch.n), ch, depth + 1, insideSpecial || !!special).then(() => {
      if (special) { const [kind, title, why] = SPECIAL[special]; addInsight(special === 'tmp' ? 'temp' : special, title, kind, why, ch.s, path.join(full, ch.n)); }
    });
  }));
  for (const ch of node.c) { node.s += ch.s; node.f += ch.f; node.d += 1 + ch.d; if (ch.m > node.m) node.m = ch.m; }
  node.c.sort((a, b) => b.s - a.s);
  if (!node.f && !node.c.length && depth > 0 && empty.length < 5000) empty.push(full);
}

(async () => {
  const t0 = Date.now();
  const tree = { n: root, s: 0, f: 0, d: 0, m: 0, c: [], fl: [], rest: null };
  const timer = setInterval(() => parentPort.postMessage({ type: 'progress', ...stats, ms: Date.now() - t0 }), 250);
  try { await scanDir(root, tree, 0, false); } catch (e) { parentPort.postMessage({ type: 'error', message: e.message }); }
  clearInterval(timer);
  top.sort((a, b) => b[1] - a[1]);
  const sum = (arr) => arr.reduce((a, x) => a + x.size, 0);
  if (bigVideos.length) insights.set('videos', { key: 'videos', title: 'Videos over 1 GB', kind: 'media', why: 'Large videos you may have already watched or backed up.', size: sum(bigVideos), count: bigVideos.length, paths: bigVideos.sort((a, b) => b.size - a.size).slice(0, 200) });
  if (isos.length) insights.set('images', { key: 'images', title: 'Disk images (ISO, VHD…)', kind: 'media', why: 'Installers and virtual disks are often kept long after use.', size: sum(isos), count: isos.length, paths: isos.sort((a, b) => b.size - a.size).slice(0, 200) });
  if (oldInstallers.length) insights.set('installers', { key: 'installers', title: 'Old installers in Downloads', kind: 'temp', why: 'Setup files older than 30 days. The apps are already installed.', size: sum(oldInstallers), count: oldInstallers.length, paths: oldInstallers.sort((a, b) => b.size - a.size).slice(0, 200) });
  if (empty.length) insights.set('empty', { key: 'empty', title: 'Empty folders', kind: 'tidy', why: 'Folders with nothing in them.', size: 0, count: empty.length, paths: empty.slice(0, 200).map(p => ({ path: p, size: 0 })) });
  const types = [...ext.entries()].map(([e, [size, count]]) => ({ ext: e, cat: catOf(e), size, count })).sort((a, b) => b.size - a.size);
  parentPort.postMessage({ type: 'done', cancelled, tree, types, age, top: top.slice(0, 500), insights: [...insights.values()].sort((a, b) => b.size - a.size), stats: { ...stats, ms: Date.now() - t0 } });
})();
