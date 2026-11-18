'use strict';
/* OpenSweep renderer — vanilla JS, no framework. Every page is a function that
   renders into #main. All system work goes through window.api.call(method, args). */

// ------------------------------------------------------------------ helpers
const $ = (s, r = document) => r.querySelector(s);
function h(tag, attrs, ...kids) {
  const el = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs || {})) {
    if (v === null || v === undefined || v === false) continue;
    if (k === 'class') el.className = v;
    else if (k.startsWith('on')) el.addEventListener(k.slice(2), v);
    else if (k === 'style' && typeof v === 'object') Object.assign(el.style, v);
    else if (k === 'checked' || k === 'disabled' || k === 'value' || k === 'selected') el[k] = v;
    else el.setAttribute(k, v === true ? '' : v);
  }
  for (const kid of kids.flat(Infinity)) if (kid !== null && kid !== undefined && kid !== false) el.append(kid instanceof Node ? kid : document.createTextNode(String(kid)));
  return el;
}
const fmt = (b) => { if (b === null || b === undefined || isNaN(b)) return '—'; const u = ['B', 'KB', 'MB', 'GB', 'TB']; let i = 0; b = Number(b); while (b >= 1024 && i < 4) { b /= 1024; i++; } return `${b.toFixed(i > 1 ? 1 : 0)} ${u[i]}`; };
const pct = (a, b) => (b ? Math.round((100 * a) / b) : 0);
const ago = (iso) => { if (!iso) return '—'; const d = (Date.now() - new Date(iso).getTime()) / 86400e3; return d < 1 ? 'today' : `${Math.floor(d)} day${d >= 2 ? 's' : ''} ago`; };

function toast(msg, err) { const t = h('div', { class: 'toast' + (err ? ' err' : '') }, msg); $('#toasts').append(t); setTimeout(() => t.remove(), err ? 7000 : 3800); }
async function call(method, args) {
  try { return await window.api.call(method, args); }
  catch (e) { toast(String(e.message || e).replace(/^Error invoking remote method 'api': (Error: )?/, ''), true); throw e; }
}
function confirmDlg(title, text, okLabel = 'Continue') {
  if (S.settings && S.settings.confirmActions === false) return Promise.resolve(true);
  const d = $('#dlg');
  $('#dlg-title').textContent = title; $('#dlg-text').textContent = text; $('#dlg-ok').textContent = okLabel;
  return new Promise(res => {
    const done = (v) => { d.close(); $('#dlg-ok').onclick = $('#dlg-cancel').onclick = null; res(v); };
    $('#dlg-ok').onclick = () => done(true); $('#dlg-cancel').onclick = () => done(false);
    d.oncancel = () => done(false);
    d.showModal();
  });
}
const loading = (msg = 'Loading…') => h('div', { class: 'loading' }, h('span', { class: 'spinner' }), msg);
const toggle = (checked, onchange, disabled) => h('label', { class: 'toggle' }, h('input', { type: 'checkbox', checked, disabled, onchange: (e) => onchange(e.target.checked, e.target) }), h('span'));
async function busy(btn, fn, label) {
  const old = btn.textContent; btn.disabled = true; btn.replaceChildren(h('span', { class: 'spinner' }), ' ', label || old);
  try { return await fn(); } finally { btn.disabled = false; btn.textContent = old; }
}
function pageHead(title, sub, ...actions) { return h('div', { class: 'page-head' }, h('div', {}, h('h1', {}, title), sub ? h('p', {}, sub) : null), h('div', { class: 'row' }, actions)); }
function tabs(names, active, onpick) { return h('div', { class: 'tabs' }, names.map(([id, label]) => h('button', { class: 'tab' + (id === active ? ' active' : ''), onclick: () => onpick(id) }, label))); }
const adminTag = (x) => (x ? h('span', { class: 'tag admin', title: 'Windows will ask for administrator permission' }, 'admin') : null);
const recTag = (x) => (x ? h('span', { class: 'tag rec' }, 'recommended') : null);
function meter(p, warnAt = 75, badAt = 90) { return h('div', { class: 'meter' + (p >= badAt ? ' bad' : p >= warnAt ? ' warn' : '') }, h('div', { style: { width: Math.min(100, p) + '%' } })); }

// ------------------------------------------------------------------ state + router
const S = { page: 'home', timers: [], C: null, info: null, settings: null, cache: {} };
const NAV = [
  ['Overview', [['home', '⌂', 'Home'], ['cleanup', '✧', 'Cleanup'], ['optimize', '⚡', 'Optimize'], ['debloat', '✂', 'Debloat & AI']]],
  ['Manage', [['startup', '⏻', 'Startup apps'], ['apps', '▦', 'Installed apps'], ['processes', '≋', 'Processes'], ['storage', '◔', 'Storage']]],
  ['Protect', [['security', '⛨', 'Security'], ['stability', '⚕', 'Drivers & stability'], ['updates', '⟳', 'Windows Update'], ['network', '⇅', 'Network']]],
  ['Tools', [['repair', '✚', 'Repair & power'], ['toolbox', '⚒', 'Toolbox'], ['system', 'ⓘ', 'System info'], ['history', '↺', 'History & undo'], ['settings', '⚙', 'Settings']]],
];
function renderNav() {
  const nav = $('#sidebar');
  nav.replaceChildren(
    h('div', { class: 'brand' }, h('div', { class: 'logo' }, '✦'), 'OpenSweep'),
    ...NAV.map(([label, items]) => h('div', { class: 'nav-group' }, h('div', { class: 'nav-label' }, label),
      items.map(([id, ico, name]) => h('button', { class: 'nav-item' + (S.page === id ? ' active' : ''), onclick: () => go(id) }, h('span', { class: 'ico' }, ico), name)))),
    h('div', { class: 'sidebar-foot' }, S.info.demo ? h('div', {}, h('span', { class: 'demo-badge' }, 'DEMO'), ' sample data — no changes are made') : null, h('div', {}, `v${S.info.version} · MIT licensed`)));
}
function go(page) {
  S.timers.forEach(clearInterval); S.timers = [];
  S.page = page; renderNav();
  const main = $('#main'); main.scrollTop = 0; main.replaceChildren(...banners());
  (PAGES[page] || PAGES.home)(main).catch(e => { console.error(e); main.append(h('div', { class: 'banner' }, 'Something went wrong: ' + e.message)); });
}

function banners() {
  const out = [];
  if (S.info && !S.info.demo && !S.info.isAdmin) out.push(h('div', { class: 'banner' }, 'OpenSweep is not running as administrator, so many changes cannot be applied. ',
    h('button', { class: 'btn small primary', onclick: () => call('relaunchAsAdmin') }, 'Restart as administrator')));
  if (S.update && S.update.state === 'ready') out.push(h('div', { class: 'banner good' }, `OpenSweep ${S.update.version} is ready. `,
    h('button', { class: 'btn small primary', onclick: () => call('installUpdate') }, 'Restart to update')));
  return out;
}

// ------------------------------------------------------------------ shared actions
const PRESETS = [
  { id: 'annoy', ico: '☺', t: 'Fix Windows annoyances', d: 'The most-complained-about things: slow folders, forced restarts, account nags, Sticky Keys, Edge tabs in Alt+Tab, Outlook reinstalling.', tweaks: (C) => C.TWEAKS.filter(t => C.ANNOYANCE_FIXES.includes(t.id)) },
  { id: 'recommended', ico: '★', t: 'Recommended', d: 'All recommended tweaks: no ads, no AI, less telemetry, faster UI. Safe for everyone.', tweaks: (C) => C.TWEAKS.filter(t => t.rec) },
  { id: 'ai', ico: '✦', t: 'AI-free Windows', d: 'Turns off Copilot, Recall, Click to Do and AI in Edge, Paint, Notepad and search.', tweaks: (C) => C.TWEAKS.filter(t => t.cat === 'ai') },
  { id: 'gaming', ico: '◆', t: 'Gaming', d: 'Game Mode, no background recording, GPU scheduling, raw mouse input, foreground priority.', tweaks: (C) => C.TWEAKS.filter(t => t.cat === 'gaming' || (t.cat === 'performance' && t.rec)) },
  { id: 'privacy', ico: '◉', t: 'Maximum privacy', d: 'Every privacy and ad tweak, including location, error reporting and app tracking.', tweaks: (C) => C.TWEAKS.filter(t => ['privacy', 'popups', 'ai'].includes(t.cat) && t.id !== 'privacy.onedrive') },
];
async function applyTweaks(list, label) {
  const states = await call('tweakStates');
  const changes = list.filter(t => !states[t.id]).map(t => ({ id: t.id, on: true }));
  if (!changes.length) { toast('Everything in this set is already applied.'); return false; }
  const admin = list.some(t => t.admin && !states[t.id]);
  if (!(await confirmDlg(label, `${changes.length} setting(s) will change.${admin ? '\nWindows will ask for administrator permission.' : ''}\nYou can undo this from History & undo.\nTip: create a restore point first (Repair & power).`, 'Apply'))) return false;
  const r = await call('setTweaks', { changes, label });
  reportTweaks(r, label);
  return true;
}
// Every apply is read back from Windows; show what really stuck.
function reportTweaks(r, label) {
  if (!r || !r.failed) return toast(`${label}: done`);
  if (!r.failed.length) return toast(`✓ ${label}: ${r.applied} setting(s) applied and verified`);
  toast(`${label}: ${r.applied} verified, ${r.failed.length} did not apply — ${r.failed.slice(0, 3).map(f => `${f.title} (${f.error})`).join('; ')}${r.failed.length > 3 ? '…' : ''}`, true);
}
function reportList(results, label, nameOf) {
  const bad = (results || []).filter(x => !x.ok);
  if (!bad.length) return toast(`✓ ${label}: ${(results || []).length} change(s) verified`);
  toast(`${label}: ${bad.length} did not apply — ${bad.slice(0, 3).map(x => `${nameOf(x)}${x.error ? ` (${x.error})` : ''}`).join('; ')}`, true);
}
async function runFix(action, btn) {
  const C = S.C;
  const run = async () => {
    if (action.go) return go(action.go);
    if (action.uri) return call('openUri', { uri: action.uri });
    const f = action.fix;
    if (f === 'boost') { const r = await call('boost'); toast(`Boost freed ${r.memFreedMB} MB of memory`); }
    else if (f === 'clean') {
      const ids = C.CLEANUP.filter(c => c.rec && !c.admin).map(c => c.id);
      const r = await call('cleanupRun', { ids, elevate: false });
      toast(`Cleaned ${fmt(r.reduce((a, x) => a + x.freed, 0))}`);
    } else if (f === 'defender-update') { await call('defenderAction', { action: 'update' }); toast('Virus definitions updated'); }
    else if (f === 'quick-scan') { toast('Quick scan started — this can take a few minutes'); await call('defenderAction', { action: 'quick' }); toast('Quick scan finished'); }
    else if (['firewall-on', 'smb1-off', 'rdp-off'].includes(f)) { await call('defenderAction', { action: f }); toast('Done'); }
    else if (f === 'preset:annoy') { await applyTweaks(PRESETS[0].tweaks(C), 'Fix Windows annoyances'); }
    else if (f && f.startsWith('preset:')) { const cat = f.slice(7); await applyTweaks(C.TWEAKS.filter(t => t.cat === cat && t.rec), C.TWEAK_CATEGORIES.find(c => c.id === cat).name); }
  };
  if (btn) await busy(btn, run); else await run();
}

