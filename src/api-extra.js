'use strict';
// IPC methods for: Clean everything, Uninstaller, Space analyzer, Reliability, live Processes.
// Same rule as main.js: the renderer only sends IDs; every path/command comes from data
// this process produced itself (caches below).
const fs = require('fs');
const os = require('os');
const path = require('path');
const C = require('./backend/catalog');
const disk = require('./backend/disk');

module.exports = function extraApi(ctx) {
  const { backend, DEMO, app, shell, dialog, send, logHistory, fmt, getWin } = ctx;
  const cache = { programs: new Map(), leftovers: new Map(), clean: new Map(), exts: new Map(), features: new Set(), winget: new Map(), procs: new Map() };
  const str = (x, max = 4096) => { if (typeof x !== 'string' || x.length > max) throw new Error('Invalid argument'); return x; };
  const trash = async (p) => { if (DEMO) return; await shell.trashItem(p); };

  // ------------------------------------------------------------ Clean everything
  async function cleanAllScan() {
    const [cats, drivers, winold] = await Promise.all([
      backend.cleanupScan({}), backend.oldDrivers().catch(() => []), backend.windowsOldSize().catch(() => ({ exists: false, size: 0 })),
    ]);
    const items = [];
    for (const c of C.CLEANUP) {
      const r = (cats || []).find(x => x.id === c.id);
      if (!r || !r.size) continue;
      items.push({ id: `cat:${c.id}`, group: c.group, name: c.name, desc: c.desc, size: r.size, count: r.files, admin: !!c.admin, safe: !!c.rec, why: c.rec ? null : 'Not needed by most people, but check before cleaning.' });
    }
    if (drivers.length) items.push({ id: 'drivers', group: 'Windows', name: 'Old driver packages', desc: `${drivers.length} older copies of drivers you have newer versions of. Drivers still in use are skipped automatically.`, size: drivers.reduce((a, d) => a + d.size, 0), count: drivers.length, admin: true, safe: true, detail: drivers });
    if (winold.exists) items.push({ id: 'winold', group: 'Windows', name: 'Previous Windows installation (Windows.old)', desc: 'Only needed if you want to go back to your previous Windows version.', size: winold.size, count: 1, admin: true, safe: false, why: 'After removing it you can no longer roll back the last Windows upgrade.' });
    const installers = backend.oldInstallers(app.getPath('downloads'));
    if (installers.length) items.push({ id: 'installers', group: 'Your files', name: 'Old installers in Downloads', desc: 'Setup files (.exe, .msi, .iso) older than 30 days. Moved to the Recycle Bin.', size: installers.reduce((a, f) => a + f.size, 0), count: installers.length, safe: false, why: 'These are your downloads — check the list first.', detail: installers });
    const empties = backend.emptyFolders(os.homedir());
    if (empties.length) items.push({ id: 'empty', group: 'Your files', name: 'Empty folders', desc: 'Folders with nothing in them in Desktop, Documents, Downloads, Pictures, Music and Videos.', size: 0, count: empties.length, safe: false, why: 'Some apps expect their empty folders to exist.', detail: empties.map(p => ({ path: p })) });
    items.push({ id: 'components', group: 'Windows', name: 'Windows component store cleanup', desc: 'Removes superseded Windows Update components (DISM). Can take 10+ minutes. Size is only known afterwards.', size: 0, count: 0, admin: true, safe: false, why: 'Slow, and updates installed before it can no longer be uninstalled.' });
    cache.clean = new Map(items.map(i => [i.id, i]));
    return { items, at: new Date().toISOString() };
  }
  async function cleanAllRun({ ids }) {
    const pick = (ids || []).map(id => cache.clean.get(id)).filter(Boolean);
    const report = [];
    const step = (name, freed, note) => { report.push({ name, freed, note }); send('clean-progress', { done: report.length, total: pick.length, name }); };
    const catIds = pick.filter(i => i.id.startsWith('cat:')).map(i => i.id.slice(4));
    if (catIds.length) {
      const r = await backend.cleanupRun({ ids: catIds, elevate: true });
      for (const x of r || []) { const c = C.CLEANUP.find(k => k.id === x.id); step(c ? c.name : x.id, x.freed, x.skipped ? `${x.skipped} in-use files skipped` : null); }
    }
    for (const i of pick.filter(i => !i.id.startsWith('cat:'))) {
      try {
        if (i.id === 'drivers') {
          const r = await backend.removeDrivers(i.detail.map(d => d.inf));
          const ok = new Set(r.filter(x => x.ok).map(x => x.inf));
          step(i.name, i.detail.filter(d => ok.has(d.inf)).reduce((a, d) => a + d.size, 0), `${ok.size} of ${i.detail.length} removed${ok.size < i.detail.length ? ' (the rest are in use)' : ''}`);
        } else if (i.id === 'winold') { const r = await backend.removeWindowsOld(); step(i.name, r.ok ? i.size : 0, r.ok ? null : 'Some files could not be removed'); }
        else if (i.id === 'installers') { let freed = 0; for (const f of i.detail) { try { await trash(f.path); freed += f.size; } catch { /* skip */ } } step(i.name, freed, 'Moved to the Recycle Bin'); }
        else if (i.id === 'empty') { let n = 0; for (const f of i.detail) { try { if (!DEMO && fs.readdirSync(f.path).length === 0) { fs.rmdirSync(f.path); n++; } else if (DEMO) n++; } catch { /* skip */ } } step(i.name, 0, `${n} folders removed`); }
        else if (i.id === 'components') { const r = await backend.componentCleanup(); step(i.name, 0, r.ok ? 'Done' : 'DISM reported a problem'); }
      } catch (e) { step(i.name, 0, `Failed: ${e.message}`); }
    }
    const freed = report.reduce((a, x) => a + (x.freed || 0), 0);
    logHistory({ kind: 'cleanup', summary: `Clean everything freed ${fmt(freed)}` });
    return { freed, report };
  }

  // ------------------------------------------------------------ Uninstaller
  async function programs() {
    const r = await backend.programs();
    cache.programs = new Map((r || []).map(a => [a.id, a]));
    const bad = C.BAD_APPS || [];
    return (r || []).map(a => {
      const flag = bad.find(b => new RegExp(b.match, 'i').test(`${a.name} ${a.publisher || ''}`));
      return { ...a, uninstall: undefined, quiet: undefined, canSilent: a.kind === 'store' || !!backend.silentCommand(a), flag: flag ? { level: flag.level, why: flag.why } : null };
    });
  }
  const getApp = (id) => { const a = cache.programs.get(id); if (!a) throw new Error('Unknown app — refresh the list'); return a; };
  async function appIcon({ id }) {
    const a = cache.programs.get(id);
    if (!a || DEMO) return null;
    let p = a.icon ? String(a.icon).split(',')[0].replace(/"/g, '').trim() : null;
    if (!p || !/\.(exe|ico|dll)$/i.test(p)) {
      if (a.kind === 'store' && a.location) { try { const m = fs.readFileSync(path.join(a.location, 'AppxManifest.xml'), 'utf8').match(/<Logo>([^<]+)<\/Logo>/); if (m) { const base = path.join(a.location, m[1]); const dir = path.dirname(base), stem = path.basename(base, path.extname(base)); const f = fs.readdirSync(dir).find(x => x.startsWith(stem)); if (f) return 'data:image/png;base64,' + fs.readFileSync(path.join(dir, f)).toString('base64'); } } catch { /* none */ } }
      return null;
    }
    try { const img = await app.getFileIcon(p, { size: 'normal' }); return img.isEmpty() ? null : img.toDataURL(); } catch { return null; }
  }
  async function uninstall({ id, silent }) {
    const a = getApp(id);
    const r = await backend.runUninstall(a, { silent: !!silent });
    logHistory({ kind: 'uninstall', summary: `${silent ? 'Uninstalled' : 'Uninstaller started'}: ${a.name}` });
    return r;
  }
  async function uninstallBatch({ ids }) {
    const out = [];
    const apps = (ids || []).map(id => cache.programs.get(id)).filter(Boolean);
    for (const [i, a] of apps.entries()) {
      send('batch-progress', { index: i, total: apps.length, name: a.name });
      try { const r = await backend.runUninstall(a, { silent: true }); out.push({ id: a.id, name: a.name, ok: r.gone !== false, error: r.gone === false ? 'Still installed — try a normal uninstall' : null }); }
      catch (e) { out.push({ id: a.id, name: a.name, ok: false, error: e.message }); }
    }
    logHistory({ kind: 'uninstall', summary: `Batch uninstall: ${out.filter(x => x.ok).length} of ${apps.length} removed` });
    return out;
  }
  const stillInstalled = ({ id }) => backend.stillInstalled(getApp(id));
  const keepLeftovers = (items) => { items.forEach(i => cache.leftovers.set(i.id, i)); return items; };
  const leftoverScan = async ({ id }) => keepLeftovers(await backend.leftoverScan(getApp(id)));
  async function forcedScan({ name, folder }) {
    const n = str(name || '', 120).trim();
    const f = folder ? str(folder, 1024) : null;
    if (n.length < 3 && !f) throw new Error('Enter the app name (at least 3 letters) or pick its folder');
    if (f && (!path.isAbsolute(f) || f.split(/[\\/]/).filter(Boolean).length < 2)) throw new Error('Pick the app\'s own folder, not a drive or system folder');
    return keepLeftovers(await backend.leftoverScan({ name: n || path.basename(f), publisher: '', location: f, regKey: null }));
  }
  const orphanScan = async () => keepLeftovers(await backend.orphanScan([...cache.programs.values()]));
  async function removeLeftovers({ ids, label }) {
    const items = (ids || []).map(id => cache.leftovers.get(id)).filter(Boolean);
    const results = [];
    for (const i of items.filter(i => i.type === 'dir' || i.type === 'file')) {
      try { await trash(i.path); results.push({ id: i.id, ok: true }); } catch (e) { results.push({ id: i.id, ok: false, error: e.message }); }
    }
    const other = items.filter(i => i.type !== 'dir' && i.type !== 'file');
    let backup = null;
    if (other.length) { const r = await backend.removeLeftovers(other, label); backup = r.backup; results.push(...r.results); }
    const ok = results.filter(r => r.ok).length;
    const freed = items.filter(i => results.find(r => r.id === i.id && r.ok)).reduce((a, i) => a + (i.size || 0), 0);
    logHistory({ kind: 'uninstall', summary: `Removed ${ok} leftover(s)${label ? ` of ${String(label).slice(0, 60)}` : ''} · ${fmt(freed)}`, ...(backup ? { undo: { fn: 'restoreBackup', args: { dir: backup } } } : {}) });
    return { results, backup, freed };
  }
  async function restoreBackup({ dir }) {
    const root = backend.BACKUP_ROOT ? backend.BACKUP_ROOT() : '';
    if (!DEMO && (!dir || !path.resolve(dir).startsWith(path.resolve(root)))) throw new Error('Invalid backup');
    return backend.restoreBackup(dir);
  }
  async function browserExtensions() { const r = await backend.browserExtensions(); cache.exts = new Map(r.map(e => [e.id, e])); return r.map(e => ({ ...e, path: undefined, policyKey: undefined })); }
  async function removeExtension({ id }) {
    const e = cache.exts.get(id);
    if (!e) throw new Error('Unknown extension — refresh the list');
    if (e.forced) await backend.removeForcedExtension(e);
    if (e.path) await trash(e.path);
    logHistory({ kind: 'uninstall', summary: `Removed ${e.browser} extension: ${e.name}` });
    return { ok: true, note: `Restart ${e.browser} to finish. If you use browser sync, also remove it from another device or it may come back.` };
  }
  async function optionalFeatures() { const r = await backend.optionalFeatures(); cache.features = new Set(r.map(f => f.name)); return r; }
  async function setFeature({ name, enable }) {
    if (!cache.features.has(name)) throw new Error('Unknown feature');
    const r = await backend.setFeature({ name, enable: !!enable });
    logHistory({ kind: 'features', summary: `${enable ? 'Turned on' : 'Turned off'} Windows feature: ${name}`, undo: { fn: 'setFeature', args: { name, enable: !enable } } });
    return r;
  }
  const trackStart = ({ label }) => backend.trackStart(str(label || 'Install', 80));
  const trackFinish = ({ id }) => backend.trackFinish(str(id, 64));
  const tracks = () => backend.tracks();
  function trackItems({ id }) { const t = backend.tracks().find(x => x.id === id); if (!t || !t.diff) throw new Error('Unknown install log'); return keepLeftovers(backend.trackItems(t)); }
  async function hunt() {
    const w = getWin(); if (w && !DEMO) w.minimize();
    let r;
    try { r = await backend.hunt({ seconds: 20 }); } finally { if (w) { w.restore(); w.focus(); } }
    if (!r || r.timeout) return { timeout: true };
    if (!cache.programs.size) await programs().catch(() => {});
    const lp = String(r.path || '').toLowerCase();
    const appMatch = [...cache.programs.values()].find(a => a.location && lp.startsWith(String(a.location).toLowerCase().replace(/[\\/]+$/, '') + '\\'))
      || [...cache.programs.values()].find(a => r.company && a.publisher && a.publisher.toLowerCase().includes(String(r.company).toLowerCase().split(' ')[0]) && lp.includes(cleanFirst(a.name)));
    return { ...r, app: appMatch ? { id: appMatch.id, name: appMatch.name } : null };
  }
  const cleanFirst = (n) => String(n || '').toLowerCase().split(/\s+/)[0];
  async function wingetUpgrades() { const r = await backend.wingetUpgrades(); cache.winget = new Map(r.map(x => [x.id, x])); return r; }
  async function wingetUpgrade({ id }) {
    if (!cache.winget.has(id)) throw new Error('Unknown update — refresh the list');
    const r = await backend.wingetUpgrade(id);
    logHistory({ kind: 'update', summary: `${r.ok ? 'Updated' : 'Update failed'}: ${cache.winget.get(id).name}` });
    return r;
  }
  async function chooseFolder({ title } = {}) {
    const r = await dialog.showOpenDialog(getWin(), { title: title || 'Choose a folder', properties: ['openDirectory'] });
    return r.canceled ? null : r.filePaths[0];
  }

  // ------------------------------------------------------------ Space analyzer
  function diskDrives() {
    if (process.platform !== 'win32') return [{ root: os.homedir(), label: 'Home folder' }, { root: '/', label: 'Whole disk' }];
    const out = [];
    for (const l of 'CDEFGHIJKLMNOPQRSTUVWXYZ') { const r = `${l}:\\`; try { fs.accessSync(r); out.push({ root: r, label: `${l}: drive` }); } catch { /* none */ } }
    out.push({ root: os.homedir(), label: 'My user folder' });
    return out;
  }
  async function diskScan({ root }) {
    const r = str(root, 1024);
    if (!path.isAbsolute(r) || !fs.existsSync(r)) throw new Error('Folder not found');
    let last = 0;
    const s = await disk.start(r, p => { const now = Date.now(); if (now - last > 200) { last = now; send('disk-progress', p); } });
    try { disk.saveSnapshot(app.getPath('userData')); } catch { /* non-fatal */ }
    return s;
  }
  const checkPaths = (paths) => (paths || []).filter(p => typeof p === 'string' && disk.known(p));
  async function diskTrash({ paths }) {
    const res = [];
    for (const p of checkPaths(paths)) { try { await trash(p); disk.forget(p); res.push({ path: p, ok: true }); } catch (e) { res.push({ path: p, ok: false, error: e.message }); } }
    logHistory({ kind: 'files', summary: `Space analyzer: moved ${res.filter(r => r.ok).length} item(s) to the Recycle Bin` });
    return res;
  }
  async function diskMove({ paths }) {
    const ok = checkPaths(paths);
    if (!ok.length) return [];
    const dest = await chooseFolder({ title: 'Move to which folder?' });
    if (!dest) return null;
    const res = [];
    for (const p of ok) {
      const target = path.join(dest, path.basename(p));
      try {
        if (fs.existsSync(target)) throw new Error('Something with that name already exists there');
        if (!DEMO) { try { fs.renameSync(p, target); } catch (e) { if (e.code !== 'EXDEV') throw e; await fs.promises.cp(p, target, { recursive: true, errorOnExist: true }); await fs.promises.rm(p, { recursive: true, force: true }); } }
        disk.forget(p); res.push({ path: p, ok: true });
      } catch (e) { res.push({ path: p, ok: false, error: e.message }); }
    }
    logHistory({ kind: 'files', summary: `Moved ${res.filter(r => r.ok).length} item(s) to ${dest}` });
    return res;
  }
  async function diskCompress({ paths }) {
    const ok = checkPaths(paths);
    if (DEMO || !ok.length) return ok.map(p => ({ path: p, ok: true }));
    const r = await backend.compressPaths(ok);
    logHistory({ kind: 'files', summary: `NTFS-compressed ${ok.length} item(s)` });
    return r;
  }
  async function diskExport({ segs }) {
    const { name, text } = disk.toCsv(segs || []);
    const r = await dialog.showSaveDialog(getWin(), { defaultPath: path.join(app.getPath('documents'), name), filters: [{ name: 'CSV', extensions: ['csv'] }] });
    if (r.canceled) return null;
    fs.writeFileSync(r.filePath, '﻿' + text);
    return { path: r.filePath };
  }
  function diskReveal({ path: p }) { if (disk.known(p) && !DEMO) shell.showItemInFolder(p); return { ok: true }; }

  // ------------------------------------------------------------ live processes
  let stopProcs = null, procTimer = null;
  function procStart() {
    if (stopProcs) return { ok: true };
    const onData = (d) => { for (const p of d.procs || []) cache.procs.set(p.pid, p); send('procs', d); };
    stopProcs = backend.startProcessStream(onData, () => { stopProcs = null; });
    clearTimeout(procTimer); procTimer = setTimeout(procStop, 10 * 60e3); // safety: never run forever
    return { ok: true };
  }
  function procStop() { if (stopProcs) { stopProcs(); stopProcs = null; } clearTimeout(procTimer); return { ok: true }; }
  function procKeepAlive() { if (stopProcs) { clearTimeout(procTimer); procTimer = setTimeout(procStop, 10 * 60e3); } return { ok: true }; }
  async function setPriority({ pid, priority }) {
    const p = cache.procs.get(pid);
    if (!p) throw new Error('Process has ended');
    if (C.PROTECTED_PROCESSES.includes(String(p.name).toLowerCase())) throw new Error(`${p.name} is a critical Windows process.`);
    return backend.setPriority({ pid, priority: str(priority, 20) });
  }
  function processLocation({ pid }) { const p = cache.procs.get(pid); if (p && p.path && !DEMO) shell.showItemInFolder(p.path); return { ok: true }; }
  function processSearch({ pid }) { const p = cache.procs.get(pid); if (p) shell.openExternal(`https://duckduckgo.com/?q=${encodeURIComponent(`${p.name}.exe ${p.company || ''} process`)}`); return { ok: true }; }

  return {
    cleanAllScan, cleanAllRun,
    programs, appIcon, uninstall, uninstallBatch, stillInstalled, leftoverScan, forcedScan, orphanScan, removeLeftovers, restoreBackup,
    browserExtensions, removeExtension, optionalFeatures, setFeature, trackStart, trackFinish, tracks, trackItems, hunt, wingetUpgrades, wingetUpgrade, chooseFolder,
    diskDrives, diskScan, diskCancel: () => { disk.cancel(); return { ok: true }; }, diskSummary: () => disk.summary(), diskNode: ({ segs }) => disk.node((segs || []).map(s => str(s, 512))),
    diskTop: () => disk.top(300), diskSearch: (q) => disk.search(q || {}), diskHistory: () => disk.history(app.getPath('userData')), diskTrash, diskMove, diskCompress, diskExport, diskReveal,
    reliability: ({ days }) => backend.reliability({ days: Math.min(90, Math.max(7, Number(days) || 30)) }),
    procStart, procStop, procKeepAlive, setPriority, processLocation, processSearch,
    _stopAll: () => { procStop(); disk.cancel(); },
  };
};
