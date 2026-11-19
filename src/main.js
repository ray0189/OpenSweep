'use strict';
const { app, BrowserWindow, ipcMain, shell, Tray, Menu, nativeImage, screen, Notification, powerMonitor, dialog } = require('electron');
const path = require('path');
const fs = require('fs');
const os = require('os');
const net = require('net');
const C = require('./backend/catalog');

const DEMO = process.platform !== 'win32' || process.argv.includes('--demo');
const backend = DEMO ? require('./backend/mock') : require('./backend/windows');

let win = null, widget = null, tray = null, quitting = false;

// ------------------------------------------------------------------ settings
const settingsFile = () => path.join(app.getPath('userData'), 'settings.json');
const DEFAULTS = { theme: 'system', minimizeToTray: true, showWidget: false, launchAtLogin: false, confirmActions: true, scanRoot: '', shortcuts: [], history: [],
  autoUpdate: true, stabilityMonitor: true, stabilityNotify: true, stabilitySeen: null, settingsGuard: true, guardTweaks: [], autoClean: 'off' };
let settings = { ...DEFAULTS };
function loadSettings() { try { settings = { ...DEFAULTS, ...JSON.parse(fs.readFileSync(settingsFile(), 'utf8')) }; } catch { settings = { ...DEFAULTS }; } }
function saveSettings() { try { fs.mkdirSync(path.dirname(settingsFile()), { recursive: true }); fs.writeFileSync(settingsFile(), JSON.stringify(settings, null, 2)); } catch { /* non-fatal */ } }
function logHistory(entry) { settings.history = [{ at: new Date().toISOString(), ...entry }, ...settings.history].slice(0, 300); saveSettings(); }

// ------------------------------------------------------------------ live stats (pure Node, works everywhere)
let lastCpu = os.cpus();
function cpuPercent() {
  const now = os.cpus();
  let idle = 0, total = 0;
  now.forEach((c, i) => {
    const p = lastCpu[i] ? lastCpu[i].times : { user: 0, nice: 0, sys: 0, idle: 0, irq: 0 };
    const t = c.times;
    const dt = (t.user - p.user) + (t.nice - p.nice) + (t.sys - p.sys) + (t.idle - p.idle) + (t.irq - p.irq);
    idle += t.idle - p.idle; total += dt;
  });
  lastCpu = now;
  return total > 0 ? Math.max(0, Math.min(100, Math.round(100 * (1 - idle / total)))) : 0;
}
function stats() {
  const total = os.totalmem(), free = os.freemem();
  return { cpu: cpuPercent(), memUsed: total - free, memTotal: total, memPct: Math.round(100 * (total - free) / total), uptime: os.uptime() };
}

// ------------------------------------------------------------------ caches that make IPC input trustworthy
// The renderer only ever passes IDs; commands/paths are looked up from data the main process produced.
const cache = { apps: new Map(), startup: new Map(), deletable: new Set() };

// ------------------------------------------------------------------ health check
async function health() {
  const [sys, junk, startup, sec, tweaks] = await Promise.allSettled([backend.systemInfo(), backend.cleanupScan({ ids: C.CLEANUP.filter(c => c.rec).map(c => c.id) }), backend.startupItems(), backend.security(), backend.tweakStates()]);
  const v = r => (r.status === 'fulfilled' ? r.value : null);
  const issues = [];
  const add = (sev, title, detail, action) => issues.push({ sev, title, detail, action });
  const s = v(sys), j = v(junk), st = v(startup), se = v(sec), tw = v(tweaks);
  const m = stats();

  if (s) {
    for (const d of s.disks || []) {
      const pct = d.total ? d.free / d.total : 1;
      if (pct < 0.1) add('high', `Low disk space on ${d.name}`, `Only ${fmt(d.free)} free of ${fmt(d.total)}.`, { go: 'storage' });
      else if (pct < 0.2) add('medium', `Disk ${d.name} is getting full`, `${fmt(d.free)} free of ${fmt(d.total)}.`, { go: 'storage' });
    }
    const days = (Date.now() - new Date(s.lastBoot).getTime()) / 86400e3;
    if (days > 7) add('low', `Not restarted in ${Math.floor(days)} days`, 'A restart applies updates and clears memory leaks.', null);
    for (const p of s.physicalDisks || []) if (p.health && p.health !== 'Healthy') add('high', `Drive health warning: ${p.name}`, `Windows reports "${p.health}". Back up your files.`, null);
  }
  if (j) {
    const total = j.reduce((a, x) => a + (x.size || 0), 0);
    if (total > 300 * 1024 ** 2) add(total > 2 * 1024 ** 3 ? 'medium' : 'low', `${fmt(total)} of junk files`, 'Temp files, caches and leftovers you can safely remove.', { fix: 'clean', label: 'Clean now' });
  }
  if (st) {
    const on = st.filter(x => x.enabled).length;
    if (on > 6) add('medium', `${on} apps start with Windows`, 'Too many startup apps slow down boot.', { go: 'startup' });
  }
  if (se) {
    const d = se.defender;
    if (d && !d.realtime) add('high', 'Real-time protection is off', 'Microsoft Defender is not actively protecting this PC.', { uri: 'windowsdefender:' });
    if (d && d.signatureAge > 3) add('medium', `Virus definitions are ${d.signatureAge} days old`, '', { fix: 'defender-update', label: 'Update' });
    if (d && d.quickScanAge > 14) add('low', `No virus scan for ${d.quickScanAge} days`, '', { fix: 'quick-scan', label: 'Quick scan' });
    if ((se.firewall || []).some(f => !f.enabled)) add('high', 'Firewall is off for some networks', (se.firewall || []).filter(f => !f.enabled).map(f => f.name).join(', '), { fix: 'firewall-on', label: 'Turn on' });
    if (se.lastUpdate && (Date.now() - new Date(se.lastUpdate).getTime()) / 86400e3 > 30) add('medium', 'Windows updates look out of date', `Last update installed ${new Date(se.lastUpdate).toLocaleDateString()}.`, { uri: 'ms-settings:windowsupdate' });
    if (se.smb1) add('high', 'SMB1 is enabled', 'Outdated file-sharing protocol used by WannaCry.', { fix: 'smb1-off', label: 'Disable' });
  }
  if (tw) {
    for (const [cat, label] of [['ai', 'Microsoft AI / Copilot features are active'], ['popups', 'Windows ads & suggestions are on'], ['privacy', 'Telemetry & tracking settings are permissive']]) {
      const off = C.TWEAKS.filter(t => t.cat === cat && t.rec && !tw[t.id]).length;
      if (off) add(cat === 'privacy' ? 'low' : 'medium', label, `${off} recommended setting${off > 1 ? 's' : ''} can be changed.`, { fix: 'preset:' + cat, label: 'Fix' });
    }
    const annoy = C.ANNOYANCE_FIXES.filter(id => !tw[id]).length;
    if (annoy) add('medium', `${annoy} common Windows annoyances are active`, 'Slow folders, forced restarts, account nags, Sticky Keys pop-ups, Edge in Alt+Tab and more.', { fix: 'preset:annoy', label: 'Fix all' });
  }
  const stab = stability && Date.now() - new Date(stability.checkedAt).getTime() < 3 * 3600e3 ? stability : await runStability({ manual: true }).catch(() => null);
  if (stab) {
    const dev = (stab.devices || []).length;
    if (dev) add('high', `${dev} device${dev > 1 ? 's have' : ' has'} a driver problem`, (stab.devices || []).slice(0, 3).map(d => `${d.name}: ${d.problem}`).join(' · '), { go: 'reliability' });
    const bsod = (stab.events || []).filter(e => e.kind === 'Blue screen').length;
    if (bsod) add('high', `${bsod} blue screen${bsod > 1 ? 's' : ''} in the last ${stab.days} days`, 'Usually a driver or hardware problem.', { go: 'reliability' });
    const other = (stab.events || []).filter(e => e.kind !== 'Blue screen').length;
    if (other) add('medium', `${other} stability warning${other > 1 ? 's' : ''} in the last ${stab.days} days`, [...new Set((stab.events || []).filter(e => e.kind !== 'Blue screen').map(e => e.kind))].join(', '), { go: 'reliability' });
  }
  if (m.memPct > 85) add('medium', `Memory is ${m.memPct}% used`, '', { fix: 'boost', label: 'Boost' });

  const weight = { high: 12, medium: 5, low: 2 };
  const score = Math.max(5, 100 - issues.reduce((a, i) => a + weight[i.sev], 0));
  issues.sort((a, b) => weight[b.sev] - weight[a.sev]);
  return { score, issues, checkedAt: new Date().toISOString() };
}
// ------------------------------------------------------------------ events to the UI + desktop notifications
function send(event, data) { for (const w of [win, widget]) if (w && !w.isDestroyed()) w.webContents.send('event', event, data); }
function notify(title, body, onClick) {
  try { if (!Notification.isSupported()) return; const n = new Notification({ title, body, icon: appIcon() }); if (onClick) n.on('click', onClick); n.show(); } catch { /* ignore */ }
}