// ------------------------------------------------------------------ pages
const PAGES = {};

PAGES.home = async (main) => {
  const ring = h('div');
  const issues = h('div', { class: 'list' });
  const checkBtn = h('button', { class: 'btn primary big' }, 'Run health check');
  const cpuEl = h('div', { class: 'stat' }, '–'), memEl = h('div', { class: 'stat' }, '–');
  const cpuM = h('div'), memM = h('div'), memSub = h('div', { class: 'sub' });
  const boostBtn = h('button', { class: 'btn primary' }, '⚡ Boost');

  function drawRing(score) {
    const r = 54, c = 2 * Math.PI * r, col = score >= 80 ? 'var(--good)' : score >= 55 ? 'var(--warn)' : 'var(--bad)';
    ring.innerHTML = `<svg class="ring" viewBox="0 0 132 132"><circle cx="66" cy="66" r="${r}" fill="none" stroke="var(--panel-2)" stroke-width="12"/>
      <circle cx="66" cy="66" r="${r}" fill="none" stroke="${score === null ? 'var(--border)' : col}" stroke-width="12" stroke-linecap="round" stroke-dasharray="${c}" stroke-dashoffset="${score === null ? c : c * (1 - score / 100)}" transform="rotate(-90 66 66)"/>
      <text x="66" y="70" text-anchor="middle" class="val">${score === null ? '—' : score}</text><text x="66" y="90" text-anchor="middle" class="lbl">HEALTH</text></svg>`;
  }
  drawRing(S.cache.health ? S.cache.health.score : null);

  async function check() {
    issues.replaceChildren(loading('Checking disk space, junk files, startup apps, security, AI & ad settings…'));
    const r = await busy(checkBtn, () => call('health'), 'Checking…');
    S.cache.health = r; drawRing(r.score); drawIssues(r);
  }
  function drawIssues(r) {
    if (!r.issues.length) return issues.replaceChildren(h('div', { class: 'empty' }, '✓ Everything looks great.'));
    issues.replaceChildren(...r.issues.map(i => h('div', { class: 'item' }, h('span', { class: 'sev ' + i.sev }),
      h('div', { class: 'body' }, h('div', { class: 'title' }, i.title), i.detail ? h('div', { class: 'desc' }, i.detail) : null),
      i.action ? h('button', { class: 'btn small', onclick: async (e) => { await runFix(i.action, e.currentTarget); if (i.action.fix) check(); } }, i.action.label || 'Open') : null)));
  }
  checkBtn.onclick = check;
  boostBtn.onclick = () => busy(boostBtn, async () => { const r = await call('boost'); toast(`Boost freed ${r.memFreedMB} MB of memory and ${fmt(r.tempFreed)} of temp files`); }, 'Boosting…');

  const tick = async () => {
    const s = await window.api.call('stats');
    cpuEl.replaceChildren(`${s.cpu}`, h('small', {}, ' %')); cpuM.replaceChildren(meter(s.cpu));
    memEl.replaceChildren(`${s.memPct}`, h('small', {}, ' %')); memM.replaceChildren(meter(s.memPct));
    memSub.textContent = `${fmt(s.memUsed)} of ${fmt(s.memTotal)} in use`;
  };
  tick(); S.timers.push(setInterval(tick, 2000));

  const tiles = [
    ['cleanup', '✧', 'Deep cleanup', 'Temp files, caches, browsers, apps'],
    ['debloat', '✦', 'Remove Microsoft AI', 'Copilot, Recall & preinstalled apps'],
    ['optimize', '⚡', 'Optimize Windows', 'Presets & ~100 tweaks'],
    ['startup', '⏻', 'Startup apps', 'Boot faster'],
    ['storage', '◔', 'Find large files', 'Large files & duplicates'],
    ['security', '⛨', 'Security scan', 'Defender, firewall, updates'],
    ['updates', '⟳', 'Control updates', 'No forced restarts, pause, roll back'],
    ['repair', '✚', 'Repair Windows', 'SFC, DISM, update reset'],
  ];

  main.append(
    pageHead('Home', S.info.demo ? 'Demo mode: showing sample data from a Windows 11 PC.' : 'Keep your PC clean, fast and private.'),
    h('div', { class: 'grid g3' },
      h('div', { class: 'card', style: { gridColumn: 'span 2' } },
        h('div', { class: 'score-wrap' }, ring, h('div', {}, h('h2', {}, 'PC health'), h('div', { class: 'sub', style: { marginBottom: '12px' } }, S.cache.health ? `Last checked ${new Date(S.cache.health.checkedAt).toLocaleTimeString()}` : 'One click checks storage, junk, startup load, security, AI/ad settings and memory.'), checkBtn))),
      h('div', { class: 'card' }, h('div', { class: 'row' }, h('h3', {}, 'Memory'), h('div', { class: 'spacer' }), boostBtn), memEl, memM, memSub,
        h('div', { style: { marginTop: '12px' } }, h('h3', {}, 'CPU'), cpuEl, cpuM))),
    h('div', { class: 'section-title' }, 'Issues'), h('div', { class: 'card' }, issues),
    h('div', { class: 'section-title' }, 'Quick actions'),
    h('div', { class: 'grid g4' }, tiles.map(([id, ico, t, d]) => h('button', { class: 'tile', onclick: () => go(id) }, h('span', { class: 'ico' }, ico), h('span', { class: 't' }, t), h('span', { class: 'd' }, d)))),
  );
  if (S.cache.health) drawIssues(S.cache.health); else issues.replaceChildren(h('div', { class: 'empty' }, 'Run a health check to find issues.'));
};

PAGES.cleanup = async (main) => {
  const C = S.C;
  const sel = new Set(C.CLEANUP.filter(c => c.rec).map(c => c.id));
  let sizes = S.cache.cleanup || null;
  const body = h('div');
  const total = h('div', { class: 'stat' });
  const elevate = h('input', { type: 'checkbox', checked: true });
  const scanBtn = h('button', { class: 'btn' }, 'Scan');
  const cleanBtn = h('button', { class: 'btn primary' }, 'Clean selected');

  function draw() {
    const groups = [...new Set(C.CLEANUP.map(c => c.group))];
    const sum = C.CLEANUP.filter(c => sel.has(c.id)).reduce((a, c) => a + ((sizes && sizes[c.id] && sizes[c.id].size) || 0), 0);
    total.replaceChildren(sizes ? fmt(sum) : '—', h('small', {}, sizes ? ' selected to clean' : ' scan to see how much you can free'));
    cleanBtn.disabled = !sizes || !sel.size;
    body.replaceChildren(...groups.map(g => h('div', { class: 'card' }, h('h3', {}, g), h('div', { class: 'list' },
      C.CLEANUP.filter(c => c.group === g).map(c => h('label', { class: 'item' },
        h('input', { type: 'checkbox', checked: sel.has(c.id), onchange: (e) => { e.target.checked ? sel.add(c.id) : sel.delete(c.id); draw(); } }),
        h('div', { class: 'body' }, h('div', { class: 'title' }, c.name, adminTag(c.admin)), h('div', { class: 'desc' }, c.desc)),
        h('div', { class: 'meta' }, sizes ? (sizes[c.id] ? `${fmt(sizes[c.id].size)} · ${sizes[c.id].files.toLocaleString()} files` : '—') : '')))))));
  }
  async function scan() {
    body.replaceChildren(loading('Scanning temp folders, caches and browsers…'));
    const r = await busy(scanBtn, () => call('cleanupScan'), 'Scanning…');
    sizes = Object.fromEntries(r.map(x => [x.id, x])); S.cache.cleanup = sizes; draw();
  }
  scanBtn.onclick = scan;
  cleanBtn.onclick = async () => {
    const ids = [...sel];
    if (!(await confirmDlg('Clean selected items?', `${ids.length} categories, about ${total.firstChild.textContent}.\nFiles in use are skipped automatically. Close browsers first for a better result.`, 'Clean'))) return;
    const r = await busy(cleanBtn, () => call('cleanupRun', { ids, elevate: elevate.checked }), 'Cleaning…');
    const freed = r.reduce((a, x) => a + x.freed, 0), skipped = r.reduce((a, x) => a + x.skipped, 0);
    toast(`Freed ${fmt(freed)}${skipped ? ` · ${skipped} in-use files skipped` : ''}`);
    await scan();
  };

  main.append(pageHead('Cleanup', 'Remove temporary files, caches and leftovers from Windows, browsers, apps and developer tools.', scanBtn, cleanBtn),
    h('div', { class: 'card' }, h('div', { class: 'row' }, total, h('div', { class: 'spacer' }),
      h('label', { class: 'row', style: { gap: '6px' } }, elevate, 'Include items that need admin permission'),
      h('button', { class: 'btn small', onclick: () => { C.CLEANUP.forEach(c => sel.add(c.id)); draw(); } }, 'Select all'),
      h('button', { class: 'btn small', onclick: () => { sel.clear(); C.CLEANUP.filter(c => c.rec).forEach(c => sel.add(c.id)); draw(); } }, 'Recommended'))),
    h('div', { style: { height: '14px' } }), body);
  draw();
  if (!sizes) scan();
};

