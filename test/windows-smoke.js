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

  console.log(failures ? `\n${failures} problem(s) found` : '\nAll good');
  process.exit(failures ? 1 : 0);
})().catch(e => { console.error(e); process.exit(1); });