// ------------------------------------------------------------------ drivers & stability monitor (background, low priority)
let stability = null, stabilityRunning = null;
function runStability({ manual = false } = {}) {
  if (stabilityRunning) return stabilityRunning;
  stabilityRunning = (async () => {
    try {
      const r = await backend.stabilityCheck({ days: 7 });
      stability = r;
      const keys = [...(r.devices || []).map(d => `dev|${d.id}|${d.code}`), ...(r.events || []).map(e => `ev|${e.key}`)];
      const first = settings.stabilitySeen === null;
      const seen = new Set(settings.stabilitySeen || []);
      const fresh = keys.filter(k => !seen.has(k));
      settings.stabilitySeen = keys.slice(0, 500); saveSettings();
      if (!manual && !first && fresh.length && settings.stabilityNotify) {
        const nd = fresh.filter(k => k.startsWith('dev|')).length, ne = fresh.length - nd;
        notify('OpenSweep: stability check', [nd ? `${nd} device(s) have a driver problem` : '', ne ? `${ne} new crash/hardware event(s)` : ''].filter(Boolean).join(' · '), () => { showMain(); send('go', 'reliability'); });
      }
      send('stability', r);
      return r;
    } finally { stabilityRunning = null; }
  })();
  return stabilityRunning;
}
function scheduleStability() {
  if (DEMO) return;
  let last = 0;
  const tick = () => {
    if (!settings.stabilityMonitor) return;
    const every = (powerMonitor.isOnBatteryPower && powerMonitor.isOnBatteryPower() ? 3 : 1) * 3600e3;
    if (Date.now() - last < every - 60e3) return;
    last = Date.now();
    runStability().catch(() => {});
    runGuard().catch(() => {});
  };
  setTimeout(tick, 90e3);
  setInterval(tick, 15 * 60e3);
}

// ------------------------------------------------------------------ protected data folder
// %ProgramData%\OpenSweep holds scripts run as admin/SYSTEM, backups and install logs. By default
// ordinary users can add files there, so lock it to SYSTEM + Administrators (read-only for Users).
function hardenDataDir() {
  try {
    const { execFileSync } = require('child_process');
    const dir = path.join(process.env.ProgramData || 'C:\\ProgramData', 'OpenSweep');
    const run = path.join(dir, 'run');
    fs.mkdirSync(run, { recursive: true });
    execFileSync('icacls.exe', [dir, '/inheritance:r', '/grant:r', '*S-1-5-18:(OI)(CI)F', '*S-1-5-32-544:(OI)(CI)F', '*S-1-5-32-545:(OI)(CI)RX', '/T', '/C', '/Q'], { windowsHide: true, timeout: 30000 });
    execFileSync('icacls.exe', [dir, '/setowner', '*S-1-5-32-544', '/T', '/C', '/Q'], { windowsHide: true, timeout: 30000 });
    require('./backend/pscore').setScriptDir(run);
  } catch { /* not admin or icacls failed: keep using %TEMP% */ }
}

// ------------------------------------------------------------------ settings guard (keeps your tweaks applied)
let guardRunning = false;
async function runGuard({ manual = false } = {}) {
  if (DEMO || guardRunning || (!settings.settingsGuard && !manual)) return { restored: [] };
  const ids = (settings.guardTweaks || []).filter(id => C.TWEAKS.some(t => t.id === id));
  if (!ids.length) return { restored: [], checked: 0 };
  guardRunning = true;
  try {
    const states = await backend.tweakStates();
    const reverted = ids.filter(id => states[id] === false);
    if (!reverted.length) return { restored: [], checked: ids.length };
    const r = await backend.setTweaks(reverted.map(id => ({ id, on: true })));
    const failedIds = new Set((r.failed || []).map(f => f.id));
    const restored = reverted.filter(id => !failedIds.has(id)).map(id => C.TWEAKS.find(t => t.id === id).title);
    if (restored.length) {
      logHistory({ kind: 'guard', summary: `Settings guard: Windows undid ${restored.length} setting(s) — restored (${restored.slice(0, 3).join(', ')}${restored.length > 3 ? '…' : ''})` });
      notify('OpenSweep kept your settings', `Windows undid ${restored.length} of your settings. They have been put back.`, () => { showMain(); send('go', 'history'); });
    }
    return { restored, failed: r.failed || [], checked: ids.length };
  } finally { guardRunning = false; }
}

// ------------------------------------------------------------------ scheduled cleaning (--auto-clean)
async function autoClean() {
  const ids = C.CLEANUP.filter(c => c.rec).map(c => c.id);
  const r = await backend.cleanupRun({ ids, elevate: true });
  const freed = (r || []).reduce((a, x) => a + (x.freed || 0), 0);
  logHistory({ kind: 'cleanup', summary: `Scheduled clean freed ${fmt(freed)}` });
  notify('OpenSweep cleaned your PC', `Scheduled clean freed ${fmt(freed)}.`);
  return freed;
}