PAGES.optimize = async (main) => {
  const C = S.C;
  let cat = S.cache.optCat || 'ai';
  let states = await call('tweakStates');
  const wrap = h('div');
  function draw() {
    const catInfo = C.TWEAK_CATEGORIES.find(c => c.id === cat);
    const list = C.TWEAKS.filter(t => t.cat === cat);
    const onCount = list.filter(t => states[t.id]).length;
    wrap.replaceChildren(
      tabs(C.TWEAK_CATEGORIES.map(c => [c.id, `${c.icon} ${c.name}`]), cat, (id) => { cat = S.cache.optCat = id; draw(); }),
      h('div', { class: 'card' },
        h('div', { class: 'row', style: { marginBottom: '6px' } }, h('div', {}, h('h2', {}, catInfo.name), h('div', { class: 'sub' }, catInfo.blurb, ` · ${onCount}/${list.length} applied`)), h('div', { class: 'spacer' }),
          h('button', { class: 'btn small', onclick: async (e) => { if (await busy(e.currentTarget, () => applyTweaks(list.filter(t => t.rec), catInfo.name + ' (recommended)'))) { states = await call('tweakStates'); draw(); } } }, 'Apply recommended'),
          h('button', { class: 'btn small', onclick: async (e) => { if (await busy(e.currentTarget, () => applyTweaks(list, catInfo.name + ' (all)'))) { states = await call('tweakStates'); draw(); } } }, 'Apply all')),
        h('div', { class: 'list' }, list.map(t => h('div', { class: 'item' },
          h('div', { class: 'body' }, h('div', { class: 'title' }, t.title, recTag(t.rec), adminTag(t.admin)), t.desc ? h('div', { class: 'desc' }, t.desc) : null),
          toggle(!!states[t.id], async (on, input) => {
            input.disabled = true;
            try { const r = await call('setTweaks', { changes: [{ id: t.id, on }], label: `${on ? 'Applied' : 'Reverted'}: ${t.title}` }); if (r && r.states) states = r.states; else states[t.id] = on; reportTweaks(r, `${on ? 'Applied' : 'Reverted'}: ${t.title}`); }
            catch { input.checked = !on; }
            finally { input.disabled = false; draw(); }
          }))))));
  }
  main.append(pageHead('Optimize', 'One-click presets or fine-grained control. Every change can be undone from History & undo.',
    h('button', { class: 'btn', onclick: () => go('repair') }, 'Create restore point first')),
    h('div', { class: 'grid', style: { gridTemplateColumns: 'repeat(auto-fit, minmax(190px, 1fr))' } }, PRESETS.map(p => h('div', { class: 'preset' }, h('div', { class: 'ico' }, p.ico), h('div', { class: 't' }, p.t), h('div', { class: 'd' }, p.d),
      h('button', { class: 'btn primary small', onclick: async (e) => { if (await busy(e.currentTarget, () => applyTweaks(p.tweaks(C), `Preset: ${p.t}`))) { states = await call('tweakStates'); draw(); } } }, 'Apply')))),
    h('div', { style: { height: '18px' } }), wrap);
  draw();
};

PAGES.debloat = async (main) => {
  const C = S.C;
  let tab = S.cache.debloatTab || 'ai';
  const wrap = h('div');

  async function drawAI() {
    const states = await call('tweakStates');
    const installed = new Set(await call('bloatStatus'));
    const aiTweaks = C.TWEAKS.filter(t => t.cat === 'ai');
    const aiApps = C.BLOAT_APPS.filter(a => a.group === 'AI' && installed.has(a.pkg));
    const allOn = aiTweaks.every(t => states[t.id]);
    const btn = h('button', { class: 'btn primary big', disabled: allOn && !aiApps.length }, allOn && !aiApps.length ? '✓ Microsoft AI is removed' : 'Remove all Microsoft AI');
    btn.onclick = () => busy(btn, async () => {
      const changes = aiTweaks.filter(t => !states[t.id]).map(t => ({ id: t.id, on: true }));
      if (!(await confirmDlg('Remove Microsoft AI?', `This turns off ${changes.length} AI feature(s) via official Windows policies${aiApps.length ? ` and uninstalls ${aiApps.length} AI app(s) (${aiApps.map(a => a.name).join(', ')})` : ''}.\nWindows will ask for administrator permission. Policies can be undone from History & undo.`, 'Remove'))) return;
      if (changes.length) reportTweaks(await call('setTweaks', { changes, label: 'Remove Microsoft AI' }), 'AI settings');
      if (aiApps.length) reportList((await call('removeApps', { pkgs: aiApps.map(a => a.pkg), allUsers: false })).map(x => ({ ...x, name: x.pkg })), 'AI apps', x => x.name);
      toast('Sign out or restart to finish removing Microsoft AI.');
      draw();
    }, 'Removing…');
    return [h('div', { class: 'card' }, h('div', { class: 'row' }, h('div', { style: { flex: 1 } }, h('h2', {}, 'AI-free Windows'), h('div', { class: 'sub' }, 'Copilot, Recall, Click to Do, generative AI access, and AI in Edge, Paint, Notepad and Search — switched off with documented Windows policies, plus the AI apps uninstalled.')), btn)),
      h('div', { class: 'card' }, h('h3', {}, 'What gets changed'), h('div', { class: 'list' },
        aiTweaks.map(t => h('div', { class: 'item' }, h('span', { class: 'tag ' + (states[t.id] ? 'good' : 'warn') }, states[t.id] ? 'off ✓' : 'active'), h('div', { class: 'body' }, h('div', { class: 'title' }, t.title, adminTag(t.admin)), h('div', { class: 'desc' }, t.desc)))),
        C.BLOAT_APPS.filter(a => a.group === 'AI').map(a => h('div', { class: 'item' }, h('span', { class: 'tag ' + (installed.has(a.pkg) ? 'warn' : 'good') }, installed.has(a.pkg) ? 'installed' : 'removed ✓'), h('div', { class: 'body' }, h('div', { class: 'title' }, `App: ${a.name}`), h('div', { class: 'desc mono' }, a.pkg))))))];
  }

  async function drawApps() {
    const installed = new Set(await call('bloatStatus'));
    const present = C.BLOAT_APPS.filter(a => installed.has(a.pkg));
    const sel = new Set(present.filter(a => a.rec).map(a => a.pkg));
    const allUsers = h('input', { type: 'checkbox' });
    const btn = h('button', { class: 'btn primary' }, 'Remove selected');
    btn.onclick = () => busy(btn, async () => {
      if (!sel.size) return;
      if (!(await confirmDlg('Remove apps?', `${sel.size} app(s) will be uninstalled${allUsers.checked ? ' for all users and new accounts (needs admin)' : ''}.\nMost can be reinstalled from the Microsoft Store.`, 'Remove'))) return;
      const r = await call('removeApps', { pkgs: [...sel], allUsers: allUsers.checked });
      const ok = (r || []).filter(x => x.ok).length;
      toast(`Removed ${ok} app(s)${ok < sel.size ? ` · ${sel.size - ok} could not be removed` : ''}`);
      draw();
    }, 'Removing…');
    const groups = [...new Set(C.BLOAT_APPS.map(a => a.group))];
    return [h('div', { class: 'card' }, h('div', { class: 'row' }, h('div', { class: 'sub', style: { flex: 1 } }, `${present.length} removable preinstalled apps found. Recommended ones are pre-selected.`), h('label', { class: 'row', style: { gap: '6px' } }, allUsers, 'Also for all users & new accounts'), btn)),
      ...groups.map(g => { const apps = present.filter(a => a.group === g); return apps.length ? h('div', { class: 'card' }, h('h3', {}, g), h('div', { class: 'list' }, apps.map(a => h('label', { class: 'item' },
        h('input', { type: 'checkbox', checked: sel.has(a.pkg), onchange: (e) => (e.target.checked ? sel.add(a.pkg) : sel.delete(a.pkg)) }),
        h('div', { class: 'body' }, h('div', { class: 'title' }, a.name, recTag(a.rec)), h('div', { class: 'desc mono' }, a.pkg)))))) : null; }),
      present.length ? null : h('div', { class: 'card empty' }, '✓ No known bloatware installed.')];
  }

  async function drawServices() {
    const st = Object.fromEntries((await call('servicesStatus')).map(s => [s.name, s]));
    const pending = {};
    const apply = async (changes, label) => { if (!changes.length) return toast('Nothing to change'); if (!(await confirmDlg(label, `${changes.length} service(s) will change. Windows will ask for administrator permission.`, 'Apply'))) return; reportList(await call('setServices', { changes }), label, x => (C.SERVICES.find(s => s.name === x.name) || {}).title || x.name); draw(); };
    const svc = C.SERVICES.filter(s => st[s.name] && st[s.name].status !== 'Missing');
    return [h('div', { class: 'card' }, h('div', { class: 'row' }, h('div', { class: 'sub', style: { flex: 1 } }, 'Background services that collect telemetry or are rarely needed. Change start type per service, or use a preset.'),
      h('button', { class: 'btn small', onclick: () => apply(svc.filter(s => st[s.name].startType !== s.rec).map(s => ({ name: s.name, type: s.rec })), 'Apply recommended') }, 'Apply recommended'),
      h('button', { class: 'btn small', onclick: () => apply(svc.filter(s => st[s.name].startType !== s.orig).map(s => ({ name: s.name, type: s.orig })), 'Restore Windows defaults') }, 'Restore defaults'),
      h('button', { class: 'btn primary small', onclick: () => apply(Object.entries(pending).map(([name, type]) => ({ name, type })), 'Apply changes') }, 'Apply changes'))),
      h('div', { class: 'card' }, h('table', {}, h('thead', {}, h('tr', {}, h('th', {}, 'Service'), h('th', {}, 'Status'), h('th', {}, 'Recommended'), h('th', {}, 'Start type'))),
        h('tbody', {}, svc.map(s => h('tr', {}, h('td', {}, h('div', { class: 'title' }, s.title), h('div', { class: 'desc' }, s.desc, ' ', h('span', { class: 'mono' }, s.name))),
          h('td', {}, st[s.name].status), h('td', {}, s.rec),
          h('td', {}, h('select', { onchange: (e) => { pending[s.name] = e.target.value; } }, ['Automatic', 'Manual', 'Disabled'].map(t => h('option', { value: t, selected: st[s.name].startType === t }, t)))))))))];
  }

  async function drawTasks() {
    const st = Object.fromEntries((await call('tasksStatus')).map(t => [t.id, t.state]));
    const tasks = C.SCHEDULED_TASKS.filter(t => st[t.path + t.name] !== 'Missing');
    const enabledOnes = tasks.filter(t => st[t.path + t.name] !== 'Disabled');
    return [h('div', { class: 'card' }, h('div', { class: 'row' }, h('div', { class: 'sub', style: { flex: 1 } }, 'Scheduled tasks that collect and upload telemetry in the background.'),
      h('button', { class: 'btn primary small', disabled: !enabledOnes.length, onclick: async (e) => { if (!(await confirmDlg('Disable telemetry tasks?', `${enabledOnes.length} task(s) will be disabled. Needs admin permission.`, 'Disable'))) return; const r = await busy(e.currentTarget, () => call('setTasks', { changes: enabledOnes.map(t => ({ id: t.path + t.name, enable: false })) })); reportList(r.results, 'Telemetry tasks', x => x.id.split('\\').pop()); draw(); } }, 'Disable all'))),
      h('div', { class: 'card' }, h('div', { class: 'list' }, tasks.map(t => h('div', { class: 'item' }, h('div', { class: 'body' }, h('div', { class: 'title' }, t.name), h('div', { class: 'desc' }, t.desc, ' ', h('span', { class: 'mono' }, t.path))),
        h('span', { class: 'meta' }, st[t.path + t.name] === 'Disabled' ? 'Disabled' : 'Enabled'),
        toggle(st[t.path + t.name] !== 'Disabled', async (on, input) => { input.disabled = true; try { const r = await call('setTasks', { changes: [{ id: t.path + t.name, enable: on }] }); reportList(r.results, t.name, x => x.id.split('\\').pop()); } catch { input.checked = !on; } draw(); })))))];
  }

  async function draw() {
    wrap.replaceChildren(tabs([['ai', '✦ Microsoft AI'], ['apps', 'Preinstalled apps'], ['services', 'Services'], ['tasks', 'Telemetry tasks']], tab, (id) => { tab = S.cache.debloatTab = id; draw(); }), loading());
    const parts = await ({ ai: drawAI, apps: drawApps, services: drawServices, tasks: drawTasks }[tab])();
    wrap.lastChild.replaceWith(h('div', {}, parts));
  }
  main.append(pageHead('Debloat & AI', 'Strip out Microsoft AI, preinstalled apps, telemetry services and background tasks that slow Windows down.'), wrap);
  draw();
};

