'use strict';
/* OpenSweep Core dashboard (Parts 13, phases 2–8). Uses helpers from app.js and pages.js.
   Every value shown is either measured by the Core service or marked "Simulated" (demo mode, or
   decisions the engine only simulated in Monitor mode). No claims without a measurement. */

const fmtDur = (s) => { if (s === null || s === undefined || isNaN(s)) return '—'; s = Math.max(0, Math.floor(s)); const d = Math.floor(s / 86400), hh = Math.floor(s % 86400 / 3600), m = Math.floor(s % 3600 / 60); return d ? `${d}d ${hh}h ${m}m` : hh ? `${hh}h ${m}m` : m ? `${m}m ${s % 60}s` : `${s}s`; };
const coreErr = (e) => String((e && e.message) || e).replace(/^Error invoking remote method 'api': (Error: )?/, '');
const tsOf = (x) => Number(x && (x.tsMs ?? x.ts ?? x.openedAt ?? x.appliedAt)) || (x && x.at ? Date.parse(x.at) : 0);
const fmtTime = (ms) => { if (!ms) return '—'; const d = new Date(ms); return Date.now() - ms < 20 * 3600e3 ? d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' }) : d.toLocaleString([], { day: 'numeric', month: 'short', hour: '2-digit', minute: '2-digit' }); };
const fmtNum = (v, d = 1) => (v === null || v === undefined || isNaN(v) ? '—' : Number(v).toLocaleString(undefined, { maximumFractionDigits: d }));
function fmtUnit(v, unit) {
  if (v === null || v === undefined || isNaN(v)) return '—';
  if (unit === 'bytes') return fmt(v);
  if (unit === 'bytes/s') return fmtRate(v) || '0';
  if (unit === '%') return `${fmtNum(v, 1)} %`;
  if (unit === 'ms') return `${fmtNum(v, v < 10 ? 2 : 0)} ms`;
  return `${fmtNum(v, v < 10 ? 2 : 0)}${unit ? ' ' + unit : ''}`;
}
const human = (s) => String(s || '').replace(/_/g, ' ').replace(/([a-z])([A-Z])/g, '$1 $2').replace(/^\w/, c => c.toUpperCase());
const targetName = (t) => (!t ? '—' : typeof t === 'string' ? t : t.name || (t.pid ? `PID ${t.pid}` : '—'));
const ACTION_LABEL = { ecoqos: 'Efficiency mode (EcoQoS)', EcoQoS: 'Efficiency mode (EcoQoS)', memoryPriority: 'Lower memory priority', MemoryPriority: 'Lower memory priority', priorityClass: 'Below-normal priority', PriorityClass: 'Below-normal priority', cpuSets: 'Efficiency cores (CPU sets)', CpuSets: 'Efficiency cores (CPU sets)', setMode: 'Mode change', restartService: 'Restart service', restartEtwSession: 'Restart trace session', reloadConfig: 'Reload configuration' };
const actionLabel = (a) => ACTION_LABEL[a] || human(a);
const TX_STATE = { verified: ['verified', 'good'], applied: ['applied', 'info'], verifying: ['verifying', 'info'], intent: ['pending', ''], rolled_back: ['rolled back', 'warn'], failed: ['failed', 'bad'], simulated: ['simulated', 'warn'], blocked: ['blocked', ''] };
const txTag = (s) => { const [l, c] = TX_STATE[s] || [human(s || 'unknown'), '']; return h('span', { class: `tag ${c}`, style: { marginLeft: 0 } }, l); };
const riskTag = (r) => (r ? h('span', { class: `tag ${r === 'high' ? 'bad' : r === 'medium' ? 'warn' : 'good'}` }, `${r} risk`) : null);
const SIM_TIP = 'Simulated: not measured on this PC, or a decision the engine only simulated (nothing was changed).';
const simTag = (on = true, label = 'Simulated') => (on ? h('span', { class: 'tag warn', title: SIM_TIP }, label) : null);

// Fixed list of the live metrics on the dashboard (names are core-validate METRICS).
const CORE_LIVE = [
  { id: 'cpuPercent', label: 'CPU', ic: 'cpu', unit: '%', max: 100, color: 'var(--c1)' },
  { id: 'memLoadPercent', label: 'Physical memory', ic: 'memory', unit: '%', max: 100, color: 'var(--c2)' },
  { id: 'commitTotal', label: 'Commit charge', ic: 'layers', unit: 'bytes', color: 'var(--c3)' },
  { id: 'diskReadLatencyMs', label: 'Storage latency (read)', ic: 'drive', unit: 'ms', color: 'var(--c5)' },
  { id: 'hardFaultsPerSec', label: 'Hard page faults', ic: 'activity', unit: '/s', color: 'var(--c8)' },
];
const MODE_INFO = [
  { id: 'monitor', ic: 'eye', t: 'Monitor', d: 'Measures, explains and recommends. Never changes Windows settings. What it would do is shown as a simulation.' },
  { id: 'balanced', ic: 'shield', t: 'Balanced', d: 'Applies only low-risk, reversible actions from the allow-list to verified background processes. Each change is journaled first, measured for 60 s and rolled back if it does not help.' },
  { id: 'advanced', ic: 'alert', t: 'Advanced', d: 'Adds experimental actions (e.g. efficiency cores on hybrid CPUs). Each one needs your separate approval. Does not bypass any Windows protection.', warn: 'Experimental. Review every intervention; roll back if anything feels slower.' },
];

function scoreRing(score, label) {
  const wrapEl = h('div'), r = 54, c = 2 * Math.PI * r, ok = Number.isFinite(score);
  const col = !ok ? 'var(--border)' : score >= 80 ? 'var(--good)' : score >= 55 ? 'var(--warn)' : 'var(--bad)';
  wrapEl.innerHTML = `<svg class="ring" viewBox="0 0 132 132"><circle cx="66" cy="66" r="${r}" fill="none" stroke="var(--panel-2)" stroke-width="12"/>
    <circle cx="66" cy="66" r="${r}" fill="none" stroke="${col}" stroke-width="12" stroke-linecap="round" stroke-dasharray="${c}" stroke-dashoffset="${ok ? c * (1 - Math.max(0, Math.min(100, score)) / 100) : c}" transform="rotate(-90 66 66)"/>
    <text x="66" y="70" text-anchor="middle" class="val">${ok ? Math.round(score) : '—'}</text><text x="66" y="90" text-anchor="middle" class="lbl">${label}</text></svg>`;
  return wrapEl;
}

// Line chart with the baseline band (median ± MAD) and a dashed median line. Numbers only go into the SVG.
function coreChart(points, { band = null, median = null, max = null, color = 'var(--accent)' } = {}) {
  const W = 300, H = 64;
  const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
  svg.setAttribute('viewBox', `0 0 ${W} ${H}`); svg.setAttribute('preserveAspectRatio', 'none'); svg.setAttribute('class', 'cchart');
  const pts = (points || []).filter(p => Array.isArray(p) && Number.isFinite(p[0]) && Number.isFinite(p[1]));
  if (pts.length < 2) return svg;
  const step = Math.max(1, Math.ceil(pts.length / 300)), thin = pts.filter((_, i) => i % step === 0 || i === pts.length - 1);
  const t0 = thin[0][0], t1 = thin[thin.length - 1][0] || t0 + 1;
  const top = max || Math.max(...thin.map(p => p[1]), band ? band[1] : 0, 1e-9) * 1.15;
  const y = (v) => (H - 2 - (Math.max(0, Math.min(top, v)) / top) * (H - 4)).toFixed(1);
  const x = (t) => (((t - t0) / Math.max(1, t1 - t0)) * W).toFixed(1);
  const line = thin.map(p => `${x(p[0])},${y(p[1])}`).join(' ');
  svg.innerHTML = `${band ? `<rect class="band" x="0" width="${W}" y="${y(band[1])}" height="${Math.max(1, y(band[0]) - y(band[1])).toFixed(1)}"/>` : ''}
    ${median !== null ? `<line class="median" x1="0" x2="${W}" y1="${y(median)}" y2="${y(median)}" vector-effect="non-scaling-stroke"/>` : ''}
    <polyline points="0,${H} ${line} ${W},${H}" fill="${color}" fill-opacity=".10" stroke="none"/>
    <polyline points="${line}" fill="none" stroke="${color}" stroke-width="2" vector-effect="non-scaling-stroke" stroke-linejoin="round"/>`;
  return svg;
}

PAGES.core = async (main) => {
  const V = S.cache.coreView || (S.cache.coreView = { tab: 'overview', range: '30m' });
  let st = null, topo = null, statusAt = 0, gen = 0;
  const wrap = h('div'), headSlot = h('div'), tabsSlot = h('div'), body = h('div');
  const read = (m, a) => window.api.call(m, a); // no toast: errors are shown inline
  const running = () => !!st && (st.demo ? st.running : st.available);
  const engine = () => (st && st.status && !st.status.unavailable ? st.status : null);
  const isSim = (x) => !!(st && st.demo) || !!(x && x.simulated);
  const canAdmin = () => !!(st && st.demo) || !!(S.info && S.info.isAdmin);
  main.append(pageHead('OpenSweep Core', 'Native background service that measures stability and resource pressure, explains what it finds and — only in the mode you choose — applies small, reversible changes that it verifies and can roll back.',
    btn('Refresh', () => refresh(true), { ic: 'refresh' })), wrap);

  // ------------------------------------------------------------ service header (status + lifecycle)
  const stateLabel = (s) => (s.demo ? (s.running ? 'Running' : s.installed ? 'Stopped' : 'Not installed')
    : s.available ? 'Running' : s.serviceState === 'not_installed' ? 'Not installed' : s.serviceState === 'running' ? 'Running (pipe not answering)' : s.serviceState === 'unknown' ? 'Unknown' : s.serviceState ? s.serviceState.replace('_', ' ').replace(/^\w/, c => c.toUpperCase()) : 'Stopped');
  const lifecycle = (method, title, text, okLabel, kind) => btn(okLabel, async (e) => {
    const b = e.currentTarget;
    if (!(await confirmDlg(title, text, okLabel))) return;
    const r = await busy(b, () => call(method), 'Working…');
    toast(`✓ OpenSweep Core: ${okLabel.toLowerCase()} done`);
    st = r; statusAt = Date.now(); draw();
  }, { small: true, kind });
  // Admin commands: the main process shows the native confirmation; here we only report the outcome.
  async function admin(b, method, args, done) {
    const r = await busy(b, () => call(method, args), 'Working…').catch(() => null);
    if (!r || r.cancelled) return;
    if (done) done(r);
    await refresh(false);
  }

  function headCard() {
    const s = st, demo = !!s.demo, run = running(), installed = s.installed !== false;
    const hello = s.hello || {}, eng = engine();
    const dotState = run ? 'on' : installed && s.serviceState !== 'not_installed' ? 'fail' : 'off';
    const canInstall = demo ? !s.installed : s.installed === false && s.exePresent;
    const mode = (eng && eng.mode) || hello.mode || 'monitor';
    return h('div', { class: 'card' }, h('div', { class: 'card-head' },
      stateDot(dotState, run ? 'Running' : 'Not running'),
      h('div', { class: 't' }, h('h2', {}, `Service: ${stateLabel(s)}`),
        h('div', { class: 'sub' }, run ? [h('span', { class: `tag ${mode === 'monitor' ? 'good' : mode === 'advanced' ? 'bad' : 'info'}`, style: { marginLeft: 0 } }, `Mode: ${human(mode)}`),
          eng && eng.simulation ? h('span', { class: 'tag', title: 'In Monitor mode every decision is simulated: nothing is applied.' }, 'decisions simulated') : null,
          h('span', { class: 'tag' }, `v${hello.version || '?'}`), h('span', { class: 'tag' }, `protocol ${hello.protocol ?? '?'}`), hello.pid ? h('span', { class: 'tag' }, `PID ${hello.pid}`) : null, simTag(demo)]
          : installed && s.installed !== null ? 'Installed but not running. Start it to see live measurements.' : 'Not installed. Install it to collect measurements in the background.')),
      canInstall ? lifecycle('coreInstall', 'Install OpenSweep Core?', 'Registers the OpenSweepCore Windows service. It starts in Monitor mode: it measures and recommends, and never changes Windows settings until you choose another mode. It can be uninstalled at any time.', 'Install', 'primary') : null,
      installed && s.installed !== null && !run ? lifecycle('coreStart', 'Start OpenSweep Core?', 'Starts the OpenSweepCore service.', 'Start', 'primary') : null,
      run ? lifecycle('coreStop', 'Stop OpenSweep Core?', 'Stops the OpenSweepCore service. Any applied policy is rolled back when it stops. Measurements pause until it is started again.', 'Stop') : null,
      installed && s.installed !== null ? lifecycle('coreUninstall', 'Uninstall OpenSweep Core?', 'Stops the service (rolling back any applied policy) and removes it from Windows. OpenSweep keeps working without it.', 'Uninstall', 'danger') : null));
  }
  function tabsBar() {
    const c = (engine() && engine().counts) || {};
    return tabs([['overview', 'Overview', 'pulse'], ['incidents', 'Incidents', 'alert', c.incidentsOpen || null], ['optimise', 'Optimisation', 'sliders', c.policiesActive || null],
      ['recovery', 'Recovery', 'repair'], ['events', 'Events', 'list'], ['system', 'System', 'cpu']], V.tab, (id) => { V.tab = id; tabsSlot.replaceChildren(tabsBar()); renderTab(); });
  }

  function draw() {
    const s = st, demo = !!s.demo, run = running();
    wrap.replaceChildren(
      demo ? h('div', { class: 'banner info' }, icon('info'), h('b', {}, 'Simulated data (demo mode).'), ' No OpenSweep Core service runs in demo mode — every number, incident and policy on this page is made up to show how it works.') : null,
      !demo && !s.exePresent && s.installed === false ? h('div', { class: 'banner' }, icon('alert'), 'OpenSweep Core is not included in this build, so it cannot be installed from here.') : null,
      !demo && s.error && s.installed !== false && !s.available ? h('div', { class: 'banner' }, icon('alert'), `The service is not answering on its pipe: ${s.error.message}`) : null,
      run && s.status && s.status.unsupported ? h('div', { class: 'banner info' }, icon('info'), 'This OpenSweep Core version only provides basic measurements. Update OpenSweep to get stability analysis, recommendations and policies.') : null,
      h('div', { class: 'gap-16' }), headSlot,
      run ? [h('div', { class: 'gap-16' }), tabsSlot, body]
        : h('div', { class: 'card' }, emptyState(s.installed !== false && s.installed !== null ? 'OpenSweep Core is not running — measurements, incidents and policies appear here once it runs.' : 'Install and start OpenSweep Core to see measurements here.', 'cpu', false)));
    headSlot.replaceChildren(headCard());
    if (run) { tabsSlot.replaceChildren(tabsBar()); renderTab(); }
  }

  // ------------------------------------------------------------ tabs
  let live = null; // overview live state, refreshed by tick()
  async function renderTab() {
    const my = ++gen; live = null;
    body.replaceChildren(h('div', { class: 'card' }, loading('Loading…')));
    const fn = { overview: tabOverview, incidents: tabIncidents, optimise: tabOptimise, recovery: tabRecovery, events: tabEvents, system: tabSystem }[V.tab] || tabOverview;
    try { const out = await fn(my); if (my === gen && out) body.replaceChildren(...[out].flat()); }
    catch (e) { if (my === gen) body.replaceChildren(h('div', { class: 'banner bad' }, icon('alert'), `Could not read from OpenSweep Core: ${coreErr(e)}`)); }
  }
  const stale = (my) => my !== gen;
  const unavailableCard = (what, e) => h('div', { class: 'card' }, emptyState(e && /unknown_cmd|unknown command/i.test(coreErr(e)) ? `${what} needs a newer OpenSweep Core.` : `${what} is not available: ${coreErr(e)}`, 'info', false));

  // ---- Overview: stability score, live metrics with baselines, recent incidents
  async function tabOverview(my) {
    const eng = engine();
    if (!eng) return phase1Overview();
    const [bl, inc] = await Promise.all([read('coreBaselines').catch(() => null), read('coreIncidents', { limit: 3 }).catch(() => null)]);
    if (stale(my)) return null;
    const reasons = eng.scoreReasons || [];
    const cards = CORE_LIVE.map(m => {
      const b = bl && bl.metrics && bl.metrics[m.id];
      const el = { m, b, value: h('div', { class: 'value' }, '–'), chart: h('div', { class: 'cchart-wrap' }), cap: h('div', { class: 'desc' }), points: [], unit: m.unit };
      el.card = h('div', { class: 'card tight kpi' }, h('div', { class: 'label' }, icon(m.ic, 'sm'), m.label), el.value, el.chart, el.cap);
      return el;
    });
    const rangeChips = h('div', { class: 'pill-group' }, [['30m', '30 min'], ['24h', '24 h'], ['14d', '14 days']].map(([k, l]) => h('button', { class: 'chip' + (V.range === k ? ' active' : ''), onclick: () => { V.range = k; renderTab(); } }, l)));
    const note = h('div', { class: 'foot-note' });
    live = { cards, note, sample: null, my };
    const out = [
      h('div', { class: 'grid g3' },
        h('div', { class: 'card', style: { gridColumn: 'span 2' } }, h('div', { class: 'score-wrap' }, scoreRing(eng.stabilityScore, 'STABILITY'),
          h('div', { class: 'stretch' }, h('h2', {}, 'Stability score', simTag(isSim(eng))), h('div', { class: 'sub' }, 'Based on measured incidents and Windows events from this PC. It explains what lowered it — it is not a speed rating.'),
            reasons.length ? h('ul', { class: 'reasons' }, reasons.map(r => h('li', {}, r))) : h('div', { class: 'desc' }, 'No reasons reported.')))),
        h('div', { class: 'card' }, h('h3', {}, 'Engine'), h('div', { class: 'kv' },
          h('div', {}, 'Mode'), h('div', {}, human(eng.mode || 'monitor'), eng.simulation ? h('span', { class: 'tag' }, 'simulation') : null),
          h('div', {}, 'Open incidents'), h('div', {}, String((eng.counts || {}).incidentsOpen ?? '—')),
          h('div', {}, 'Active policies'), h('div', {}, String((eng.counts || {}).policiesActive ?? '—')),
          h('div', {}, 'Recommendations'), h('div', {}, String((eng.counts || {}).recommendations ?? '—')),
          h('div', {}, 'Recovery breaker'), h('div', {}, eng.heal && eng.heal.breakerOpen ? h('span', { class: 'tag bad', style: { marginLeft: 0 } }, 'open — paused') : 'closed'),
          h('div', {}, 'Last tick'), h('div', {}, eng.engine ? `${fmtTime(eng.engine.lastTickMs)} · ${fmtNum((eng.engine.tickCostUs || 0) / 1000, 2)} ms` : '—')),
          btn('Change mode', () => { V.tab = 'optimise'; tabsSlot.replaceChildren(tabsBar()); renderTab(); }, { small: true, ic: 'sliders' }))),
      h('div', { class: 'section-title' }, 'Measurements', simTag(isSim(bl)), h('div', { class: 'spacer' }), rangeChips),
      h('div', { class: 'grid g5c' }, cards.map(c => c.card)),
      h('div', { class: 'legend', style: { marginTop: '8px' } }, h('span', {}, h('span', { class: 'sw band-sw' }), 'usual range on this PC (median ± MAD)'), h('span', {}, h('span', { class: 'sw median-sw' }), 'median')),
      note,
      h('div', { class: 'section-title' }, 'Recent incidents'),
      h('div', { class: 'card' }, !inc ? emptyState('Incidents are not available.', 'info', false) : (inc.incidents || []).length ? incidentList(inc.incidents) : emptyState('No incidents. OpenSweep Core has not found anything unusual.')),
    ];
    loadHistory(my);
    return out;
  }
  async function loadHistory(my) {
    const L = live; if (!L) return;
    const res = await Promise.all(L.cards.map(c => read('coreHistory', { metric: c.m.id, range: V.range }).then(r => ({ r }), e => ({ e }))));
    if (stale(my) || live !== L) return;
    res.forEach((x, i) => { const c = L.cards[i]; if (x.e) { c.err = coreErr(x.e); c.points = []; } else { c.err = null; c.points = x.r.points || []; c.unit = x.r.unit || c.m.unit; c.sim = !!x.r.simulated; } });
    L.loadedAt = Date.now();
    drawLive();
    tick();
  }
  function drawLive() {
    const L = live; if (!L) return;
    const smp = L.sample;
    for (const c of L.cards) {
      const b = c.b, unit = c.m.unit;
      const last = c.points.length ? c.points[c.points.length - 1][1] : null;
      const cur = smp && Number.isFinite(smp[c.m.id]) ? smp[c.m.id] : last;
      let max = c.m.max || null;
      if (c.m.id === 'commitTotal' && smp && smp.commitLimit) max = smp.commitLimit;
      c.value.replaceChildren(fmtUnit(cur, unit), c.m.id === 'commitTotal' && smp && smp.commitLimit ? h('small', {}, ` ${pct(cur, smp.commitLimit)} %`) : null);
      if (c.m.id === 'commitTotal') c.value.title = smp && smp.commitLimit ? `${fmt(cur)} of a ${fmt(smp.commitLimit)} commit limit` : '';
      const band = b && b.ready && Number.isFinite(b.median) && Number.isFinite(b.mad) ? [Math.max(0, b.median - b.mad), b.median + b.mad] : null;
      c.chart.replaceChildren(c.err ? h('div', { class: 'desc' }, `History not available: ${c.err}`) : coreChart(c.points, { band, median: band ? b.median : null, max, color: c.m.color }));
      c.cap.replaceChildren(c.m.id === 'commitTotal' && smp && smp.commitLimit ? h('div', {}, `of ${fmt(smp.commitLimit)} limit`) : null, !b ? 'No baseline yet' : b.ready ? `Usual ${fmtUnit(b.median, unit)} ± ${fmtUnit(b.mad, unit)}` : h('span', { class: 'tag', style: { marginLeft: 0 }, title: 'Baselines need 120 samples before they are used for decisions' }, `learning (${Math.min(120, b.samples || 0)}/120)`));
    }
  }
  async function tick() {
    const L = live; if (!L) return;
    let smp;
    try { smp = await read('coreSample'); } catch (e) { if (L === live) L.note.textContent = `Could not read the latest sample: ${coreErr(e)}`; return; }
    if (L !== live) return;
    L.sample = smp;
    const ts = Number(smp.tsMs) || Date.now();
    if (V.range === '30m') for (const c of L.cards) {
      if (!Number.isFinite(smp[c.m.id]) || c.err) continue;
      const lastTs = c.points.length ? c.points[c.points.length - 1][0] : 0;
      if (ts > lastTs) { c.points.push([ts, smp[c.m.id]]); while (c.points.length && c.points[0][0] < ts - 30 * 60e3) c.points.shift(); }
    } else if (Date.now() - (L.loadedAt || 0) > 60e3) { loadHistory(L.my); return; }
    drawLive();
    L.note.textContent = `${isSim(smp) ? 'Simulated' : 'Measured by OpenSweep Core'} · sample ${new Date(ts).toLocaleTimeString()} · ${V.range === '30m' ? 'updated every 2 seconds' : 'history refreshed every minute'}`;
  }
  // Phase 1 service (no `status` command): the basic metrics view.
  async function phase1Overview() {
    const m = await read('coreMetrics');
    const mem = m.memory || {}, cp = pct(mem.commitTotal, mem.commitLimit), cpu = Math.max(0, Math.min(100, Number((m.cpu || {}).percent) || 0));
    return [h('div', { class: 'grid g3' },
      kpi('CPU', h('span', {}, cpu.toFixed(1), h('small', {}, ' %')), '', 'cpu'),
      kpi('Physical memory', h('span', {}, String(mem.loadPercent ?? '—'), h('small', {}, ' %')), '', 'memory', h('div', { class: 'desc' }, `${fmt(mem.totalPhys - mem.availPhys)} of ${fmt(mem.totalPhys)} in use`)),
      kpi('Commit charge', h('span', {}, String(cp), h('small', {}, ` %  ·  ${fmt(mem.commitTotal)} of ${fmt(mem.commitLimit)}`)), '', 'layers', meter(cp, 80, 92))),
    h('div', { class: 'foot-note' }, `${isSim(m) ? 'Simulated' : 'Measured by OpenSweep Core'} · ${new Date(m.timestamp || Date.now()).toLocaleTimeString()} · press Refresh to update`)];
  }

  // ---- Incidents
  function incidentList(list) {
    return h('div', { class: 'list' }, list.map(x => h('div', { class: 'item clickable', role: 'button', tabindex: '0', onclick: () => incidentDrawer(x), onkeydown: (e) => { if (e.key === 'Enter') incidentDrawer(x); } },
      h('span', { class: 'sev ' + (x.state === 'resolved' ? 'low' : x.risk === 'high' ? 'high' : 'medium') }),
      h('div', { class: 'body' }, h('div', { class: 'title' }, x.title || human(x.kind), x.state === 'resolved' ? h('span', { class: 'tag good' }, 'resolved') : h('span', { class: 'tag warn' }, 'open'), simTag(isSim(x))),
        h('div', { class: 'desc' }, `${human(x.kind)} · ${fmtTime(tsOf(x))} · confidence ${x.confidenceLabel || '—'}${Number.isFinite(x.confidence) ? ` (${Math.round(x.confidence * 100)} %)` : ''}`)),
      icon('chevron'))));
  }
  function incidentDrawer(x) {
    const unit = (e) => e.unit;
    const sec = (title, ...kids) => [h('h4', {}, title), ...kids];
    const d = h('dialog', { class: 'drawer', 'aria-label': x.title || 'Incident' });
    const close = () => { d.close(); d.remove(); };
    d.addEventListener('close', () => d.remove());
    d.append(
      h('div', { class: 'dh' }, h('div', { class: 'stretch' }, h('h3', {}, x.title || human(x.kind)),
        h('div', { class: 'row' }, x.state === 'resolved' ? h('span', { class: 'tag good', style: { marginLeft: 0 } }, 'resolved') : h('span', { class: 'tag warn', style: { marginLeft: 0 } }, 'open'), riskTag(x.risk),
          h('span', { class: 'tag' }, `confidence ${x.confidenceLabel || '—'}${Number.isFinite(x.confidence) ? ` · ${Math.round(x.confidence * 100)} %` : ''}`), simTag(isSim(x)))),
        h('button', { class: 'btn ghost icon-only', title: 'Close', onclick: close }, icon('x'))),
      h('div', { class: 'db' },
        h('div', { class: 'desc' }, `${human(x.kind)} · opened ${fmtTime(tsOf(x))}${x.resolvedAt ? ` · resolved ${fmtTime(x.resolvedAt)}` : ''}${x.subject ? ` · ${targetName(x.subject)}` : ''}`),
        h('div', { class: 'banner info' }, icon('info'), 'OpenSweep Core reports what happened at the same time (correlation). It does not prove what caused it.'),
        sec('Symptoms', (x.symptoms || []).length ? h('ul', {}, x.symptoms.map(s => h('li', {}, s))) : h('div', { class: 'desc' }, 'None recorded.')),
        sec('Evidence', (x.evidence || []).length ? h('div', { class: 'table-wrap' }, h('table', {}, h('thead', {}, h('tr', {}, h('th', {}, 'Measurement'), h('th', { class: 'num' }, 'Observed'), h('th', { class: 'num' }, 'Usual'), h('th', {}, 'Window'))),
          h('tbody', {}, x.evidence.map(e => h('tr', {}, h('td', {}, human(e.metric), e.note ? h('div', { class: 'desc' }, e.note) : null), h('td', { class: 'num' }, fmtUnit(e.observed, unit(e))),
            h('td', { class: 'num' }, e.baseline === undefined ? '—' : `${fmtUnit(e.baseline, unit(e))}${Number.isFinite(e.mad) ? ` ± ${fmtUnit(e.mad, unit(e))}` : ''}`), h('td', {}, e.window || '—')))))) : h('div', { class: 'desc' }, 'None recorded.')),
        sec('Correlated with', (x.contributors || []).length ? h('div', { class: 'bars' }, x.contributors.map(c => h('div', { class: 'contrib' },
          h('div', { class: 'row nowrap' }, h('b', { class: 'trunc' }, c.name), c.kind ? h('span', { class: 'tag' }, c.kind) : null, h('div', { class: 'spacer' }), h('span', { class: 'meta num' }, `${Math.round((c.confidence || 0) * 100)} %`)),
          h('div', { class: 'meter thin' }, h('div', { style: { width: `${Math.round(Math.max(0, Math.min(1, c.confidence || 0)) * 100)}%` } })),
          c.detail ? h('div', { class: 'desc' }, c.detail) : null))) : h('div', { class: 'desc' }, 'No contributor identified.')),
        (x.possibleCauses || []).length ? sec('Possible causes', h('ul', {}, x.possibleCauses.map(s => h('li', {}, s)))) : null,
        sec('Recommended actions', (x.recommendedActions || []).length ? h('ul', {}, x.recommendedActions.map(s => h('li', {}, s))) : h('div', { class: 'desc' }, 'None.')),
        sec('Automatic recovery', h('div', { class: 'row nowrap' }, stateDot(x.autoRecoveryAllowed ? 'on' : 'off', x.autoRecoveryAllowed ? 'Allowed' : 'Not allowed'),
          h('div', { class: 'stretch' }, x.autoRecoveryAllowed ? 'Allowed — OpenSweep Core may recover this on its own (OpenSweep-owned components only).' : 'Not allowed.', x.autoRecoveryReason ? h('div', { class: 'desc' }, x.autoRecoveryReason) : null)))));
    document.body.append(d);
    d.showModal();
  }
  async function tabIncidents(my) {
    const r = await read('coreIncidents', { limit: 100 }).catch(e => ({ _e: e }));
    if (stale(my)) return null;
    if (r._e) return unavailableCard('The incident list', r._e);
    const list = r.incidents || [];
    return h('div', { class: 'card' }, h('div', { class: 'card-head' }, h('div', { class: 't' }, h('h3', {}, 'Incidents', simTag(isSim(r))), h('div', { class: 'sub' }, 'Unusual patterns found by comparing measurements with this PC’s own baselines and Windows events. Click one for the evidence.'))),
      list.length ? incidentList(list) : emptyState('No incidents recorded.'));
  }

  // ---- Optimisation: mode, recommendations, active policies, policy history
  async function tabOptimise(my) {
    const [rec, pol, jr] = await Promise.all([read('coreRecommendations').catch(e => ({ _e: e })), read('corePolicies').catch(e => ({ _e: e })), read('coreJournal', { limit: 200 }).catch(e => ({ _e: e }))]);
    if (stale(my)) return null;
    const eng = engine() || {}, mode = eng.mode || 'monitor', monitor = mode === 'monitor';
    const modes = h('div', { class: 'grid g3' }, MODE_INFO.map(m => h('div', { class: 'preset mode-card' + (m.id === mode ? ' current' : '') },
      icon(m.ic), h('div', { class: 't' }, m.t, m.id === mode ? h('span', { class: 'tag rec' }, 'current') : null), h('div', { class: 'd' }, m.d),
      m.warn ? h('div', { class: 'warn-note' }, icon('alert', 'sm'), m.warn) : null,
      m.id === mode ? null : btn(`Switch to ${m.t}`, (e) => admin(e.currentTarget, 'coreSetMode', { mode: m.id }, (r) => toast(`OpenSweep Core is now in ${human(r.mode || m.id)} mode`)), { small: true, kind: m.id === 'advanced' ? 'danger' : '', disabled: !canAdmin(), title: canAdmin() ? null : 'Needs OpenSweep running as administrator' }))));
    const active = pol._e ? null : pol.active || [];
    const recs = rec._e ? null : rec.items || [];
    const hist = jr._e ? null : (jr.entries || []).filter(t => t.kind !== 'recovery');
    return [
      h('div', { class: 'section-title' }, 'Mode'), modes,
      !canAdmin() ? h('div', { class: 'banner' }, icon('lock'), 'Changing the mode, rolling back or approving needs OpenSweep running as administrator. Reading is always allowed.') : null,
      h('div', { class: 'section-title' }, 'Recommendations', monitor ? h('span', { class: 'tag warn', title: SIM_TIP }, 'Simulation — nothing is applied in Monitor mode') : null, simTag(isSim(rec) && !monitor)),
      h('div', { class: 'card' }, recs === null ? emptyState(`Recommendations are not available: ${coreErr(rec._e)}`, 'info', false) : recs.length ? h('div', { class: 'list' }, recs.map(r => h('div', { class: 'item' },
        h('span', { class: 'app-ico' }, icon(r.area === 'memory' ? 'memory' : 'cpu')),
        h('div', { class: 'body' }, h('div', { class: 'title' }, `${actionLabel(r.action)} → ${targetName(r.target)}`, r.workload ? h('span', { class: 'tag' }, r.workload) : null, riskTag(r.risk)),
          h('div', { class: 'desc' }, r.reason), r.expectedEffect && r.expectedEffect !== '—' ? h('div', { class: 'desc' }, h('b', {}, 'Expected: '), r.expectedEffect) : null),
        h('div', { class: 'rec-verdict' }, r.wouldApply ? h('span', { class: 'tag good', style: { marginLeft: 0 } }, monitor ? 'would apply (simulated)' : 'would apply') : h('span', { class: 'tag', style: { marginLeft: 0 } }, 'blocked'),
          !r.wouldApply && r.blockedBy ? h('div', { class: 'desc' }, r.blockedBy) : null)))) : emptyState('No recommendations — OpenSweep Core decided nothing needs to change.')),
      h('div', { class: 'section-title' }, 'Active policies', simTag(isSim(pol))),
      h('div', { class: 'card' }, h('div', { class: 'card-head' }, h('div', { class: 't' }, h('div', { class: 'sub' }, 'Changes currently applied by OpenSweep Core. Each is rolled back automatically if it does not help, when the service stops, and after a crash.')),
        btn('Roll back everything', (e) => admin(e.currentTarget, 'coreRollbackAll', {}, (r) => toast(`Rolled back ${r.restored ?? 0} polic${r.restored === 1 ? 'y' : 'ies'}${r.failed ? ` · ${r.failed} failed` : ''}`, !!r.failed)), { small: true, kind: 'danger', ic: 'undo', disabled: !active || !active.length || !canAdmin() })),
        active === null ? emptyState(`Policies are not available: ${coreErr(pol._e)}`, 'info', false) : active.length ? h('div', { class: 'list' }, active.map(p => h('div', { class: 'item' },
          stateDot(p.state === 'verified' ? 'on' : 'busy', human(p.state)),
          h('div', { class: 'body' }, h('div', { class: 'title' }, `${actionLabel(p.kind)} → ${targetName(p.target)}`, txTag(p.state), simTag(isSim(p))),
            h('div', { class: 'desc' }, `Value ${fmtNum(p.value, 0)} (was ${fmtNum(p.original, 0)}) · applied ${fmtTime(p.appliedAt)}${p.verifyUntil && p.verifyUntil > Date.now() ? ` · measuring until ${fmtTime(p.verifyUntil)}` : ''} · transaction ${p.txId}`)),
          btn('Disable', (e) => admin(e.currentTarget, 'coreDisablePolicy', { txId: p.txId }, (r) => toast(r.restored ? 'Policy rolled back' : 'The policy could not be restored (the process may have exited)', !r.restored)), { small: true, disabled: !canAdmin() })))) : emptyState(monitor ? 'No active policies — Monitor mode never applies any.' : 'No active policies.')),
      h('div', { class: 'section-title' }, 'Policy history', simTag(isSim(jr))),
      h('div', { class: 'card flush' }, hist === null ? emptyState(`The journal is not available: ${coreErr(jr._e)}`, 'info', false) : journalTable(hist, 'No policy changes recorded yet.')),
    ];
  }
  function journalTable(list, empty) {
    if (!list.length) return emptyState(empty);
    return h('div', { class: 'table-wrap' }, h('table', {}, h('thead', {}, h('tr', {}, h('th', {}, 'Time'), h('th', {}, 'Change'), h('th', {}, 'Target'), h('th', {}, 'State'), h('th', {}, 'Why / outcome'))),
      h('tbody', {}, list.map(t => h('tr', {}, h('td', { class: 'nowrap' }, fmtTime(tsOf(t))), h('td', {}, actionLabel(t.action || t.kind), t.value !== undefined && t.kind !== 'recovery' ? h('div', { class: 'desc mono' }, `${t.original !== undefined ? `${t.original} → ` : ''}${t.value}`) : null),
        h('td', {}, targetName(t.target)), h('td', {}, txTag(t.state), simTag(isSim(t) && t.state !== 'simulated')), h('td', {}, h('div', { class: 'desc' }, t.reason || ''), t.outcome ? h('div', { class: 'desc' }, h('b', {}, 'Measured: '), t.outcome) : null))))));
  }

  // ---- Recovery (SweepHeal)
  async function tabRecovery(my) {
    const r = await read('coreHeal').catch(e => ({ _e: e }));
    if (stale(my)) return null;
    if (r._e) return unavailableCard('Recovery', r._e);
    const b = r.breaker || {}, pending = r.pending || [];
    return [
      h('div', { class: 'card' }, h('div', { class: 'card-head' }, stateDot(b.open ? 'fail' : 'on', b.open ? 'Breaker open' : 'Breaker closed'),
        h('div', { class: 't' }, h('h3', {}, b.open ? 'Automatic recovery is paused (circuit breaker open)' : 'Automatic recovery is available', simTag(isSim(r))),
          h('div', { class: 'sub' }, b.open ? `Opened ${fmtTime(b.openedAt)} after ${b.failures ?? '?'} failed attempts. It closes again after a quiet period.` : `OpenSweep Core recovers only its own components on its own (${b.failures ?? 0} recent failure${b.failures === 1 ? '' : 's'}). Anything else needs your approval.`)))),
      h('div', { class: 'section-title' }, 'Waiting for your approval'),
      h('div', { class: 'card' }, pending.length ? h('div', { class: 'list' }, pending.map(a => h('div', { class: 'item' }, h('span', { class: 'app-ico' }, icon('repair')),
        h('div', { class: 'body' }, h('div', { class: 'title' }, `${actionLabel(a.action)} → ${targetName(a.target)}`, riskTag(a.risk), simTag(isSim(a))), h('div', { class: 'desc' }, a.reason || ''),
          h('div', { class: 'desc' }, `Requested ${fmtTime(tsOf(a))}${a.expiresAt ? ` · expires ${fmtTime(a.expiresAt)}` : ''}`)),
        btn('Approve', (e) => admin(e.currentTarget, 'coreApprove', { id: a.id }, (x) => toast(`Approved — transaction ${x.txId ?? '?'}`)), { small: true, kind: 'primary', disabled: !canAdmin() })))) : emptyState('Nothing is waiting for approval.')),
      h('div', { class: 'section-title' }, 'Recovery history'),
      h('div', { class: 'card flush' }, journalTable(r.history || [], 'No recovery actions recorded.')),
    ];
  }

  // ---- Events
  async function tabEvents(my) {
    const r = await read('coreEvents', { limit: 200 }).catch(e => ({ _e: e }));
    if (stale(my)) return null;
    if (r._e) return unavailableCard('The event list', r._e);
    const lv = { 1: ['critical', 'bad'], 2: ['error', 'bad'], 3: ['warning', 'warn'], 4: ['info', 'info'] };
    const list = r.events || [];
    return h('div', { class: 'card flush' }, h('div', { class: 'card-head', style: { padding: '14px 16px 0' } }, h('div', { class: 't' }, h('h3', {}, 'Stability events', simTag(isSim(r))), h('div', { class: 'sub' }, 'Crashes, hangs, service failures, hardware and update events collected from Windows logs. Only names and short summaries are kept.'))),
      list.length ? h('div', { class: 'table-wrap' }, h('table', {}, h('thead', {}, h('tr', {}, h('th', {}, 'Time'), h('th', {}, 'Kind'), h('th', {}, 'Subject'), h('th', {}, 'Summary'), h('th', {}, 'Source'))),
        h('tbody', {}, list.map(e => { const [l, c] = lv[e.level] || ['info', 'info']; return h('tr', {}, h('td', { class: 'nowrap' }, fmtTime(tsOf(e))), h('td', { class: 'nowrap' }, h('span', { class: `tag ${c}`, style: { marginLeft: 0 }, title: l }, human(e.kind))),
          h('td', {}, e.subject || '—'), h('td', {}, h('div', { class: 'desc' }, e.message || '')), h('td', { class: 'mono' }, `${e.provider || ''}${e.eventId ? ` ${e.eventId}` : ''}`)); })))) : emptyState('No stability events recorded.'));
  }

  // ---- System: service health, topology, CPU sets, diagnostics
  async function tabSystem(my) {
    const t = await read('coreTopology').catch(() => topo);
    if (stale(my)) return null;
    if (t) topo = t;
    const health = st.health || {}, hello = st.hello || {}, eng = engine();
    const sets = (topo && topo.cpuSets) || [];
    const classes = [...new Set(sets.map(c => c.efficiencyClass))].sort((a, b) => b - a);
    const maxClass = classes.length ? classes[0] : 0;
    return [
      h('div', { class: 'section-title' }, 'Service health', simTag(!!st.demo)),
      h('div', { class: 'grid g4' },
        kpi('Uptime', fmtDur((health.uptimeSec ?? hello.uptimeSec ?? 0) + (Date.now() - statusAt) / 1000), '', 'history', hello.startedAt ? h('div', { class: 'desc' }, `since ${new Date(hello.startedAt).toLocaleString()}`) : null),
        kpi('Service RAM', fmt(health.workingSetBytes), '', 'memory', h('div', { class: 'desc' }, `${fmt(health.privateBytes)} private`)),
        kpi('Service CPU time', health.cpuTimeMs === undefined ? '—' : `${(health.cpuTimeMs / 1000).toFixed(1)} s`, '', 'cpu', h('div', { class: 'desc' }, eng && eng.engine ? `${(eng.engine.ticks || 0).toLocaleString()} engine ticks` : '')),
        kpi('Engine tick cost', eng && eng.engine && Number.isFinite(eng.engine.tickCostUs) ? `${fmtNum(eng.engine.tickCostUs / 1000, 2)} ms` : '—', '', 'activity', h('div', { class: 'desc' }, `every 2 s · ${health.requests === undefined ? '—' : health.requests.toLocaleString()} requests · ${health.errors ?? 0} error(s)`))),
      h('div', { class: 'gap-16' }),
      h('div', { class: 'grid g2', style: { alignItems: 'start' } },
        h('div', { class: 'card' }, h('h3', {}, 'Processor topology', simTag(isSim(topo))), topo ? h('div', { class: 'kv' },
          h('div', {}, 'Logical processors'), h('div', {}, String(topo.logicalProcessors)),
          h('div', {}, 'Physical cores'), h('div', {}, String(topo.cores)),
          h('div', {}, 'Packages (sockets)'), h('div', {}, String(topo.packages)),
          h('div', {}, 'NUMA nodes'), h('div', {}, String(topo.numaNodes)),
          h('div', {}, 'Processor groups'), h('div', {}, String(topo.groups)),
          h('div', {}, 'Hybrid (P/E cores)'), h('div', {}, topo.hybrid ? `Yes — ${(topo.efficiencyClasses || []).map((n, i) => `${n} in class ${i}`).join(', ')}` : 'No'),
          h('div', {}, 'L3 caches'), h('div', {}, String(topo.l3Caches))) : emptyState('Topology not available.', 'info', false)),
        h('div', { class: 'card' }, h('h3', {}, 'CPU sets', simTag(isSim(topo))), sets.length ? [
          h('div', { class: 'sub' }, topo.hybrid ? 'Higher efficiency class = faster (performance) cores. OpenSweep only ever moves background work to the lowest class, in Advanced mode, with your approval.' : 'All cores are the same type, so OpenSweep never uses CPU sets on this PC.'),
          ...classes.map(cl => h('div', { class: 'cpuset-group' }, h('div', { class: 'desc' }, topo.hybrid ? `${cl === maxClass ? 'Performance' : 'Efficiency'} cores · class ${cl}` : `Class ${cl}`),
            h('div', { class: 'cpusets' }, sets.filter(c => c.efficiencyClass === cl).map(c => h('span', { class: 'cpuset' + (topo.hybrid && cl !== maxClass ? ' e' : ''), title: `CPU set ${c.id} · group ${c.group} · core ${c.core} · NUMA ${c.numa}` }, String(c.id))))))]
          : emptyState('CPU sets are not reported by this OpenSweep Core version.', 'info', false))),
      h('div', { class: 'section-title' }, 'Diagnostics'),
      h('div', { class: 'card' }, h('div', { class: 'row' }, h('div', { class: 'stretch' }, h('div', { class: 'title' }, 'Export diagnostics'), h('div', { class: 'desc' }, 'Writes one local JSON file (status, incidents, events, baselines, journal) to the Core data folder. Nothing is sent anywhere.')),
        btn('Export', (e) => admin(e.currentTarget, 'coreExportDiagnostics', {}, (r) => toast(`Diagnostics written to ${r.path}${isSim(r) ? ' (simulated)' : ''}`)), { small: true, ic: 'download', disabled: !canAdmin() || !eng }))),
    ];
  }

  // ------------------------------------------------------------ refresh loops
  async function refresh(full) {
    const s = await call('coreStatus');
    const wasRunning = running(), hadEngine = !!engine();
    st = s; statusAt = Date.now();
    if (full || wasRunning !== running() || hadEngine !== !!engine() || !headSlot.isConnected) draw();
    else { headSlot.replaceChildren(headCard()); tabsSlot.replaceChildren(tabsBar()); if (V.tab !== 'overview') renderTab(); }
  }
  wrap.append(h('div', { class: 'card' }, loading('Checking OpenSweep Core…')));
  await refresh(true);
  S.timers.push(setInterval(() => { if (V.tab === 'overview') tick(); }, 2000));
  // Service status every 10 s; not while an action is in progress or a drawer is open.
  S.timers.push(setInterval(async () => {
    if (wrap.querySelector('button:disabled .spinner') || document.querySelector('dialog.drawer[open]')) return;
    const s = await window.api.call('coreStatus').catch(() => null); if (!s || S.page !== 'core') return;
    const wasRunning = running(), hadEngine = !!engine();
    st = s; statusAt = Date.now();
    if (wasRunning !== running() || hadEngine !== !!engine()) draw(); else { headSlot.replaceChildren(headCard()); tabsSlot.replaceChildren(tabsBar()); }
  }, 10000));
  S.onLeave = () => document.querySelectorAll('dialog.drawer').forEach(d => d.remove());
};