// ------------------------------------------------------------------ auto-update from GitHub Releases
const upd = { state: 'idle', version: null, progress: 0, error: null, checkedAt: null };
let updater = null;
function setUpd(patch) { Object.assign(upd, patch); send('update', upd); }
function setupUpdater() {
  if (DEMO || !app.isPackaged) { upd.state = 'disabled'; return; }
  try { updater = require('electron-updater').autoUpdater; } catch { upd.state = 'disabled'; return; }
  updater.autoDownload = true;
  updater.autoInstallOnAppQuit = true;
  updater.allowPrerelease = true;
  updater.on('checking-for-update', () => setUpd({ state: 'checking', error: null }));
  updater.on('update-available', i => setUpd({ state: 'downloading', version: i.version, progress: 0 }));
  updater.on('update-not-available', () => setUpd({ state: 'latest', checkedAt: new Date().toISOString() }));
  updater.on('download-progress', p => setUpd({ progress: Math.round(p.percent || 0) }));
  updater.on('error', e => setUpd({ state: 'error', error: String((e && e.message) || e).split('\n')[0] }));
  updater.on('update-downloaded', i => {
    setUpd({ state: 'ready', version: i.version, progress: 100 });
    // Nobody is looking at the window: install quietly now and come back in the tray.
    if (!win || win.isDestroyed() || !win.isVisible()) return installUpdate();
    notify('OpenSweep update ready', `Version ${i.version} is downloaded. Restart OpenSweep to use it.`, showMain);
  });
  const check = () => { if (settings.autoUpdate && upd.state !== 'downloading' && upd.state !== 'ready') updater.checkForUpdates().catch(() => {}); };
  setTimeout(check, 20e3);
  setInterval(check, 4 * 3600e3);
}
function installUpdate() {
  if (!updater || upd.state !== 'ready') return { ok: false };
  quitting = true;
  setImmediate(() => updater.quitAndInstall(true, true));
  return { ok: true };
}

function fmt(b) { const u = ['B', 'KB', 'MB', 'GB', 'TB']; let i = 0; while (b >= 1024 && i < 4) { b /= 1024; i++; } return `${b.toFixed(i > 1 ? 1 : 0)} ${u[i]}`; }

// Move to the Recycle Bin; if Electron's call fails (seen on some elevated setups), use Windows' own API.
async function recycle(p) {
  try { await shell.trashItem(p); }
  catch (e) {
    if (process.platform !== 'win32') throw e;
    await backend.recycle(p);
  }
  if (fs.existsSync(p)) throw new Error('Windows did not delete it (the file may be open in another app)');
}

// ------------------------------------------------------------------ downloads organiser (pure Node)
let organizeDir = null; // a folder the user picked with the folder dialog
function organizeTarget(where) {
  if (where === 'desktop') return app.getPath('desktop');
  if (where === 'documents') return app.getPath('documents');
  if (where === 'custom' && organizeDir) return organizeDir;
  return app.getPath('downloads');
}
// Sort loose files by type. "all" = system-wide: every personal folder, each type sent to its library.
function organizeDownloads({ apply, where }) {
  const lib = { Images: 'pictures', Videos: 'videos', Music: 'music', Documents: 'documents' };
  const sources = where === 'all' ? ['desktop', 'downloads', 'documents', 'pictures', 'videos', 'music'].map(k => app.getPath(k)) : [organizeTarget(where)];
  const moves = [];
  for (const dir of sources) {
    let names = []; try { names = fs.readdirSync(dir); } catch { continue; }
    for (const name of names) {
      const full = path.join(dir, name);
      let st; try { st = fs.statSync(full); } catch { continue; }
      if (!st.isFile() || name.startsWith('.') || /\.(crdownload|part|tmp)$/i.test(name) || name.toLowerCase() === 'desktop.ini') continue;
      const ext = path.extname(name).toLowerCase();
      const folder = Object.keys(C.DOWNLOAD_TYPES).find(k => C.DOWNLOAD_TYPES[k].includes(ext));
      if (!folder) continue;
      let destDir;
      if (where !== 'all') destDir = path.join(dir, folder);
      else if (lib[folder]) destDir = app.getPath(lib[folder]);
      else destDir = path.join(app.getPath(folder === 'Code' ? 'documents' : 'downloads'), folder);
      if (path.resolve(destDir) === path.resolve(dir)) continue; // already in the right place
      moves.push({ name, from: full, destDir, folder: where === 'all' ? `${folder} → ${destDir}` : folder, size: st.size });
    }
  }
  const done = [];
  if (apply && !DEMO) {
    for (const mv of moves) {
      try {
        fs.mkdirSync(mv.destDir, { recursive: true });
        let dest = path.join(mv.destDir, mv.name), n = 1;
        while (fs.existsSync(dest)) { const e = path.extname(mv.name); dest = path.join(mv.destDir, `${path.basename(mv.name, e)} (${n++})${e}`); }
        fs.renameSync(mv.from, dest);
        done.push({ from: mv.from, to: dest });
      } catch { /* in use: skip */ }
    }
    if (done.length) logHistory({ kind: 'files', summary: `Organised ${done.length} file(s)${where === 'all' ? ' across your folders' : ''}`, undo: { fn: 'organizeUndo', args: { moves: done } } });
  }
  return { dir: sources.join(', '), moves: moves.map(m => ({ name: m.name, folder: m.folder, size: m.size })), applied: !!apply && !DEMO, moved: done.length };
}

// ------------------------------------------------------------------ network latency (TCP connect time; only on user request)
function tcpPing(host, port = 443, timeout = 3000) {
  return new Promise(resolve => {
    const t0 = process.hrtime.bigint();
    const sock = net.connect({ host, port });
    const done = ms => { sock.destroy(); resolve(ms); };
    sock.setTimeout(timeout, () => done(null));
    sock.once('error', () => done(null));
    sock.once('connect', () => done(Number(process.hrtime.bigint() - t0) / 1e6));
  });
}
async function latency() {
  const targets = [['Cloudflare', '1.1.1.1'], ['Google', '8.8.8.8'], ['Quad9', '9.9.9.9']];
  const out = [];
  for (const [name, host] of targets) {
    const samples = [];
    for (let i = 0; i < 4; i++) samples.push(await tcpPing(host));
    const ok = samples.filter(x => x !== null);
    out.push({ name, host, avg: ok.length ? Math.round(ok.reduce((a, b) => a + b, 0) / ok.length) : null, loss: Math.round(100 * (samples.length - ok.length) / samples.length) });
  }
  return out;
}