PAGES.startup = async (main) => {
  const list = h('div', { class: 'list' }, loading());
  const count = h('div', { class: 'sub' });
  async function load() {
    const items = await call('startupItems');
    const on = items.filter(i => i.enabled).length;
    count.textContent = `${on} of ${items.length} enabled. Disabling only stops auto-start — the app is not removed.`;
    list.replaceChildren(...items.sort((a, b) => b.enabled - a.enabled || a.name.localeCompare(b.name)).map(i => h('div', { class: 'item' },
      h('div', { class: 'body' }, h('div', { class: 'title' }, i.name, h('span', { class: 'tag' }, { HKCU: 'User', HKLM: 'All users', HKLM32: 'All users (32-bit)', UserFolder: 'Startup folder', CommonFolder: 'Startup folder (all)', Task: 'Scheduled task' }[i.source] || i.source), i.impact ? h('span', { class: 'tag ' + (i.impact === 'High' ? 'bad' : '') }, `${i.impact} impact`) : null, adminTag(i.admin)),
        h('div', { class: 'desc mono trunc', title: i.command }, i.command)),
      toggle(i.enabled, async (en, input) => { input.disabled = true; try { await call('setStartup', { id: i.id, enabled: en }); toast(`${i.name} ${en ? 'will' : "won't"} start with Windows`); } catch { input.checked = !en; } load(); }))));
  }
  main.append(pageHead('Startup apps', 'Apps, registry entries, startup folders and scheduled tasks that launch when you sign in.', h('button', { class: 'btn', onclick: load }, 'Refresh')), h('div', { class: 'card' }, count, list));
  load();
};

PAGES.apps = async (main) => {
  const q = h('input', { type: 'text', placeholder: 'Search apps…', oninput: () => draw() });
  const kind = h('select', { onchange: () => draw() }, h('option', { value: 'all' }, 'All apps'), h('option', { value: 'desktop' }, 'Desktop apps'), h('option', { value: 'store' }, 'Store apps'));
  const tbody = h('tbody');
  let apps = [];
  let sort = 'name';
  function draw() {
    const term = q.value.toLowerCase();
    const rows = apps.filter(a => (kind.value === 'all' || a.kind === kind.value) && (`${a.name} ${a.publisher}`.toLowerCase().includes(term)))
      .sort((a, b) => (sort === 'size' ? (b.sizeKB || 0) - (a.sizeKB || 0) : a.name.localeCompare(b.name)));
    tbody.replaceChildren(...rows.map(a => h('tr', {}, h('td', {}, h('div', { class: 'title' }, a.name), h('div', { class: 'desc' }, a.publisher || '')), h('td', {}, a.version || ''), h('td', {}, a.kind === 'store' ? 'Store' : 'Desktop'),
      h('td', { class: 'num' }, a.sizeKB ? fmt(a.sizeKB * 1024) : '—'),
      h('td', { class: 'num' }, h('button', { class: 'btn small danger', onclick: async (e) => { if (!(await confirmDlg(`Uninstall ${a.name}?`, a.kind === 'store' ? 'The app will be removed for your account.' : "The app's own uninstaller will open.", 'Uninstall'))) return; await busy(e.currentTarget, () => call('uninstallApp', { id: a.id })); toast(a.kind === 'store' ? `${a.name} removed` : `Uninstaller for ${a.name} opened`); if (a.kind === 'store') load(); } }, 'Uninstall')))));
    if (!rows.length) tbody.replaceChildren(h('tr', {}, h('td', { colspan: 5, class: 'empty' }, 'No apps match.')));
  }
  async function load() { tbody.replaceChildren(h('tr', {}, h('td', { colspan: 5 }, loading()))); apps = await call('installedApps'); draw(); }
  main.append(pageHead('Installed apps', 'Desktop and Microsoft Store apps. Sort by size to find space hogs.', q, kind),
    h('div', { class: 'card' }, h('table', {}, h('thead', {}, h('tr', {}, h('th', { onclick: () => { sort = 'name'; draw(); } }, 'Name ↕'), h('th', {}, 'Version'), h('th', {}, 'Type'), h('th', { class: 'num', onclick: () => { sort = 'size'; draw(); } }, 'Size ↕'), h('th'))), tbody)));
  load();
};

