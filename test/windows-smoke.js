'use strict';
// Real-Windows smoke test for src/backend/windows.js. Runs on the GitHub Actions
// windows-latest runner (a throwaway VM with admin rights): calls every read function,
// then applies every registry tweak, reads it back, and reverts it.
// Usage (Windows only): npm run test:windows

const W = require('../src/backend/windows');
const C = require('../src/backend/catalog');

let failures = 0;
const fail = (what, msg) => { failures++; console.log(`  ✗ ${what}: ${msg}`); };
async function step(name, fn) {
  const t0 = Date.now();
  try {
    const r = await fn();
    const summary = Array.isArray(r) ? `${r.length} item(s)` : r && typeof r === 'object' ? Object.keys(r).slice(0, 8).join(', ') : String(r);
    console.log(`✓ ${name} (${Date.now() - t0} ms) → ${summary}`);
    return r;
  } catch (e) { fail(name, e.message); return null; }
}

(async () => {
  console.log('Admin:', await W.isAdmin());
  const reads = ['systemInfo', 'processes', 'installedApps', 'startupItems', 'security', 'threats', 'network', 'tweakStates', 'bloatStatus',
    'servicesStatus', 'tasksStatus', 'updatesStatus', 'powerPlans', 'stabilityCheck'];
  for (const f of reads) await step(f, () => W[f]({}));
  await step('cleanupScan', () => W.cleanupScan({ ids: C.CLEANUP.filter(c => c.rec).map(c => c.id) }));
  await step('largeFiles', () => W.largeFiles({ root: process.env.USERPROFILE, minMB: 50, limit: 20 }));
  await step('folderSizes', () => W.folderSizes({ root: process.env.USERPROFILE }));

  // Tweaks: apply everything, verify, then revert and verify again.
  const before = await W.tweakStates();
  const toApply = C.TWEAKS.filter(t => !before[t.id]).map(t => ({ id: t.id, on: true }));
  const on = await step(`setTweaks ON (${toApply.length})`, () => W.setTweaks(toApply));
  if (on) for (const f of on.failed) fail(`tweak ${f.id}`, f.error);
  const off = await step(`setTweaks OFF (${toApply.length})`, () => W.setTweaks(toApply.map(c => ({ id: c.id, on: false }))));
  if (off && off.failed.length) {
    // Diagnose: show what is actually in the registry, then retry once.
    const vals = off.failed.flatMap(f => C.TWEAKS.find(t => t.id === f.id).values.map(v => ({ id: f.id, key: v.key, name: v.name || '', want: v.off })));
    console.log(JSON.stringify(await W._ps(`Out-Json @(foreach ($q in $a) { $v = $null; try { $v = (Get-ItemProperty -LiteralPath ('Registry::' + $q.key) -Name $q.name -ErrorAction Stop).($q.name) } catch { $v = 'ERR ' + $_.Exception.Message }; [pscustomobject]@{ id=$q.id; name=$q.name; want=$q.want; got=$v } })`, vals), null, 1));
    const again = await W.setTweaks(off.failed.map(f => ({ id: f.id, on: false })));
    console.log('retry failed:', again.failed.map(f => f.id).join(', ') || 'none');
    for (const f of again.failed) fail(`revert ${f.id}`, f.error);
  }

  // Services: set each present one to its recommended type, verify, then restore.
  const svc = (await W.servicesStatus()).filter(s => s.status !== 'Missing');
  const changes = svc.map(s => ({ name: s.name, type: C.SERVICES.find(x => x.name === s.name).rec }));
  const sr = await step(`setServices (${changes.length})`, () => W.setServices(changes));
  if (sr) for (const r of sr.filter(x => !x.ok)) fail(`service ${r.name}`, r.error);
  await step('restore services', () => W.setServices(svc.map(s => ({ name: s.name, type: s.startType })).filter(c => c.type)));

  // Tasks
  const present = (await W.tasksStatus()).filter(t => t.state !== 'Missing');
  const tc = C.SCHEDULED_TASKS.filter(t => present.some(p => p.id === t.path + t.name)).map(t => ({ path: t.path, name: t.name, enable: false }));
  const tr = await step(`setTasks (${tc.length})`, () => W.setTasks(tc));
  if (tr) for (const r of tr.results.filter(x => !x.ok)) fail(`task ${r.id}`, r.error);

  // Windows Update controls
  await step('updates pause 7', () => W.updatesAction({ action: 'pause', days: 7 }));
  const u = await W.updatesStatus();
  if (!u.pausedUntil) fail('updates pause', 'pausedUntil not set');
  await step('updates resume', () => W.updatesAction({ action: 'resume' }));
  await step('updates active-hours', () => W.updatesAction({ action: 'active-hours', start: 9, end: 23 }));

  // Start-with-Windows task
  const exe = process.execPath;
  const as = await step('setAutoStart on', () => W.setAutoStart({ enabled: true, exe }));
  if (as && !as.enabled) fail('setAutoStart', 'task not registered');
  await step('setAutoStart off', () => W.setAutoStart({ enabled: false, exe }));

  // Locked-down script folder (same icacls as main.js hardenDataDir): scripts must still run from it.
  await step('hardened script dir', async () => {
    const { execFileSync } = require('child_process'); const path = require('path'); const fs = require('fs');
    const dir = path.join(process.env.ProgramData, 'OpenSweepHardenTest'), run = path.join(dir, 'run');
    fs.mkdirSync(run, { recursive: true });
    execFileSync('icacls.exe', [dir, '/inheritance:r', '/grant:r', '*S-1-5-18:(OI)(CI)F', '*S-1-5-32-544:(OI)(CI)F', '*S-1-5-32-545:(OI)(CI)RX', '/T', '/C', '/Q']);
    execFileSync('icacls.exe', [dir, '/setowner', '*S-1-5-32-544', '/T', '/C', '/Q']);
    require('../src/backend/pscore').setScriptDir(run);
    const r = await W._ps('Out-Json @{ ok = $true }');
    const e = await W.asSystem('Out-Result @{ ok = $true }');
    require('../src/backend/pscore').setScriptDir(null);
    if (!r || !r.ok || !e || !e.ok) throw new Error('scripts did not run from the hardened folder');
    return { acl: execFileSync('icacls.exe', [dir]).toString().split(/\r?\n/).slice(0, 4).join(' | ') };
  });

  // ---------------- v0.3 features
  await step('isAdmin', () => W.isAdmin());
  const sys = await step('asSystem whoami', () => W.asSystem('Out-Result @{ who = [Security.Principal.WindowsIdentity]::GetCurrent().Name }'));
  if (sys && !/SYSTEM/i.test(sys.who || '')) fail('asSystem', `ran as ${sys && sys.who}`);
  const rel = await step('reliability', () => W.reliability({ days: 14 }));
  if (rel && rel.days.length !== 14) fail('reliability', 'wrong number of days');
  // live process stream: wait for 2 samples
  await step('process stream', () => new Promise((res, rej) => { let n = 0; let errText = ''; const stop = W.startProcessStream(d => { if (++n === 2) { stop(); res({ procs: d.procs.length }); } }, (code, e) => { errText = `exit ${code}: ${e}`; }); setTimeout(() => { stop(); rej(new Error('no data from stream. ' + errText.slice(-1500))); }, 45000); }));
  await step('oldDrivers', () => W.oldDrivers());
  await step('windowsOldSize', () => W.windowsOldSize());
  await step('emptyFolders', () => W.emptyFolders(require('os').homedir()));
  const progs = await step('programs', () => W.programs());
  await step('browserExtensions', () => W.browserExtensions());
  await step('optionalFeatures', () => W.optionalFeatures());
  await step('wingetUpgrades', () => W.wingetUpgrades());
  await step('orphanScan', () => W.orphanScan(progs || []));
  // Fake app: folder + registry + Run entry + scheduled task; leftovers must find and remove them.
  const fs = require('fs'), path = require('path');
  const fakeDir = path.join(process.env.ProgramData, 'OpenSweepFakeApp');
  fs.mkdirSync(fakeDir, { recursive: true }); fs.writeFileSync(path.join(fakeDir, 'fake.exe'), 'x'.repeat(5000));
  await W._ps(`New-Item -Path 'HKCU:\\Software\\OpenSweepFakeApp' -Force | Out-Null; if (-not (Test-Path 'HKCU:\\Software\\Microsoft\\Windows\\CurrentVersion\\Run')) { New-Item -Path 'HKCU:\\Software\\Microsoft\\Windows\\CurrentVersion\\Run' | Out-Null }; New-ItemProperty -Path 'HKCU:\\Software\\Microsoft\\Windows\\CurrentVersion\\Run' -Name 'OpenSweepFakeApp' -Value ('"' + $a + '\\fake.exe"') -Force | Out-Null
Register-ScheduledTask -TaskName 'OpenSweepFakeAppUpdate' -Action (New-ScheduledTaskAction -Execute ($a + '\\fake.exe')) -Trigger (New-ScheduledTaskTrigger -Daily -At 3am) -Force | Out-Null; Out-Json @{ ok=$true }`, fakeDir);
  const left = await step('leftoverScan (fake app)', () => W.leftoverScan({ name: 'OpenSweepFakeApp', publisher: 'Nobody Inc', location: fakeDir, regKey: null }));
  if (left) {
    for (const want of ['dir', 'reg', 'run', 'task']) if (!left.some(i => i.type === want)) fail('leftoverScan', `did not find the ${want} leftover`);
    if (left.length < 4) { console.log('  found:', JSON.stringify(left.map(i => [i.type, i.path]))); console.log('  Run key:', JSON.stringify(await W._ps("Out-Json (Get-ItemProperty 'HKCU:\\Software\\Microsoft\\Windows\\CurrentVersion\\Run' -ErrorAction SilentlyContinue)"))); }
    const nonFile = left.filter(i => i.type !== 'dir' && i.type !== 'file');
    const rm = await step('removeLeftovers', () => W.removeLeftovers(nonFile, 'FakeApp'));
    if (rm) { for (const r of rm.results.filter(x => !x.ok)) fail('removeLeftovers', `${r.id}: ${r.error}`);
      const again = await W.leftoverScan({ name: 'OpenSweepFakeApp', publisher: '', location: null, regKey: null });
      if (again.some(i => i.type !== 'dir')) fail('removeLeftovers', 'items still present after removal');
      await step('restoreBackup', () => W.restoreBackup(rm.backup));
      const back = await W._ps("Out-Json (Test-Path 'HKCU:\\Software\\OpenSweepFakeApp')");
      if (!back) fail('restoreBackup', 'registry key was not restored'); }
    fs.rmSync(fakeDir, { recursive: true, force: true });
    await W._ps("Remove-Item 'HKCU:\\Software\\OpenSweepFakeApp' -Recurse -Force -ErrorAction SilentlyContinue; Remove-ItemProperty 'HKCU:\\Software\\Microsoft\\Windows\\CurrentVersion\\Run' -Name OpenSweepFakeApp -ErrorAction SilentlyContinue; Unregister-ScheduledTask -TaskName OpenSweepFakeAppUpdate -Confirm:$false -ErrorAction SilentlyContinue; Out-Json 1");
  }
  // Install tracker: a folder created between start and finish must show up.
  const t = await step('trackStart', () => W.trackStart('Smoke test'));
  if (t) {
    const trackDir = path.join(process.env.LOCALAPPDATA, 'OpenSweepTrackTest'); fs.mkdirSync(trackDir, { recursive: true });
    const done = await step('trackFinish', () => W.trackFinish(t.id));
    if (done && !done.diff.dirs.some(d => d.toLowerCase() === trackDir.toLowerCase())) fail('install tracker', 'new folder not detected');
    fs.rmSync(trackDir, { recursive: true, force: true });
  }
  // Space analyzer worker on the user profile
  const disk = require('../src/backend/disk');
  const ds = await step('disk scan', () => disk.start(process.env.USERPROFILE, () => {}));
  if (ds && !(ds.files > 0)) fail('disk scan', 'no files found');
  await step('setCleanSchedule weekly', () => W.setCleanSchedule({ freq: 'weekly', exe: process.execPath }));
  await step('setCleanSchedule off', () => W.setCleanSchedule({ freq: 'off', exe: process.execPath }));

  // ---------------- 0.4.0-alpha.4: Hardware check (read-only; no fixes are run here)
  const HW = require('../src/backend/hardware');
  const hwr = await step('hwCheck', () => HW.hwCheck());
  if (hwr) {
    const checks = Array.isArray(hwr.checks) ? hwr.checks : [];
    for (const id of HW.CHECK_IDS) if (!checks.some(c => c.id === id)) fail('hwCheck', `check ${id} missing`);
    for (const c of checks) {
      for (const p of HW.validateCheck(c)) fail(`hwCheck ${c.id}`, p);
      console.log(`  ${({ ok: '✓', warn: '!', bad: '✗', unknown: '?' })[c.status] || c.status} ${c.id}: ${c.found} | expected ${c.expected}${c.status === 'unknown' ? ` | ${c.why}` : ''}`);
      for (const i of c.items || []) console.log(`      - ${i.label}: ${i.found}`);
    }
    console.log('  timings (ms):', JSON.stringify(hwr.ms));
  }

  console.log(failures ? `\n${failures} problem(s) found` : '\nAll good');
  process.exit(failures ? 1 : 0);
})().catch(e => { console.error(e); process.exit(1); });