// ------------------------------------------------------------------ OpenSweep Core (native monitor service, Windows only)
// The renderer only picks a method; the exe path, service name and commands are fixed here.
const CORE_SERVICE = 'OpenSweepCore';
let coreClient = null;
const core = () => coreClient || (coreClient = require('./core-client').createCoreClient());
const coreExe = () => (app.isPackaged ? path.join(process.resourcesPath, 'core', 'OpenSweepCore.exe') : path.join(__dirname, '..', 'core', 'build', 'Release', 'OpenSweepCore.exe'));
const scExe = () => path.join(process.env.SystemRoot || 'C:\\Windows', 'System32', 'sc.exe');
// 'running' | 'stopped' | 'start_pending' | 'stop_pending' | ... | 'not_installed' | 'unknown'
function coreServiceState() {
  return new Promise(resolve => {
    require('child_process').execFile(scExe(), ['query', CORE_SERVICE], { windowsHide: true, timeout: 10000 }, (err, stdout) => {
      const m = /STATE\s*:\s*\d+\s+(\w+)/.exec(stdout || '');
      if (m) return resolve(m[1].toLowerCase());
      resolve(/1060/.test(stdout || '') || (err && err.code === 1060) ? 'not_installed' : 'unknown');
    });
  });
}
async function coreStatus() {
  if (DEMO) return backend.coreStatus();
  const [serviceState, hello] = await Promise.all([coreServiceState(), core().call('hello', undefined, { timeout: 1500 }).catch(e => ({ _err: e }))]);
  let health = null, engine = null, error = null;
  if (hello._err) error = { code: hello._err.code || 'internal', message: hello._err.message };
  else {
    // `status` exists from Phase 2 on; an older service answers unknown_cmd and the UI shows Phase 1 data only.
    [health, engine] = await Promise.all([core().call('health').catch(() => null), core().call('status').catch(e => ({ _unsupported: e.code === 'unknown_cmd', _code: e.code || 'internal' }))]);
    if (engine && engine._code) engine = { unavailable: true, unsupported: engine._unsupported, code: engine._code };
    startForegroundHint();
  }
  const ok = !hello._err;
  return { available: ok, running: ok || serviceState === 'running', installed: serviceState === 'unknown' ? null : serviceState !== 'not_installed', serviceState,
    hello: ok ? hello : null, health, status: engine, exePresent: fs.existsSync(coreExe()), error };
}

// Phase 2–8 commands. The renderer names a method from core-validate's fixed table; args are rebuilt
// there. Admin commands are confirmed here with a native dialog that states exactly what will happen.
const coreValidate = require('./core-validate');
const coreRead = (cmd, args) => (DEMO ? backend.coreCommand(cmd, args) : core().call(cmd, args, { timeout: 5000 }));
async function coreAdmin(method, input) {
  const { cmd, args } = coreValidate.prepare(method, input);
  // Look up what is being acted on so the dialog can describe it (and unknown ids fail before anything is sent).
  const ctx = {};
  if (method === 'coreSetMode') ctx.current = await coreRead('status').then(s => s && s.mode).catch(() => null);
  if (method === 'coreRollbackAll' || method === 'coreDisablePolicy') {
    const active = (await coreRead('policies')).active || [];
    ctx.count = active.length;
    if (method === 'coreDisablePolicy') { ctx.policy = active.find(p => p.txId === args.txId); if (!ctx.policy) throw new Error('That policy is no longer active — refresh the page.'); }
  }
  if (method === 'coreApprove') { ctx.approval = ((await coreRead('heal')).pending || []).find(a => a.id === args.id); if (!ctx.approval) throw new Error('That approval request is no longer pending — refresh the page.'); }
  const d = coreValidate.describeAdmin(method, args, ctx);
  const opts = { type: 'warning', title: d.title, message: d.message, detail: d.detail + (DEMO ? '\n\n(Demo mode: simulated, nothing on this PC changes.)' : ''), buttons: ['Cancel', d.ok], defaultId: 0, cancelId: 0, noLink: true };
  if (d.consent) opts.checkboxLabel = d.consent;
  const r = win && !win.isDestroyed() ? await dialog.showMessageBox(win, opts) : await dialog.showMessageBox(opts);
  if (r.response !== 1) return { cancelled: true };
  if (d.consent && !r.checkboxChecked) throw new Error('Advanced mode needs your explicit consent (tick the box in the confirmation).');
  let res;
  try { res = await (DEMO ? backend.coreCommand(cmd, args) : core().call(cmd, args, { timeout: 15000 })); }
  catch (e) { if (e.code === 'forbidden') throw new Error('OpenSweep Core refused: this change needs OpenSweep to run as administrator.'); throw e; }
  logHistory({ kind: 'core', summary: `OpenSweep Core: ${d.title}${cmd === 'setMode' ? ` → ${args.mode}` : ''}${DEMO ? ' (simulated)' : ''}` });
  return res;
}