PAGES.processes = async (main) => {
  const C = S.C;
  const q = h('input', { type: 'text', placeholder: 'Filter…', oninput: () => draw() });
  const tbody = h('tbody');
  let procs = [], sort = 'mem';
  const auto = h('input', { type: 'checkbox', checked: true });
  function draw() {
    const groups = {};
    for (const p of procs) { const g = groups[p.name] || (groups[p.name] = { name: p.name, company: p.company, pids: [], mem: 0, cpu: 0 }); g.pids.push(p.pid); g.mem += p.memMB; g.cpu += p.cpu; }
    const rows = Object.values(groups).filter(g => g.name.toLowerCase().includes(q.value.toLowerCase())).sort((a, b) => (sort === 'cpu' ? b.cpu - a.cpu : sort === 'name' ? a.name.localeCompare(b.name) : b.mem - a.mem)).slice(0, 150);
    tbody.replaceChildren(...rows.map(g => {
      const prot = C.PROTECTED_PROCESSES.includes(g.name.toLowerCase());
      return h('tr', {}, h('td', {}, h('div', { class: 'title' }, g.name, g.pids.length > 1 ? h('span', { class: 'tag' }, `×${g.pids.length}`) : null, prot ? h('span', { class: 'tag' }, 'system') : null), h('div', { class: 'desc' }, g.company || '')),
        h('td', { class: 'num' }, `${g.cpu.toFixed(1)} %`), h('td', { class: 'num' }, `${Math.round(g.mem).toLocaleString()} MB`),
        h('td', { class: 'num' }, prot ? null : h('button', { class: 'btn small danger', onclick: async (e) => {
          if (!(await confirmDlg(`End ${g.name}?`, `${g.pids.length} process(es) will be closed. Unsaved work in that app will be lost.`, 'End task'))) return;
          await busy(e.currentTarget, async () => { for (const pid of g.pids) await call('killProcess', { pid, name: g.name }).catch(() => {}); });
          toast(`Ended ${g.name}`); load();
        } }, 'End task')));
    }));
  }
  async function load() { procs = await window.api.call('processes'); draw(); }
  main.append(pageHead('Processes', 'Everything running right now, grouped by app. Critical Windows processes are protected.', q, h('label', { class: 'row', style: { gap: '6px' } }, auto, 'Live')),
    h('div', { class: 'card' }, h('table', {}, h('thead', {}, h('tr', {}, h('th', { onclick: () => { sort = 'name'; draw(); } }, 'Name ↕'), h('th', { class: 'num', onclick: () => { sort = 'cpu'; draw(); } }, 'CPU ↕'), h('th', { class: 'num', onclick: () => { sort = 'mem'; draw(); } }, 'Memory ↕'), h('th'))), tbody)));
  tbody.replaceChildren(h('tr', {}, h('td', { colspan: 4 }, loading())));
  await load();
  S.timers.push(setInterval(() => { if (auto.checked && document.activeElement !== q) load(); }, 3000));
};

PAGES.storage = async (main) => {
  let tab = S.cache.storageTab || 'large';
  const root = h('input', { type: 'text', value: S.settings.scanRoot || S.info.home });
  const disks = h('div', { class: 'grid g3' }, loading());
  const wrap = h('div');
  call('systemInfo').then(si => disks.replaceChildren(...si.disks.map(d => { const used = d.total - d.free, p = pct(used, d.total); return h('div', { class: 'card' }, h('div', { class: 'row' }, h('h3', {}, `${d.name} ${d.label || ''}`), h('div', { class: 'spacer' }), h('span', { class: 'sub' }, `${p}%`)), meter(p, 80, 90), h('div', { class: 'sub', style: { marginTop: '6px' } }, `${fmt(d.free)} free of ${fmt(d.total)}`)); })));

  function fileTable(files, sel) {
    return h('table', {}, h('thead', {}, h('tr', {}, h('th'), h('th', {}, 'File'), h('th', { class: 'num' }, 'Size'), h('th', {}, 'Modified'), h('th'))),
      h('tbody', {}, files.map(f => h('tr', {}, h('td', {}, h('input', { type: 'checkbox', checked: sel.has(f.path), onchange: (e) => (e.target.checked ? sel.add(f.path) : sel.delete(f.path)) })),
        h('td', {}, h('div', { class: 'mono trunc', style: { maxWidth: '560px' }, title: f.path }, f.path)), h('td', { class: 'num' }, f.size !== undefined ? fmt(f.size) : ''), h('td', {}, (f.modified || '').slice(0, 10)),
        h('td', {}, h('button', { class: 'btn small', onclick: () => call('revealFile', { path: f.path }) }, 'Show'))))));
  }
  async function trash(sel, after) {
    if (!sel.size) return toast('Nothing selected');
    if (!(await confirmDlg('Move to Recycle Bin?', `${sel.size} file(s) will be moved to the Recycle Bin. You can restore them from there.`, 'Move'))) return;
    const r = await call('deleteFiles', { paths: [...sel] });
    toast(`${r.filter(x => x.ok).length} file(s) moved to the Recycle Bin`);
    after();
  }

  const views = {
    large: async (out) => {
      const min = h('input', { type: 'number', value: 100, min: 1, style: { width: '90px' } });
      const res = h('div');
      const btn = h('button', { class: 'btn primary' }, 'Find large files');
      const run = () => busy(btn, async () => {
        res.replaceChildren(loading('Scanning… this can take a minute on big folders.'));
        const files = await call('largeFiles', { root: root.value, minMB: Number(min.value) });
        const sel = new Set();
        res.replaceChildren(files.length ? h('div', {}, h('div', { class: 'row', style: { margin: '10px 0' } }, h('span', { class: 'sub' }, `${files.length} files · ${fmt(files.reduce((a, f) => a + f.size, 0))}`), h('div', { class: 'spacer' }), h('button', { class: 'btn danger small', onclick: () => trash(sel, run) }, 'Move selected to Recycle Bin')), fileTable(files, sel)) : h('div', { class: 'empty' }, 'No files above that size.'));
      }, 'Scanning…');
      btn.onclick = run;
      out.append(h('div', { class: 'card' }, h('div', { class: 'row' }, 'Larger than', min, 'MB', btn), res));
    },
    dupes: async (out) => {
      const res = h('div');
      const btn = h('button', { class: 'btn primary' }, 'Find duplicates');
      const run = () => busy(btn, async () => {
        res.replaceChildren(loading('Comparing file contents (SHA-256)…'));
        const groups = await call('duplicates', { root: root.value, minKB: 256 });
        const sel = new Set();
        groups.forEach(g => g.files.slice(1).forEach(f => sel.add(f.path))); // keep the oldest copy
        const waste = groups.reduce((a, g) => a + g.size * (g.files.length - 1), 0);
        res.replaceChildren(groups.length ? h('div', {}, h('div', { class: 'row', style: { margin: '10px 0' } }, h('span', { class: 'sub' }, `${groups.length} sets · ${fmt(waste)} wasted. The oldest copy of each is kept unticked.`), h('div', { class: 'spacer' }), h('button', { class: 'btn danger small', onclick: () => trash(sel, run) }, 'Move selected to Recycle Bin')),
          ...groups.map(g => h('div', { style: { marginBottom: '12px' } }, h('div', { class: 'title' }, `${fmt(g.size)} × ${g.files.length}`), fileTable(g.files, sel)))) : h('div', { class: 'empty' }, 'No duplicates found.'));
      }, 'Scanning…');
      btn.onclick = run;
      out.append(h('div', { class: 'card' }, h('div', { class: 'row' }, h('span', { class: 'sub', style: { flex: 1 } }, 'Finds identical files by content, not just name.'), btn), res));
    },
    folders: async (out) => {
      const res = h('div');
      const btn = h('button', { class: 'btn primary' }, 'Analyse folders');
      btn.onclick = () => busy(btn, async () => {
        res.replaceChildren(loading());
        const f = await call('folderSizes', { root: root.value });
        const max = Math.max(1, ...f.map(x => x.size));
        res.replaceChildren(h('div', { class: 'bars', style: { marginTop: '12px' } }, f.slice(0, 30).map(x => h('div', { class: 'bar-row' }, h('div', { class: 'trunc', title: x.path }, x.name), meter(pct(x.size, max), 101, 101), h('div', { class: 'num' }, fmt(x.size))))));
      }, 'Analysing…');
      out.append(h('div', { class: 'card' }, h('div', { class: 'row' }, h('span', { class: 'sub', style: { flex: 1 } }, 'See which folders take the most space.'), btn), res));
    },
    downloads: async (out) => {
      const res = h('div', {}, loading());
      out.append(h('div', { class: 'card' }, h('div', { class: 'sub' }, 'Sorts files in your Downloads folder into sub-folders by type (Images, Documents, Installers…). Nothing is deleted.'), res));
      const preview = await call('organizeDownloads', { apply: false });
      const by = {};
      preview.moves.forEach(m => { (by[m.folder] = by[m.folder] || []).push(m); });
      const btn = h('button', { class: 'btn primary', disabled: !preview.moves.length }, `Organise ${preview.moves.length} files`);
      btn.onclick = () => busy(btn, async () => { const r = await call('organizeDownloads', { apply: true }); toast(r.applied ? `Organised ${r.moves.length} files` : 'Demo mode: no files were moved'); });
      res.replaceChildren(h('div', { class: 'list', style: { margin: '10px 0' } }, Object.entries(by).map(([k, v]) => h('div', { class: 'item' }, h('div', { class: 'body' }, h('div', { class: 'title' }, k), h('div', { class: 'desc' }, v.slice(0, 4).map(x => x.name).join(', ') + (v.length > 4 ? ` +${v.length - 4} more` : ''))), h('div', { class: 'meta' }, `${v.length} files · ${fmt(v.reduce((a, x) => a + x.size, 0))}`)))),
        preview.moves.length ? null : h('div', { class: 'empty' }, 'Downloads is already tidy.'), btn);
    },
  };
  async function draw() {
    wrap.replaceChildren(tabs([['large', 'Large files'], ['dupes', 'Duplicates'], ['folders', 'Folder sizes'], ['downloads', 'Organise Downloads']], tab, (id) => { tab = S.cache.storageTab = id; draw(); }));
    const out = h('div'); wrap.append(out); await views[tab](out);
  }
  main.append(pageHead('Storage', 'See what fills your drives, find large and duplicate files, and tidy Downloads.'), disks, h('div', { class: 'row', style: { margin: '16px 0' } }, 'Folder to scan:', root, h('button', { class: 'btn small', onclick: () => { call('setSettings', { patch: { scanRoot: root.value } }).then(s => { S.settings = s; toast('Saved as default'); }); } }, 'Save as default')), wrap);
  draw();
};

PAGES.security = async (main) => {
  const body = h('div', {}, loading('Reading Windows Security status…'));
  main.append(pageHead('Security', 'Your Windows protection at a glance, with one-click fixes.', h('button', { class: 'btn', onclick: () => call('openUri', { uri: 'windowsdefender:' }) }, 'Open Windows Security')), body);
  const [s, threats] = await Promise.all([call('security'), call('threats').catch(() => [])]);
  const d = s.defender || {};
  const status = (ok, good, bad) => h('span', { class: 'tag ' + (ok ? 'good' : 'bad') }, ok ? good : bad);
  const scanBtn = (label, action, primary) => h('button', { class: 'btn' + (primary ? ' primary' : ''), onclick: (e) => busy(e.currentTarget, async () => { if (action !== 'update') toast(`${label} running — you can keep using the PC`); await call('defenderAction', { action }); toast(`${label} finished`); PAGES.security(main.replaceChildren() || main); }, 'Running…') }, label);
  const rows = [
    ['Real-time protection', status(d.realtime, 'On', 'Off'), d.realtime ? null : h('button', { class: 'btn small', onclick: () => call('openUri', { uri: 'windowsdefender:' }) }, 'Turn on')],
    ['Virus definitions', status(d.signatureAge <= 3, `${d.signatureAge} day(s) old`, `${d.signatureAge} days old`), scanBtn('Update', 'update')],
    ['Last quick scan', status(d.quickScanAge <= 14, `${d.quickScanAge} day(s) ago`, `${d.quickScanAge} days ago`), null],
    ['Tamper protection', status(d.tamper, 'On', 'Off'), null],
    ['Firewall', h('span', {}, (s.firewall || []).map(f => status(f.enabled, `${f.name} on`, `${f.name} off`))), (s.firewall || []).some(f => !f.enabled) ? h('button', { class: 'btn small', onclick: (e) => busy(e.currentTarget, () => runFix({ fix: 'firewall-on' })) }, 'Turn all on') : null],
    ['Windows Update', status(s.lastUpdate && (Date.now() - new Date(s.lastUpdate)) / 86400e3 < 30, `Last installed ${ago(s.lastUpdate)}`, `Last installed ${ago(s.lastUpdate)}`), h('button', { class: 'btn small', onclick: () => call('openUri', { uri: 'ms-settings:windowsupdate' }) }, 'Check')],
    ['User Account Control', status(s.uac, 'On', 'Off'), null],
    ['SmartScreen', status(s.smartscreen !== 'Off', s.smartscreen || 'Default', 'Off'), null],
    ['BitLocker (system drive)', h('span', { class: 'tag' }, s.bitlocker || 'Unknown'), null],
    ['Remote Desktop', status(!s.rdpEnabled, 'Off', 'On — exposed'), s.rdpEnabled ? h('button', { class: 'btn small', onclick: (e) => busy(e.currentTarget, () => runFix({ fix: 'rdp-off' })) }, 'Turn off') : null],
    ['SMB1 (legacy sharing)', status(!s.smb1, 'Off', 'On — insecure'), s.smb1 ? h('button', { class: 'btn small', onclick: (e) => busy(e.currentTarget, () => runFix({ fix: 'smb1-off' })) }, 'Turn off') : null],
  ];
  body.replaceChildren(
    h('div', { class: 'card' }, h('div', { class: 'row' }, h('div', { style: { flex: 1 } }, h('h2', {}, 'Virus & threat scan'), h('div', { class: 'sub' }, `Antivirus: ${(s.antivirus || []).join(', ') || 'Microsoft Defender'}`)), scanBtn('Quick scan', 'quick', true), scanBtn('Full scan', 'full'))),
    h('div', { class: 'card' }, h('div', { class: 'list' }, rows.map(([k, v, a]) => h('div', { class: 'item' }, h('div', { class: 'body title' }, k), v, a)))),
    h('div', { class: 'section-title' }, 'Recent threats'),
    h('div', { class: 'card' }, threats.length ? h('div', { class: 'list' }, threats.map(t => h('div', { class: 'item' }, h('span', { class: 'sev ' + (t.severity >= 4 ? 'high' : 'medium') }), h('div', { class: 'body' }, h('div', { class: 'title' }, t.name), h('div', { class: 'desc mono' }, (t.resources || []).join(', '))), h('div', { class: 'meta' }, `${(t.when || '').slice(0, 10)} · ${t.action ? 'removed' : 'action needed'}`)))) : h('div', { class: 'empty' }, 'No threats detected.')));
};

PAGES.updates = async (main) => {
  const C = S.C;
  const body = h('div', {}, loading('Reading Windows Update settings…'));
  main.append(pageHead('Windows Update', 'Take back control: no surprise restarts, pause as long as you need, stay on a version you trust, and roll back a bad update.',
    h('button', { class: 'btn', onclick: () => call('updatesAction', { action: 'scan' }) }, 'Check for updates')), body);
  const draw = async () => {
    const [st, states] = await Promise.all([call('updatesStatus'), call('tweakStates')]);
    const act = (label, args, msg) => h('button', { class: 'btn small', onclick: (e) => busy(e.currentTarget, async () => { await call('updatesAction', args); toast(msg); draw(); }) }, label);
    const paused = st.pausedUntil && new Date(st.pausedUntil) > new Date();
    const startSel = h('select', {}, [...Array(24).keys()].map(i => h('option', { value: i, selected: i === st.activeStart }, `${String(i).padStart(2, '0')}:00`)));
    const endSel = h('select', {}, [...Array(24).keys()].map(i => h('option', { value: i, selected: i === st.activeEnd }, `${String(i).padStart(2, '0')}:00`)));
    const tw = C.TWEAKS.filter(t => t.cat === 'updates');
    body.replaceChildren(
      st.rebootPending ? h('div', { class: 'banner info' }, 'A restart is waiting to finish installing updates. Restart when it suits you.') : null,
      h('div', { class: 'grid g2' },
        h('div', { class: 'card' }, h('h3', {}, 'Pause updates'),
          h('div', { class: 'sub', style: { marginBottom: '10px' } }, paused ? `Paused until ${new Date(st.pausedUntil).toLocaleDateString()}.` : 'Updates are running normally. Pausing delays security fixes too — resume when you can.'),
          h('div', { class: 'row' }, [7, 14, 35, 90, 365].map(d => act(d === 365 ? '1 year' : `${d} days`, { action: 'pause', days: d }, `Updates paused for ${d} days`)), paused ? act('Resume now', { action: 'resume' }, 'Updates resumed') : null)),
        h('div', { class: 'card' }, h('h3', {}, 'Active hours'), h('div', { class: 'sub', style: { marginBottom: '10px' } }, 'Windows never restarts for updates during these hours (max 18 hours).'),
          h('div', { class: 'row' }, 'From', startSel, 'to', endSel, h('button', { class: 'btn primary small', onclick: (e) => busy(e.currentTarget, async () => { await call('updatesAction', { action: 'active-hours', start: Number(startSel.value), end: Number(endSel.value) }); toast('Active hours saved'); draw(); }) }, 'Save'))),
        h('div', { class: 'card' }, h('h3', {}, 'Stay on this version'), h('div', { class: 'sub', style: { marginBottom: '10px' } }, st.lockedVersion ? `Locked to ${st.product} ${st.lockedVersion}. You still get security updates, but no big feature upgrades.` : `You are on ${st.product} ${st.displayVersion}. Lock it to skip feature upgrades (and their new AI features and bugs) while still getting security fixes.`),
          st.lockedVersion ? act('Unlock', { action: 'unlock' }, 'Version unlocked') : act(`Lock to ${st.displayVersion}`, { action: 'lock', version: st.displayVersion }, `Locked to ${st.displayVersion}`)),
        h('div', { class: 'card' }, h('h3', {}, 'Update behaviour'), h('div', { class: 'list' }, tw.map(t => h('div', { class: 'item' }, h('div', { class: 'body' }, h('div', { class: 'title' }, t.title, recTag(t.rec)), t.desc ? h('div', { class: 'desc' }, t.desc) : null),
          toggle(!!states[t.id], async (on, input) => { input.disabled = true; try { reportTweaks(await call('setTweaks', { changes: [{ id: t.id, on }], label: `${on ? 'Applied' : 'Reverted'}: ${t.title}` }), `${on ? 'Applied' : 'Reverted'}: ${t.title}`); } catch { input.checked = !on; } draw(); })))))),
      h('div', { class: 'section-title' }, 'Recently installed updates'),
      h('div', { class: 'card' }, h('div', { class: 'sub', style: { marginBottom: '6px' } }, 'If something broke after an update, uninstall it here. Some combined updates cannot be removed — use Update history → Uninstall updates for those.'),
        h('div', { class: 'list' }, (st.hotfixes || []).map(u => h('div', { class: 'item' }, h('div', { class: 'body' }, h('div', { class: 'title' }, u.kb), h('div', { class: 'desc' }, `${u.desc} · installed ${(u.installed || '').slice(0, 10)} (${ago(u.installed)})`)),
          h('button', { class: 'btn small danger', onclick: async (e) => { if (!(await confirmDlg(`Uninstall ${u.kb}?`, 'Windows Update Standalone Installer will ask you to confirm, then a restart is usually needed. Pause updates afterwards so it does not reinstall straight away.', 'Uninstall'))) return; await busy(e.currentTarget, () => call('updatesAction', { action: 'uninstall', kb: u.kb })); toast(`Uninstall started for ${u.kb}`); } }, 'Uninstall'))))));
  };
  draw();
};