// Foreground hint (phases-2-8 `reportForeground`): the service runs in session 0 and cannot see the
// foreground window, so the logged-on app reports its process id every 2 s while Core is connected.
// One persistent PowerShell process; P/Invoke is defined in memory (Reflection.Emit), so no compiled
// DLL is written to disk. Not used in demo mode. Stops when Core does not support the command.
const FG_SCRIPT = `$ErrorActionPreference = 'Stop'
$asm = [Reflection.Emit.AssemblyBuilder]::DefineDynamicAssembly((New-Object Reflection.AssemblyName 'OpenSweepFg'), [Reflection.Emit.AssemblyBuilderAccess]::Run)
$tb = $asm.DefineDynamicModule('OpenSweepFg').DefineType('OpenSweepFg.U', 'Public, Class')
$attr = [Reflection.MethodAttributes]'Public, Static, PinvokeImpl'
$m1 = $tb.DefinePInvokeMethod('GetForegroundWindow', 'user32.dll', $attr, [Reflection.CallingConventions]::Standard, [IntPtr], [Type[]]@(), [Runtime.InteropServices.CallingConvention]::Winapi, [Runtime.InteropServices.CharSet]::Auto)
$m1.SetImplementationFlags('PreserveSig')
$m2 = $tb.DefinePInvokeMethod('GetWindowThreadProcessId', 'user32.dll', $attr, [Reflection.CallingConventions]::Standard, [UInt32], [Type[]]@([IntPtr], [UInt32].MakeByRefType()), [Runtime.InteropServices.CallingConvention]::Winapi, [Runtime.InteropServices.CharSet]::Auto)
$m2.SetImplementationFlags('PreserveSig')
$u = $tb.CreateType()
while ($true) {
  $p = [UInt32]0; $h = $u::GetForegroundWindow()
  if ($h -ne [IntPtr]::Zero) { [void]$u::GetWindowThreadProcessId($h, [ref]$p) }
  [Console]::Out.WriteLine('{"pid":' + $p + '}'); [Console]::Out.Flush()
  Start-Sleep -Milliseconds 2000
}`;
const fg = { stop: null, unsupportedUntil: 0, lastErr: null };
function startForegroundHint() {
  if (DEMO || fg.stop || Date.now() < fg.unsupportedUntil) return;
  const startedAt = Date.now();
  let mine = null;
  try {
    mine = fg.stop = require('./backend/pscore').stream(FG_SCRIPT, undefined, (d) => {
      const pid = d && d.pid;
      if (!coreClient || !coreClient.connected || !Number.isSafeInteger(pid) || pid <= 4) return; // only while connected
      coreClient.call('reportForeground', { pid }, { timeout: 1500 }).then(() => { fg.lastErr = null; }).catch(e => {
        fg.lastErr = e.code;
        if (e.code === 'unknown_cmd') { fg.unsupportedUntil = Date.now() + 10 * 60e3; stopForegroundHint(); }
      });
    }, () => {
      if (fg.stop !== mine) return; // stopped on purpose, or already replaced by a newer helper
      fg.stop = null;
      if (Date.now() - startedAt < 10e3) fg.unsupportedUntil = Date.now() + 10 * 60e3; // died at start: back off
    });
  } catch { fg.stop = null; }
}
function stopForegroundHint() { const s = fg.stop; fg.stop = null; if (s) s(); }
// Supervisor: (re)start the hint when Core answers, stop it when Core is gone. Cheap: one hello every 30 s.
function scheduleForegroundHint() {
  if (DEMO) return;
  let misses = 0;
  setInterval(async () => {
    const p = await core().probe({ timeout: 1500 });
    if (p.available) { misses = 0; startForegroundHint(); }
    else if (++misses >= 2) stopForegroundHint();
  }, 30e3);
  setTimeout(() => core().probe({ timeout: 1500 }).then(p => { if (p.available) startForegroundHint(); }), 15e3);
}
async function coreAction(action) {
  if (DEMO) { await backend.coreAction(action); logHistory({ kind: 'core', summary: `OpenSweep Core: ${action} (simulated)` }); return backend.coreStatus(); }
  const exe = coreExe();
  if (action === 'install' && !fs.existsSync(exe)) throw new Error('OpenSweep Core is not included in this build of OpenSweep.');
  const run = (cmd) => `$o = ${cmd} 2>&1 | Out-String; Out-Result @{ code = $LASTEXITCODE; out = $o }`;
  const scripts = {
    install: run('& $a.exe --install'),
    uninstall: `if (Test-Path -LiteralPath $a.exe) { ${run('& $a.exe --uninstall')} } else { & $a.sc stop $a.svc | Out-Null; ${run('& $a.sc delete $a.svc')} }`,
    start: run('& $a.sc start $a.svc'),
    stop: run('& $a.sc stop $a.svc'),
  };
  const { elevated } = require('./backend/pscore');
  const r = await elevated(scripts[action], { exe, sc: scExe(), svc: CORE_SERVICE }, { timeout: 60000 }) || {};
  // 1056 = already running, 1062 = not started: the wanted state already holds.
  if (r.code && !(action === 'start' && r.code === 1056) && !(action === 'stop' && r.code === 1062)) {
    throw new Error(`OpenSweep Core ${action} failed (code ${r.code}): ${String(r.out || '').trim().split(/\r?\n/).filter(Boolean).slice(-2).join(' ')}`);
  }
  const want = { start: 'running', stop: 'stopped', uninstall: 'not_installed' }[action];
  let st = await coreServiceState();
  for (let i = 0; want && st !== want && i < 30; i++) { await new Promise(res => setTimeout(res, 500)); st = await coreServiceState(); }
  if (want && st !== want) throw new Error(`OpenSweep Core ${action}: the service is "${st}" instead of "${want}".`);
  if (action === 'install' && st === 'not_installed') throw new Error('OpenSweep Core install: the service was not registered.');
  if (action === 'stop' || action === 'uninstall') { stopForegroundHint(); try { coreClient && coreClient.close(); } catch { /* ignore */ } }
  logHistory({ kind: 'core', summary: `OpenSweep Core: ${{ install: 'installed', uninstall: 'uninstalled', start: 'started', stop: 'stopped' }[action]}` });
  return coreStatus();
}

// ------------------------------------------------------------------ IPC API
const str =(x, max = 4096) => { if (typeof x !== 'string' || x.length > max) throw new Error('Invalid argument'); return x; };
const SERVICE_TYPES = ['Automatic', 'Manual', 'Disabled'];

const api = {
  info: async () => ({ demo: DEMO, platform: process.platform, version: app.getVersion(), home: os.homedir(), isAdmin: DEMO ? true : await backend.isAdmin() }),
  catalog: () => C,
  stats,
  health,
  systemInfo: () => backend.systemInfo(),

  processes: () => backend.processes(),
  killProcess: async ({ pid, name }) => {
    if (!Number.isInteger(pid) || pid <= 4) throw new Error('Invalid process');
    if (C.PROTECTED_PROCESSES.includes(String(name || '').toLowerCase())) throw new Error(`${name} is a critical Windows process and cannot be ended.`);
    return backend.killProcess({ pid });
  },
  boost: async () => { const r = await backend.boost(); logHistory({ kind: 'boost', summary: `Boost freed ${r.memFreedMB} MB RAM` }); return r; },

  cleanupScan: () => backend.cleanupScan({}),
  cleanupRun: async ({ ids, elevate }) => {
    const valid = (ids || []).filter(id => C.CLEANUP.some(c => c.id === id));
    const r = await backend.cleanupRun({ ids: valid, elevate: !!elevate });
    logHistory({ kind: 'cleanup', summary: `Cleaned ${fmt(r.reduce((a, x) => a + x.freed, 0))}` });
    return r;
  },

  largeFiles: async ({ root, minMB }) => { const r = await backend.largeFiles({ root: str(root || os.homedir()), minMB: Math.max(1, Number(minMB) || 100), limit: 300 }); (r || []).forEach(f => cache.deletable.add(f.path)); return r; },
  duplicates: async ({ root, minKB }) => { const r = await backend.duplicates({ root: str(root || os.homedir()), minKB: Math.max(1, Number(minKB) || 256) }); (r || []).forEach(g => g.files.forEach(f => cache.deletable.add(f.path))); return r; },
  folderSizes: ({ root }) => backend.folderSizes({ root: str(root || os.homedir()) }),
  deleteFiles: async ({ paths }) => {
    const res = [];
    for (const p of paths || []) {
      if (!cache.deletable.has(p)) { res.push({ path: p, ok: false, error: 'Not from a scan result' }); continue; }
      try { if (!DEMO) await recycle(p); cache.deletable.delete(p); res.push({ path: p, ok: true }); }
      catch (e) { res.push({ path: p, ok: false, error: e.message }); }
    }
    logHistory({ kind: 'files', summary: `Moved ${res.filter(r => r.ok).length} file(s) to the Recycle Bin` });
    return res;
  },
  revealFile: ({ path: p }) => { if (cache.deletable.has(p) && !DEMO) shell.showItemInFolder(p); return { ok: true }; },
  organizeDownloads: ({ apply, where }) => organizeDownloads({ apply: !!apply, where }),
  organizeUndo: ({ moves }) => {
    let n = 0;
    const roots = ['desktop', 'downloads', 'documents', 'pictures', 'videos', 'music'].map(k => path.resolve(app.getPath(k)).toLowerCase() + path.sep);
    const inside = (p) => typeof p === 'string' && roots.some(r => path.resolve(p).toLowerCase().startsWith(r));
    for (const m of moves || []) { if (!inside(m.from) || !inside(m.to)) continue; try { if (fs.existsSync(m.to) && !fs.existsSync(m.from)) { fs.renameSync(m.to, m.from); n++; } } catch { /* skip */ } }
    return { ok: true, restored: n };
  },
  organizePick: async () => { const r = await dialog.showOpenDialog(win, { title: 'Folder to organise', properties: ['openDirectory'] }); if (r.canceled) return null; organizeDir = r.filePaths[0]; return organizeDir; },

  installedApps: async () => { const r = await backend.installedApps(); cache.apps = new Map((r || []).map(a => [a.id, a])); return r; },
  uninstallApp: async ({ id }) => {
    const a = cache.apps.get(id);
    if (!a) throw new Error('Unknown app — refresh the list');
    const r = await backend.uninstallApp(a);
    logHistory({ kind: 'uninstall', summary: `Uninstall started: ${a.name}` });
    return r;
  },

  startupItems: async () => { const r = await backend.startupItems(); cache.startup = new Map((r || []).map(s => [s.id, s])); return r; },
  setStartup: async ({ id, enabled }) => {
    const s = cache.startup.get(id);
    if (!s) throw new Error('Unknown startup item — refresh the list');
    await backend.setStartup(s, !!enabled);
    const now = (await api.startupItems()).find(x => x.id === id);
    if (now && now.enabled !== !!enabled) throw new Error(`${s.name}: the change did not stick. Try again or check Task Manager → Startup apps.`);
    const r = { ok: true, verified: true };
    logHistory({ kind: 'startup', summary: `${enabled ? 'Enabled' : 'Disabled'} startup: ${s.name}`, undo: { fn: 'setStartup', args: { id, enabled: !enabled } } });
    return r;
  },

  security: () => backend.security(),
  threats: () => backend.threats(),
  defenderAction: async ({ action }) => {
    const r = await backend.defenderAction({ action: str(action, 32) });
    const check = { 'firewall-on': se => (se.firewall || []).every(f => f.enabled), 'smb1-off': se => !se.smb1, 'rdp-off': se => !se.rdpEnabled }[action];
    if (check && !check(await backend.security())) throw new Error('Windows did not keep this change. Another security app or a policy may be controlling it.');
    if (check) logHistory({ kind: 'security', summary: { 'firewall-on': 'Firewall turned on', 'smb1-off': 'SMB1 disabled', 'rdp-off': 'Remote Desktop disabled' }[action] });
    return r;
  },

  network: () => backend.network(),
  latency,
  networkAction: async ({ action, index, dns }) => {
    if (action === 'dns') {
      const p = C.DNS_PROVIDERS.find(d => d.id === dns);
      if (!p || !Number.isInteger(index)) throw new Error('Invalid DNS selection');
      const r = await backend.networkAction({ action, index, servers: p.servers });
      if (!DEMO && p.servers.length) {
        const a = (await backend.network() || []).find(x => x.index === index);
        if (a && p.servers.filter(x => x.includes('.')).some(x => !(a.dns || []).includes(x))) throw new Error(`DNS for ${a.name} did not change (it shows ${(a.dns || []).join(', ') || 'automatic'}).`);
      }
      logHistory({ kind: 'network', summary: `DNS set to ${p.name}` });
      return r;
    }
    if (!['flushdns', 'renew', 'hosts'].includes(action)) throw new Error('Unknown action');
    return backend.networkAction({ action });
  },

  tweakStates: () => backend.tweakStates(),
  setTweaks: async ({ changes, label }) => {
    const valid = (changes || []).filter(c => C.TWEAKS.some(t => t.id === c.id)).map(c => ({ id: c.id, on: !!c.on }));
    if (!valid.length) return { ok: true };
    const r = await backend.setTweaks(valid);
    const done = valid.filter(c => !(r.failed || []).some(f => f.id === c.id));
    // Settings guard remembers what you turned on, and forgets what you turned off.
    const g = new Set(settings.guardTweaks || []);
    done.forEach(c => (c.on ? g.add(c.id) : g.delete(c.id)));
    settings.guardTweaks = [...g];
    if (done.length) logHistory({ kind: 'tweaks', summary: `${label || 'Settings changed'} (${done.length} verified${r.failed && r.failed.length ? `, ${r.failed.length} failed` : ''})`, undo: { fn: 'setTweaks', args: { changes: done.map(c => ({ id: c.id, on: !c.on })), label: `Undo: ${label || 'settings'}` } } });
    return r;
  },

  bloatStatus: () => backend.bloatStatus(),
  removeApps: async ({ pkgs, allUsers }) => {
    const valid = (pkgs || []).filter(p => typeof p === 'string' && /^[A-Za-z0-9._-]{2,128}$/.test(p));
    const r = await backend.removeApps({ pkgs: valid, allUsers: !!allUsers });
    logHistory({ kind: 'debloat', summary: `Removed ${valid.length} preinstalled app(s)` });
    return r;
  },
  servicesStatus: () => backend.servicesStatus(),
  setServices: async ({ changes }) => {
    const valid = (changes || []).filter(c => C.SERVICES.some(s => s.name === c.name) && SERVICE_TYPES.includes(c.type));
    const r = await backend.setServices(valid);
    const ok = (r || []).filter(x => x.ok).length;
    logHistory({ kind: 'services', summary: `Changed ${ok} of ${valid.length} service(s) (verified)` });
    return r;
  },
  tasksStatus: () => backend.tasksStatus(),
  setTasks: async ({ changes }) => {
    const valid = (changes || []).map(c => ({ t: C.SCHEDULED_TASKS.find(t => t.path + t.name === c.id), enable: !!c.enable })).filter(x => x.t).map(x => ({ path: x.t.path, name: x.t.name, enable: x.enable }));
    const r = await backend.setTasks(valid);
    const ok = (r.results || []).filter(x => x.ok).length;
    logHistory({ kind: 'tasks', summary: `${ok} of ${valid.length} scheduled task(s) changed (verified)`, undo: { fn: 'setTasks', args: { changes: valid.map(v => ({ id: v.path + v.name, enable: !v.enable })) } } });
    return r;
  },

  updatesStatus: () => backend.updatesStatus(),
  updatesAction: async ({ action, days, start, end, kb, version }) => {
    const args = { action };
    if (action === 'pause') { args.days = Math.round(Number(days)); if (!(args.days >= 1 && args.days <= 365)) throw new Error('Pause must be 1–365 days'); }
    else if (action === 'active-hours') {
      args.start = Number(start); args.end = Number(end);
      if (![args.start, args.end].every(h => Number.isInteger(h) && h >= 0 && h <= 23) || args.start === args.end) throw new Error('Invalid active hours');
    } else if (action === 'lock') {
      const st = await backend.updatesStatus();
      args.product = st.product; args.version = String(version || st.displayVersion);
      if (!/^\d{2}H\d$/.test(args.version)) throw new Error('Invalid version');
    } else if (action === 'uninstall') { if (!/^KB\d{6,8}$/.test(kb)) throw new Error('Invalid update ID'); args.kb = kb; }
    else if (!['resume', 'unlock', 'scan'].includes(action)) throw new Error('Unknown action');
    const r = await backend.updatesAction(args);
    if (['pause', 'resume', 'active-hours', 'lock', 'unlock'].includes(action)) {
      const st = await backend.updatesStatus();
      const paused = st.pausedUntil && new Date(st.pausedUntil).getTime() > Date.now();
      const ok = { pause: paused, resume: !paused, 'active-hours': Number(st.activeStart) === args.start && Number(st.activeEnd) === args.end, lock: st.lockedVersion === args.version, unlock: !st.lockedVersion }[action];
      if (!ok) throw new Error('Windows did not keep this Windows Update change. If your PC is managed by work or school, their policy wins.');
    }
    const label = { pause: `Paused updates for ${args.days} days`, resume: 'Resumed updates', 'active-hours': `Active hours set to ${args.start}:00–${args.end}:00`, lock: `Locked Windows to version ${args.version}`, unlock: 'Unlocked Windows version', uninstall: `Uninstall started for ${kb}` }[action];
    if (label) logHistory({ kind: 'updates', summary: label });
    return r;
  },
  powerPlans: () => backend.powerPlans(),
  setPowerPlan: ({ guid }) => { if (!/^[0-9a-f-]{36}$/i.test(guid)) throw new Error('Invalid plan'); return backend.setPowerPlan({ guid }); },
  runRepair: async ({ id }) => {
    const r = C.REPAIRS.find(x => x.id === id);
    if (!r) throw new Error('Unknown repair');
    logHistory({ kind: 'repair', summary: `Ran: ${r.name}` });
    return backend.runRepair(r);
  },
  launchTool: ({ id }) => { const t = C.TOOLBOX.find(x => x.id === id); if (!t) throw new Error('Unknown tool'); return backend.launchTool(t); },
  openShortcut: async ({ index }) => {
    const s = settings.shortcuts[index];
    if (!s) throw new Error('Unknown shortcut');
    if (/^https?:\/\//i.test(s.target)) await shell.openExternal(s.target);
    else { const err = await shell.openPath(s.target); if (err) throw new Error(err); }
    return { ok: true };
  },
  // Shortcuts: files are only added through the system file picker, links must be http(s) (security-audit S4).
  addShortcut: async ({ name, url }) => {
    const n = String(name || '').trim().slice(0, 60);
    let target;
    if (url !== undefined) { target = String(url).trim(); if (!/^https?:\/\/[^\s]+$/i.test(target) || target.length > 1024) throw new Error('Enter a web address starting with https://'); }
    else { const r = await dialog.showOpenDialog(win, { title: 'Choose an app, file or folder', properties: ['openFile', 'openDirectory'] }); if (r.canceled) return null; target = r.filePaths[0]; }
    settings.shortcuts = [...(settings.shortcuts || []), { name: n || path.basename(target), target }].slice(0, 50);
    saveSettings(); return api.getSettings();
  },
  removeShortcut: ({ index }) => { if (!Number.isInteger(index)) throw new Error('Invalid'); settings.shortcuts = (settings.shortcuts || []).filter((_, i) => i !== index); saveSettings(); return api.getSettings(); },
  openUri: async ({ uri }) => {
    if (!/^(ms-settings:[a-z-]*|windowsdefender:)$/i.test(uri)) throw new Error('Blocked URI');
    if (!DEMO) await shell.openExternal(uri);
    return { ok: true };
  },
  openRepo: () => shell.openExternal('https://github.com/'),

  getSettings: () => ({ ...settings, history: undefined, stabilitySeen: undefined, guardTweaks: undefined, guardCount: (settings.guardTweaks || []).length }),
  setSettings: ({ patch }) => {
    const allowed = ['theme', 'minimizeToTray', 'showWidget', 'launchAtLogin', 'confirmActions', 'scanRoot', 'autoUpdate', 'stabilityMonitor', 'stabilityNotify', 'settingsGuard', 'autoClean', 'compactNav'];
    if ('autoClean' in (patch || {}) && !['off', 'weekly', 'monthly'].includes(patch.autoClean)) throw new Error('Invalid schedule');
    const cleanChanged = 'autoClean' in (patch || {}) && patch.autoClean !== settings.autoClean;
    for (const k of allowed) if (k in (patch || {})) settings[k] = patch[k];
    if (Array.isArray(settings.shortcuts)) settings.shortcuts = settings.shortcuts.slice(0, 50).map(s => ({ name: String(s.name).slice(0, 60), target: String(s.target).slice(0, 1024) }));
    saveSettings();
    applySettings();
    if (cleanChanged && !DEMO) backend.setCleanSchedule({ freq: settings.autoClean, exe: process.execPath }).catch(e => logHistory({ kind: 'cleanup', summary: `Could not set the cleaning schedule: ${e.message}` }));
    return api.getSettings();
  },
  history: () => settings.history,
  undo: async ({ index }) => {
    const h = settings.history[index];
    if (!h || !h.undo || h.undone) throw new Error('Nothing to undo');
    // settings.json is user-writable: only known undo actions, with their arguments re-validated.
    if (!UNDO_FNS.has(h.undo.fn)) throw new Error('This history entry cannot be undone');
    await api[h.undo.fn](h.undo.args || {});
    h.undone = true; saveSettings();
    return { ok: true };
  },
  relaunchAsAdmin: async () => {
    if (DEMO) return { ok: false, demo: true };
    const { execFile } = require('child_process');
    const args = process.argv.slice(1).map(a => `'${a.replace(/'/g, "''")}'`).join(',');
    execFile('powershell.exe', ['-NoProfile', '-Command', `Start-Process -FilePath '${process.execPath.replace(/'/g, "''")}' -Verb RunAs ${args ? `-ArgumentList @(${args})` : ''}`], { windowsHide: true }, (err) => { if (!err) { quitting = true; app.quit(); } });
    return { ok: true };
  },
  openMain: () => { showMain(); return { ok: true }; },
  stability: async ({ refresh }) => (refresh || !stability ? runStability({ manual: true }) : stability),
  scanDevices: async () => { const r = await backend.scanDevices(); logHistory({ kind: 'drivers', summary: 'Scanned for hardware changes' }); return r; },
  updateStatus: () => ({ ...upd, current: app.getVersion() }),
  checkUpdate: async () => { if (!updater) return { ...upd }; await updater.checkForUpdates().catch(e => setUpd({ state: 'error', error: e.message })); return { ...upd }; },
  installUpdate: () => installUpdate(),
  closeWidget: () => { settings.showWidget = false; saveSettings(); applySettings(); return { ok: true }; },

  // OpenSweep Core: Phase 1 methods use no renderer input; the Phase 2–8 methods are added below
  // from core-validate's fixed table (strict argument checks, admin methods confirmed natively).
  coreStatus: () => coreStatus(),
  coreMetrics: () => (DEMO ? backend.coreMetrics() : core().call('metrics')),
  coreTopology: () => (DEMO ? backend.coreTopology() : core().call('topology')),
  coreInstall: () => coreAction('install'),
  coreUninstall: () => coreAction('uninstall'),
  coreStart: () => coreAction('start'),
  coreStop: () => coreAction('stop'),
};

const extra = require('./api-extra')({ recycle, backend, DEMO, app, shell, dialog, send: (...a) => send(...a), logHistory, fmt, getWin: () => win });
Object.assign(api, extra, {
  guardStatus: () => ({ enabled: !!settings.settingsGuard, count: (settings.guardTweaks || []).length }),
  guardRun: () => runGuard({ manual: true }),
});
for (const [method, spec] of Object.entries(coreValidate.CORE_METHODS)) {
  api[method] = spec.cap === 'admin' ? (args) => coreAdmin(method, args) : (args) => { const p = coreValidate.prepare(method, args); return coreRead(p.cmd, p.args); };
}

const UNDO_FNS = new Set(['setTweaks', 'setStartup', 'setTasks', 'setFeature', 'restoreBackup', 'organizeUndo']);
const INTERNAL_ONLY = new Set(['organizeUndo', 'restoreBackup', '_stopAll']); // only reachable through History & undo
ipcMain.handle('api', async (_e, method, args) => {
  if (!Object.prototype.hasOwnProperty.call(api, method) || INTERNAL_ONLY.has(method)) throw new Error('Unknown method ' + method);
  return api[method](args || {});
});

// ------------------------------------------------------------------ windows
const ICON_SVG = `<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64" viewBox="0 0 64 64"><defs><linearGradient id="g" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#2dd4bf"/><stop offset="1" stop-color="#3b82f6"/></linearGradient></defs><rect width="64" height="64" rx="16" fill="url(#g)"/><path d="M18 40c6-14 22-20 30-16-6 2-10 6-12 12-2 6-10 10-18 4z" fill="#fff"/><circle cx="44" cy="20" r="4" fill="#fff"/></svg>`;
const appIcon = () => nativeImage.createFromDataURL('data:image/svg+xml;base64,' + Buffer.from(ICON_SVG).toString('base64'));

const webPrefs = { preload: path.join(__dirname, 'preload.js'), contextIsolation: true, nodeIntegration: false, sandbox: true };

function createMain() {
  win = new BrowserWindow({ width: 1240, height: 820, minWidth: 960, minHeight: 640, title: 'OpenSweep', backgroundColor: '#0f1115', icon: appIcon(), autoHideMenuBar: true, show: false, webPreferences: webPrefs });
  win.loadFile(path.join(__dirname, 'renderer', 'index.html'));
  win.once('ready-to-show', () => win.show());
  win.on('close', e => { if (!quitting && settings.minimizeToTray && tray) { e.preventDefault(); win.hide(); } });
  win.webContents.setWindowOpenHandler(() => ({ action: 'deny' }));
  win.webContents.on('will-navigate', e => e.preventDefault());
}
function showMain() { if (!win || win.isDestroyed()) createMain(); win.show(); win.focus(); }

function createWidget() {
  if (widget && !widget.isDestroyed()) return;
  const { workArea } = screen.getPrimaryDisplay();
  widget = new BrowserWindow({ width: 300, height: 76, x: workArea.x + workArea.width - 320, y: workArea.y + 20, frame: false, resizable: false, alwaysOnTop: true, skipTaskbar: true, transparent: true, hasShadow: false, webPreferences: webPrefs });
  widget.loadFile(path.join(__dirname, 'renderer', 'widget.html'));
  widget.on('closed', () => { widget = null; });
}

function createTray() {
  tray = new Tray(appIcon().resize({ width: 16, height: 16 }));
  tray.setToolTip('OpenSweep');
  const menu = () => Menu.buildFromTemplate([
    { label: 'Open OpenSweep', click: showMain },
    { label: 'Boost now', click: async () => { const r = await api.boost(); tray.displayBalloon?.({ title: 'OpenSweep', content: `Freed ${r.memFreedMB} MB of memory` }); } },
    { label: 'Mini toolbar', type: 'checkbox', checked: settings.showWidget, click: (i) => api.setSettings({ patch: { showWidget: i.checked } }) },
    { type: 'separator' },
    { label: 'Quit', click: () => { quitting = true; app.quit(); } },
  ]);
  tray.setContextMenu(menu());
  tray.on('click', showMain);
  tray.on('right-click', () => tray.setContextMenu(menu()));
}

function applySettings() {
  if (settings.showWidget) createWidget(); else if (widget) widget.close();
  if (!DEMO && autoStartApplied !== !!settings.launchAtLogin) {
    // OpenSweep runs as admin, which Windows won't start from the normal startup list,
    // so "Start with Windows" uses a logon task with highest privileges instead.
    autoStartApplied = !!settings.launchAtLogin;
    app.setLoginItemSettings({ openAtLogin: false });
    backend.setAutoStart({ enabled: autoStartApplied, exe: process.execPath }).catch(() => { autoStartApplied = null; });
  }
}
let autoStartApplied = null;

if (!app.requestSingleInstanceLock()) app.quit();
else {
  app.on('second-instance', (_e, argv) => { if (argv.includes('--auto-clean')) autoClean().catch(() => {}); else showMain(); });
  app.whenReady().then(() => {
    app.setAppUserModelId('org.opensweep.app');
    loadSettings();
    if (!DEMO) hardenDataDir();
    if (process.argv.includes('--auto-clean')) {
      // Scheduled run: clean quietly, then quit.
      autoClean().catch(() => {}).finally(() => setTimeout(() => { quitting = true; app.quit(); }, 8000));
      return;
    }
    createMain();
    if (process.argv.includes('--hidden')) win.once('ready-to-show', () => win.hide());
    try { createTray(); } catch { /* tray unsupported */ }
    if (!settings.launchAtLogin) autoStartApplied = false; // nothing to clean up on a normal start
    applySettings();
    setupUpdater();
    scheduleStability();
    scheduleForegroundHint();
    if (!DEMO) setTimeout(() => runGuard().catch(() => {}), 120e3);
  });
  app.on('before-quit', () => { quitting = true; try { extra._stopAll(); } catch { /* ignore */ } try { stopForegroundHint(); } catch { /* ignore */ } try { if (coreClient) coreClient.close(); } catch { /* ignore */ } });
  app.on('window-all-closed', () => { if (process.platform !== 'darwin') app.quit(); });
  app.on('activate', showMain);
}