PAGES.network = async (main) => {
  const C = S.C;
  const adapters = h('div', {}, loading());
  const lat = h('div');
  const latBtn = h('button', { class: 'btn' }, 'Test latency');
  latBtn.onclick = () => busy(latBtn, async () => { const r = await call('latency'); lat.replaceChildren(h('div', { class: 'list' }, r.map(x => h('div', { class: 'item' }, h('div', { class: 'body title' }, `${x.name} (${x.host})`), h('div', { class: 'meta' }, x.avg === null ? 'unreachable' : `${x.avg} ms · ${x.loss}% loss`))))); }, 'Testing…');
  const act = (label, action, note) => h('button', { class: 'btn', onclick: (e) => busy(e.currentTarget, async () => { await call('networkAction', { action }); toast(note); }) }, label);
  main.append(pageHead('Network', 'Fix connection problems, switch to a faster or safer DNS, and test your connection.'),
    h('div', { class: 'card' }, h('h3', {}, 'Quick fixes'), h('div', { class: 'row' }, act('Flush DNS cache', 'flushdns', 'DNS cache cleared'), act('Renew IP address', 'renew', 'IP address renewed'), act('Edit hosts file', 'hosts', 'Hosts file opened'), h('button', { class: 'btn', onclick: () => go('repair') }, 'Full network reset…'), latBtn), lat),
    h('div', { class: 'section-title' }, 'Adapters & DNS'), adapters);
  const list = await call('network');
  adapters.replaceChildren(...list.map(a => {
    const sel = h('select', {}, C.DNS_PROVIDERS.map(p => h('option', { value: p.id, selected: p.servers.length && p.servers[0] === a.dns[0] }, p.name)));
    return h('div', { class: 'card' }, h('div', { class: 'row' }, h('h3', { style: { flex: 1 } }, `${a.name} `, h('span', { class: 'sub' }, a.desc)), h('span', { class: 'tag' }, a.speed)),
      h('div', { class: 'kv', style: { margin: '8px 0 12px' } }, h('div', {}, 'IP address'), h('div', {}, a.ipv4.join(', ')), h('div', {}, 'Gateway'), h('div', {}, a.gateway.join(', ') || '—'), h('div', {}, 'DNS servers'), h('div', {}, a.dns.join(', ') || '—'), h('div', {}, 'MAC'), h('div', { class: 'mono' }, a.mac)),
      h('div', { class: 'row' }, 'DNS provider:', sel, adminTag(true), h('button', { class: 'btn primary small', onclick: (e) => busy(e.currentTarget, async () => { await call('networkAction', { action: 'dns', index: a.index, dns: sel.value }); toast(`DNS updated for ${a.name}`); }) }, 'Apply')));
  }));
};

PAGES.repair = async (main) => {
  const C = S.C;
  const plans = h('div', { class: 'list' }, loading());
  main.append(pageHead('Repair & power', 'Built-in Windows repair tools, one click away. Admin tools open in a window so you can watch progress.'),
    h('div', { class: 'card' }, h('div', { class: 'list' }, C.REPAIRS.map(r => h('div', { class: 'item' }, h('div', { class: 'body' }, h('div', { class: 'title' }, r.name, adminTag(r.admin)), r.desc ? h('div', { class: 'desc' }, r.desc) : null),
      h('button', { class: 'btn small', onclick: async (e) => { if (r.admin && !(await confirmDlg(r.name, `${r.desc}\nWindows will ask for administrator permission and a console window will show progress.`, 'Run'))) return; await busy(e.currentTarget, () => call('runRepair', { id: r.id })); toast(`${r.name} started`); } }, 'Run'))))),
    h('div', { class: 'section-title' }, 'Power plan'), h('div', { class: 'card' }, plans));
  const load = async () => {
    const p = await call('powerPlans');
    plans.replaceChildren(...p.map(x => h('label', { class: 'item' }, h('input', { type: 'radio', name: 'plan', checked: x.active, onchange: async () => { await call('setPowerPlan', { guid: x.guid }); toast(`Power plan: ${x.name}`); load(); } }), h('div', { class: 'body title' }, x.name), x.active ? h('span', { class: 'tag good' }, 'active') : null)));
  };
  load();
};

PAGES.toolbox = async (main) => {
  const C = S.C;
  const cats = [...new Set(C.TOOLBOX.map(t => t.cat))];
  const custom = h('div');
  const name = h('input', { type: 'text', placeholder: 'Name', style: { minWidth: '160px' } });
  const target = h('input', { type: 'text', placeholder: 'C:\\path\\to\\app.exe or https://…' });
  function drawCustom() {
    const sc = S.settings.shortcuts || [];
    custom.replaceChildren(sc.length ? h('div', { class: 'grid g4' }, sc.map((s, i) => h('div', { class: 'tile', onclick: () => call('openShortcut', { index: i }) }, h('span', { class: 't' }, s.name), h('span', { class: 'd trunc', style: { maxWidth: '100%' } }, s.target),
      h('button', { class: 'btn small', onclick: async (e) => { e.stopPropagation(); S.settings = await call('setSettings', { patch: { shortcuts: sc.filter((_, j) => j !== i) } }); drawCustom(); } }, 'Remove')))) : h('div', { class: 'sub' }, 'Add your own apps, folders or websites.'));
  }
  main.append(pageHead('Toolbox', 'Every useful Windows tool in one place, plus your own shortcuts.'),
    ...cats.flatMap(c => [h('div', { class: 'section-title' }, c), h('div', { class: 'grid g4' }, C.TOOLBOX.filter(t => t.cat === c).map(t => h('button', { class: 'tile', onclick: () => call('launchTool', { id: t.id }).then(() => S.info.demo && toast(`Demo: would open ${t.name}`)) }, h('span', { class: 't' }, t.name), h('span', { class: 'd mono' }, t.cmd || t.uri))))]),
    h('div', { class: 'section-title' }, 'My shortcuts'),
    h('div', { class: 'card' }, h('div', { class: 'row', style: { marginBottom: '12px' } }, name, target, h('button', { class: 'btn primary', onclick: async () => { if (!name.value || !target.value) return; S.settings = await call('setSettings', { patch: { shortcuts: [...(S.settings.shortcuts || []), { name: name.value, target: target.value }] } }); name.value = target.value = ''; drawCustom(); } }, 'Add')), custom));
  drawCustom();
};

PAGES.system = async (main) => {
  const body = h('div', {}, loading());
  main.append(pageHead('System info', 'Hardware and Windows details.'), body);
  const s = await call('systemInfo');
  const kv = (pairs) => h('div', { class: 'kv' }, pairs.flatMap(([k, v]) => [h('div', {}, k), h('div', {}, v ?? '—')]));
  body.replaceChildren(h('div', { class: 'grid g2' },
    h('div', { class: 'card' }, h('h3', {}, 'Windows'), kv([['Edition', s.os], ['Version', `${s.displayVersion || ''} (build ${s.build})`], ['Architecture', s.arch], ['Installed', (s.installDate || '').slice(0, 10)], ['Last boot', `${(s.lastBoot || '').replace('T', ' ')} (${ago(s.lastBoot)})`], ['Computer name', s.computerName], ['User', `${s.user}${s.isAdmin ? ' (administrator)' : ''}`]])),
    h('div', { class: 'card' }, h('h3', {}, 'Hardware'), kv([['Device', `${s.manufacturer} ${s.model}`], ['Processor', s.cpu], ['Cores / threads', `${s.cores} / ${s.threads}`], ['Memory', fmt(s.ramTotal)], ['Graphics', (s.gpu || []).join(', ')], ['Motherboard', s.board], ['BIOS', s.bios], ['Battery', s.battery ? `${s.battery.charge}%` : 'None']])),
    h('div', { class: 'card' }, h('h3', {}, 'Drives'), kv((s.physicalDisks || []).map(d => [d.name, `${fmt(d.size)} · ${d.bus} ${d.media} · ${d.health}`]))),
    h('div', { class: 'card' }, h('h3', {}, 'Volumes'), kv((s.disks || []).map(d => [`${d.name} ${d.label || ''}`, `${fmt(d.free)} free of ${fmt(d.total)} (${d.fs})`])))));
};

PAGES.history = async (main) => {
  const list = h('div', { class: 'list' });
  const load = async () => {
    const hist = await call('history');
    list.replaceChildren(...(hist.length ? hist.map((x, i) => h('div', { class: 'item' }, h('div', { class: 'body' }, h('div', { class: 'title' }, x.summary), h('div', { class: 'desc' }, new Date(x.at).toLocaleString(), ' · ', x.kind)),
      x.undo ? h('button', { class: 'btn small', disabled: x.undone, onclick: async (e) => { await busy(e.currentTarget, () => call('undo', { index: i })); toast('Undone'); load(); } }, x.undone ? 'Undone' : 'Undo') : null)) : [h('div', { class: 'empty' }, 'No changes yet.')]));
  };
  main.append(pageHead('History & undo', 'Everything OpenSweep changed on this PC. Settings, startup and task changes can be undone individually.'), h('div', { class: 'card' }, list));
  load();
};

PAGES.settings = async (main) => {
  const s = S.settings;
  const set = async (patch) => { S.settings = await call('setSettings', { patch }); applyTheme(); };
  const theme = h('select', { onchange: (e) => set({ theme: e.target.value }) }, [['system', 'System'], ['light', 'Light'], ['dark', 'Dark']].map(([v, l]) => h('option', { value: v, selected: s.theme === v }, l)));
  const row = (title, desc, ctrl) => h('div', { class: 'item' }, h('div', { class: 'body' }, h('div', { class: 'title' }, title), desc ? h('div', { class: 'desc' }, desc) : null), ctrl);
  main.append(pageHead('Settings'),
    h('div', { class: 'card' }, h('div', { class: 'list' },
      row('Theme', null, theme),
      row('Mini toolbar', 'Small always-on-top window with live CPU/RAM and a Boost button.', toggle(s.showWidget, (v) => set({ showWidget: v }))),
      row('Keep running in the tray', 'Closing the window hides OpenSweep to the notification area.', toggle(s.minimizeToTray, (v) => set({ minimizeToTray: v }))),
      row('Start with Windows', 'Starts hidden in the tray.', toggle(s.launchAtLogin, (v) => set({ launchAtLogin: v }))),
      row('Ask before making changes', 'Show a confirmation before cleaning, removing or changing settings.', toggle(s.confirmActions, (v) => set({ confirmActions: v }))),
      row('Background stability check', 'Checks drivers, blue screens and hardware errors about once an hour at the lowest CPU priority.', toggle(s.stabilityMonitor, (v) => set({ stabilityMonitor: v }))),
      row('Stability notifications', 'Show a Windows notification when a new driver problem or crash is found.', toggle(s.stabilityNotify, (v) => set({ stabilityNotify: v }))),
      row('Administrator mode', S.info.isAdmin ? 'Running as administrator ✓ — changes apply directly, no prompts.' : 'Not running as administrator.', S.info.isAdmin ? h('span', { class: 'tag good' }, 'admin') : h('button', { class: 'btn small', onclick: async () => { const r = await call('relaunchAsAdmin'); if (r.demo) toast('Not available in demo mode'); } }, 'Restart as admin')))),
    updateCard(set),
    h('div', { class: 'card' }, h('h3', {}, 'About OpenSweep'), h('div', { class: 'sub' }, `Version ${S.info.version}${S.info.demo ? ' · demo mode' : ''}. Free and open source (MIT). No telemetry, no ads, no account. OpenSweep only goes online to check GitHub for updates (if turned on) and when you press "Test latency".`)));
};

function updateCard(set) {
  const u = S.update || {};
  const status = { disabled: S.info.demo ? 'Not available in demo mode.' : 'Updates work in the installed app.', idle: 'Not checked yet.', checking: 'Checking GitHub…', latest: `You have the latest version (${S.info.version}).`,
    downloading: `Downloading ${u.version}… ${u.progress || 0}%`, ready: `Version ${u.version} is downloaded and ready.`, error: `Could not check for updates: ${u.error}` }[u.state] || '';
  return h('div', { class: 'card' }, h('h3', {}, 'Updates'), h('div', { class: 'list' },
    h('div', { class: 'item' }, h('div', { class: 'body' }, h('div', { class: 'title' }, 'Update automatically'), h('div', { class: 'desc' }, 'Downloads new versions from the OpenSweep GitHub releases in the background. If OpenSweep is in the tray it updates itself straight away; otherwise it asks you to restart.')), toggle(S.settings.autoUpdate, (v) => set({ autoUpdate: v }))),
    h('div', { class: 'item' }, h('div', { class: 'body' }, h('div', { class: 'title' }, `Version ${S.info.version}`), h('div', { class: 'desc' }, status)),
      u.state === 'ready' ? h('button', { class: 'btn small primary', onclick: () => call('installUpdate') }, 'Restart to update')
        : h('button', { class: 'btn small', disabled: u.state === 'disabled' || u.state === 'checking' || u.state === 'downloading', onclick: async (e) => { S.update = await busy(e.currentTarget, () => call('checkUpdate'), 'Checking…'); if (S.page === 'settings') go('settings'); } }, 'Check now'))));
}

PAGES.stability = async (main) => {
  const body = h('div', {}, loading('Checking drivers and the event log…'));
  const refresh = h('button', { class: 'btn', onclick: (e) => busy(e.currentTarget, () => load(true), 'Checking…') }, 'Check now');
  main.append(pageHead('Drivers & stability', 'Finds devices with driver problems, blue screens, unexpected shutdowns, hardware errors and display driver crashes from the last 7 days. Runs in the background at low priority.', refresh,
    h('button', { class: 'btn', onclick: (e) => busy(e.currentTarget, async () => { await call('scanDevices'); toast('Scanned for hardware changes'); await load(true); }) }, 'Scan for hardware changes', adminTag(true))), body);
  const tool = (label, id) => h('button', { class: 'btn small', onclick: () => call('launchTool', { id }).catch(() => {}) }, label);
  async function load(refreshNow) {
    const r = await call('stability', { refresh: !!refreshNow });
    const ok = (x) => h('div', { class: 'empty' }, x);
    const toolIds = new Set((S.C.TOOLBOX || []).map(t => t.id));
    body.replaceChildren(
      h('div', { class: 'grid g3' },
        h('div', { class: 'card stat' }, h('div', { class: 'sub' }, 'Devices with problems'), h('div', { class: 'big ' + (r.devices.length ? 'bad' : 'good') }, String(r.devices.length))),
        h('div', { class: 'card stat' }, h('div', { class: 'sub' }, `Stability events (${r.days} days)`), h('div', { class: 'big ' + (r.events.length ? 'warn' : 'good') }, String(r.events.length))),
        h('div', { class: 'card stat' }, h('div', { class: 'sub' }, 'Third-party drivers'), h('div', { class: 'big' }, String(r.thirdPartyDrivers)))),
      h('div', { class: 'card' }, h('h3', {}, 'Devices with driver problems'), r.devices.length ? h('div', { class: 'list' }, r.devices.map(d => h('div', { class: 'item' }, h('span', { class: 'tag warn' }, `code ${d.code}`),
        h('div', { class: 'body' }, h('div', { class: 'title' }, d.name), h('div', { class: 'desc' }, `${d.problem}${d.cls ? ' · ' + d.cls : ''}`))))) : ok('✓ All devices are working.'),
        h('div', { class: 'row', style: { marginTop: '10px' } }, h('button', { class: 'btn small', onclick: () => call('openUri', { uri: 'ms-settings:windowsupdate-optionalupdates' }) }, 'Optional driver updates'), toolIds.has('devmgmt') ? tool('Device Manager', 'devmgmt') : null, toolIds.has('reliability') ? tool('Reliability Monitor', 'reliability') : null)),
      h('div', { class: 'card' }, h('h3', {}, 'Crashes & hardware errors'), r.events.length ? h('div', { class: 'list' }, r.events.map(e => h('div', { class: 'item' }, h('span', { class: 'tag ' + (e.kind === 'Blue screen' || e.kind === 'Hardware error' ? 'bad' : 'warn') }, e.kind),
        h('div', { class: 'body' }, h('div', { class: 'desc' }, e.message), h('div', { class: 'desc mono' }, `${new Date(e.when).toLocaleString()} · ${e.source} ${e.eventId}`))))) : ok('✓ No crashes or hardware errors.')),
      r.oldDrivers.length || r.unsignedDrivers.length ? h('div', { class: 'card' }, h('h3', {}, 'Drivers worth updating'), h('div', { class: 'list' },
        r.unsignedDrivers.map(d => h('div', { class: 'item' }, h('span', { class: 'tag bad' }, 'unsigned'), h('div', { class: 'body' }, h('div', { class: 'title' }, d.name), h('div', { class: 'desc' }, `${d.provider} · ${d.version}`)))),
        r.oldDrivers.map(d => h('div', { class: 'item' }, h('span', { class: 'tag warn' }, d.date.slice(0, 4)), h('div', { class: 'body' }, h('div', { class: 'title' }, d.name), h('div', { class: 'desc' }, `${d.provider} · ${d.version} · over 5 years old — check the maker's website`)))))) : null,
      h('div', { class: 'sub' }, `Last checked ${new Date(r.checkedAt).toLocaleString()}.`));
  }
  load(false).catch(e => body.replaceChildren(h('div', { class: 'banner' }, 'Could not check: ' + e.message)));
};

// ------------------------------------------------------------------ boot
function applyTheme() { const t = S.settings.theme; if (t === 'system') document.documentElement.removeAttribute('data-theme'); else document.documentElement.setAttribute('data-theme', t); }
(async function boot() {
  [S.info, S.C, S.settings] = await Promise.all([window.api.call('info'), window.api.call('catalog'), window.api.call('getSettings')]);
  applyTheme();
  S.update = await window.api.call('updateStatus').catch(() => null);
  window.api.on((name, data) => {
    if (name === 'go') go(data);
    else if (name === 'update') { const was = S.update && S.update.state; S.update = data; if (data.state === 'ready' && was !== 'ready') go(S.page); else if (S.page === 'settings' && data.state !== 'downloading') go('settings'); }
  });
  go('home');
})();
