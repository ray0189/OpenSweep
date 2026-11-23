'use strict';
/* Performance page (docs/core/alpha4.md): Hardware check (perf-hardware.js), Game boost and Scorecard.
   Uses helpers from app.js, pages.js and core-page.js. Every number shown comes from Windows or
   OpenSweep Core (or is marked Simulated in demo mode); nothing is claimed without Before/Now numbers. */
(() => {
  const read = (m, a) => window.api.call(m, a); // no toast: errors are shown inline
  const errText = (e) => coreErr(e);
  const isUnknownCmd = (e) => /unknown_cmd|unknown command/i.test(errText(e));
  const toMs = (x) => (x === null || x === undefined || x === '' ? 0 : typeof x === 'number' ? x : Date.parse(x) || 0);
  const when = (x) => fmtTime(toMs(x));
  const coreRunning = (st) => !!st && (st.demo ? !!st.running : !!st.available);
  const canAdmin = (st) => !!(st && st.demo) || !!(S.info && S.info.isAdmin);

  // Game boost toast, wherever the user is in the app (the main process sends it on start / stop).
  window.api.on((name, data) => {
    if (name !== 'gameBoost' || !data || !data.event) return;
    if (data.event === 'on') toast(`Game boost on${data.plan && data.prevPlan ? ` · ${data.plan.name} power plan` : ''}`);
    else toast('Game boost off · settings restored');
  });

  // ---- Scorecard metrics: all "lower is better". fmt(v) → text.
  const SC = [
    { id: 'bootMs', label: 'Boot time', hint: 'Median of recent boots (Windows Diagnostics-Performance event 100)', f: (v) => `${fmtNum(v / 1000, 1)} s` },
    { id: 'idleCpuPercent', label: 'CPU use at idle', hint: 'Average while you are not using the PC (CPU below 15 %, no full-screen app)', f: (v) => `${fmtNum(v, 1)} %` },
    { id: 'idleRamUsedBytes', label: 'Memory in use at idle', hint: 'Physical memory in use while idle', f: (v) => fmt(v) },
    { id: 'diskLatencyMs', label: 'Storage latency', hint: 'Average time a read or write takes', f: (v) => `${fmtNum(v, 2)} ms` },
    { id: 'hardFaultsPerSec', label: 'Hard page faults', hint: 'Reads from disk because memory was paged out', f: (v) => `${fmtNum(v, 1)} /s` },
    { id: 'stutterPerHour', label: 'Stutter events', hint: 'Moments a driver held the CPU for 2,000 µs or more (needs kernel tracing)', f: (v) => `${fmtNum(v, 1)} /h` },
    { id: 'crashesPerWeek', label: 'Crashes', hint: 'App crashes, blue screens and unexpected shutdowns', f: (v) => `${fmtNum(v, 1)} /week` },
  ];
  const num = (v) => typeof v === 'number' && Number.isFinite(v);

  PAGES.performance = async (main) => {
    const V = S.cache.perfView || (S.cache.perfView = { tab: 'hardware' });
    const tabsSlot = h('div'), body = h('div');
    let gen = 0, poll = null;
    const stopPoll = () => { if (poll) { clearInterval(poll); S.timers = S.timers.filter(t => t !== poll); poll = null; } };
    const startPoll = (fn, ms) => { stopPoll(); poll = setInterval(() => { if (document.visibilityState === 'visible') fn(); }, ms); S.timers.push(poll); };
    main.append(pageHead('Performance', 'Check that your hardware runs at its rated speed, give games the PC’s full attention, and compare before / after with measured numbers.'),
      tabsSlot, body);

    function drawTabs() {
      tabsSlot.replaceChildren(tabs([['hardware', 'Hardware check', 'cpu'], ['boost', 'Game boost', 'game'], ['scorecard', 'Scorecard', 'pulse']], V.tab, (id) => { V.tab = id; drawTabs(); render(); }));
    }
    async function render() {
      const my = ++gen;
      stopPoll(); S.listeners.gameBoost = [];
      body.replaceChildren(h('div', { class: 'card' }, loading('Loading…')));
      const fn = { hardware: tabHardware, boost: tabBoost, scorecard: tabScorecard }[V.tab] || tabHardware;
      try { await fn(my); }
      catch (e) { if (my === gen) body.replaceChildren(h('div', { class: 'banner bad' }, icon('alert'), `Could not load this tab: ${errText(e)}`)); }
    }
    const stale = (my) => my !== gen;

    // ------------------------------------------------------------ Hardware check (owned by perf-hardware.js)
    async function tabHardware(my) {
      const c = h('div');
      if (typeof window.PERF_HARDWARE !== 'function') {
        body.replaceChildren(h('div', { class: 'card' }, emptyState('The hardware check is not available in this build of OpenSweep.', 'info', false)));
        return;
      }
      body.replaceChildren(c);
      try { await window.PERF_HARDWARE(c); }
      catch (e) { if (!stale(my)) c.replaceChildren(h('div', { class: 'banner bad' }, icon('alert'), `The hardware check failed: ${errText(e)}`)); }
    }

    // ------------------------------------------------------------ Game boost
    async function tabBoost(my) {
      const [st, app0] = await Promise.all([read('coreStatus').catch(() => null), read('gameBoostStatus').catch(() => null)]);
      if (stale(my)) return;
      const demo = !!(S.info && S.info.demo);
      const run = coreRunning(st);
      const appSlot = h('div'), coreSlot = h('div');
      let app = app0, cb = null, cbErr = null;

      // ---- App side: setting + state
      function drawApp() {
        const a = app || { enabled: !!S.settings.gameBoost };
        const tg = toggle(!!a.enabled, async (on, input) => {
          input.disabled = true;
          try { S.settings = await call('setSettings', { patch: { gameBoost: on } }); toast(on ? 'Game boost turned on — it starts when a game runs full screen' : 'Game boost turned off'); }
          catch { /* toast shown */ }
          app = await read('gameBoostStatus').catch(() => app); drawApp();
        });
        const state = !a.enabled ? h('span', { class: 'tag', style: { marginLeft: 0 } }, 'Off')
          : a.boosting ? h('span', { class: 'tag good', style: { marginLeft: 0 } }, 'Boosting now') : h('span', { class: 'tag info', style: { marginLeft: 0 } }, 'Waiting for a full-screen game');
        const rows = [];
        if (a.enabled && a.boosting) {
          rows.push(h('div', {}, 'Since'), h('div', {}, when(a.since)));
          rows.push(h('div', {}, 'Power plan'), h('div', {}, a.plan && a.prevPlan ? `${a.prevPlan.name} → ${a.plan.name} (restored afterwards)` : a.planNote || 'Not changed'));
          rows.push(h('div', {}, 'Paused'), h('div', {}, (a.paused || []).join(', ') || '—'));
        }
        if (a.lastSession) rows.push(h('div', {}, 'Last boost'), h('div', {}, `${when(a.lastSession.start)} – ${when(a.lastSession.end)} (${fmtDur((toMs(a.lastSession.end) - toMs(a.lastSession.start)) / 1000)})${a.lastSession.restoredPlan ? ` · plan restored to ${a.lastSession.restoredPlan}` : ''}`));
        const detection = a.detection === 'demo' ? 'Demo mode: full-screen detection only runs on Windows, so the app-side boost never starts here.'
          : a.detection === 'stopped' && a.enabled ? 'Full-screen detection is starting (or could not start — it retries automatically).' : null;
        appSlot.replaceChildren(h('div', { class: 'card' },
          h('div', { class: 'card-head' }, h('div', { class: 't' }, h('h2', {}, 'Game boost'), h('div', { class: 'sub' }, 'OpenSweep app · works without OpenSweep Core')), state, tg),
          h('div', { class: 'desc' }, `When a game or other app has been full screen for ${(a.startAfterSec || 5)} seconds, OpenSweep switches to the Ultimate or High performance power plan (if Windows has one) and pauses its own background work. ${(a.endAfterSec || 10)} seconds after full screen ends — or when you close OpenSweep — your previous power plan is restored and everything resumes.`),
          rows.length ? h('div', { class: 'kv', style: { marginTop: '12px' } }, rows) : null,
          detection ? h('div', { class: 'foot-note' }, detection) : null));
      }

      // ---- Core side: switch + live status
      function drawCore() {
        if (!run) {
          coreSlot.replaceChildren(h('div', { class: 'card' }, h('div', { class: 'card-head' }, h('div', { class: 't' }, h('h2', {}, 'Background app throttling'), h('div', { class: 'sub' }, 'OpenSweep Core')), h('span', { class: 'tag', style: { marginLeft: 0 } }, 'Core not running')),
            h('div', { class: 'desc' }, 'With OpenSweep Core running, game boost can also put busy background apps into efficiency mode while you play. Without it, the power plan switch and paused scans above still work.'),
            h('div', { class: 'row', style: { marginTop: '12px' } }, btn('Open OpenSweep Core', () => go('core'), { small: true, ic: 'cpu' }))));
          return;
        }
        if (cbErr) {
          coreSlot.replaceChildren(h('div', { class: 'card' }, h('h2', {}, 'Background app throttling'), emptyState(isUnknownCmd(cbErr) ? 'Game boost needs a newer OpenSweep Core. Update OpenSweep to get it.' : `OpenSweep Core did not answer: ${errText(cbErr)}`, 'info', false)));
          return;
        }
        const b = cb || {};
        const admin = canAdmin(st);
        const tg = toggle(!!b.enabled, async (on, input) => {
          input.disabled = true;
          const r = await call('coreSetBoost', { enabled: on }).catch(() => null);
          if (!r || r.cancelled) input.checked = !on; else toast(on ? 'Core game boost turned on' : 'Core game boost turned off');
          await refreshCore();
        }, !admin);
        const status = !b.enabled ? h('span', { class: 'tag', style: { marginLeft: 0 } }, 'Off')
          : b.active ? h('span', { class: 'tag good', style: { marginLeft: 0 } }, 'Active') : h('span', { class: 'tag info', style: { marginLeft: 0 } }, 'Waiting for a full-screen game');
        const thr = Array.isArray(b.throttled) ? b.throttled : [];
        const ls = b.lastSession;
        coreSlot.replaceChildren(h('div', { class: 'card' },
          h('div', { class: 'card-head' }, h('div', { class: 't' }, h('h2', {}, 'Background app throttling', simTag(!!(st && st.demo) || !!b.simulated)), h('div', { class: 'sub' }, 'OpenSweep Core · separate switch, needs administrator')), status, tg),
          !admin ? h('div', { class: 'foot-note' }, 'Turning this on or off needs OpenSweep to run as administrator.') : null,
          h('div', { class: 'kv', style: { marginTop: '4px' } },
            h('div', {}, 'Active'), h('div', {}, b.active ? `Yes, since ${when(b.since)}` : 'No'),
            h('div', {}, 'App in front'), h('div', {}, b.foregroundName ? `${b.foregroundName}${b.foregroundPid ? ` (PID ${b.foregroundPid})` : ''}` : '—'),
            h('div', {}, 'Last session'), h('div', {}, ls ? `${when(ls.start)} – ${when(ls.end)} · ${fmtDur((toMs(ls.end) - toMs(ls.start)) / 1000)} · ${ls.throttledCount} app${ls.throttledCount === 1 ? '' : 's'} throttled, all restored` : 'None yet')),
          h('h3', { style: { marginTop: '16px' } }, `Throttled background apps (${thr.length})`),
          thr.length ? h('div', { class: 'list' }, thr.map(t => h('div', { class: 'item' }, h('div', { class: 'body' }, h('div', { class: 'title' }, t.name), h('div', { class: 'desc' }, `PID ${t.pid}`)), h('span', { class: 'tag info' }, actionLabel(t.kind)))))
            : h('div', { class: 'desc' }, b.active ? 'No background app is using noticeable CPU right now.' : 'None — apps are only throttled while a game is full screen.')));
      }
      async function refreshCore() {
        if (!run) return;
        try { cb = await read('coreBoost'); cbErr = null; } catch (e) { cb = null; cbErr = e; }
        if (!stale(my)) drawCore();
      }

      const explain = h('div', { class: 'card' }, h('h3', {}, 'What game boost does — and never does'),
        h('div', { class: 'grid g2', style: { marginTop: '8px' } },
          h('div', {}, h('div', { class: 'title', style: { marginBottom: '6px' } }, 'It does'), h('ul', { class: 'reasons' },
            h('li', {}, 'Switch to the Ultimate or High performance power plan while a game is full screen, then put your plan back.'),
            h('li', {}, 'Pause OpenSweep’s own background work (stability check, settings guard, scheduled clean) so it never competes with your game.'),
            h('li', {}, 'With OpenSweep Core: put background apps that use noticeable CPU into efficiency mode and lower their memory priority — at most 16, each one journaled and restored when the game ends.'))),
          h('div', {}, h('div', { class: 'title', style: { marginBottom: '6px' } }, 'It never'), h('ul', { class: 'reasons' },
            h('li', {}, 'Touches the game itself or its child processes, audio, anti-cheat, security software or Windows system processes.'),
            h('li', {}, 'Closes or kills apps, overclocks, changes drivers or the BIOS.'),
            h('li', {}, 'Claims a speed-up: use the Scorecard tab to compare measured numbers before and after.')))));

      body.replaceChildren(demo ? h('div', { class: 'banner info' }, icon('info'), h('b', {}, 'Simulated data (demo mode).'), ' The Core status below is made up to show how it works; nothing on this PC changes.') : null,
        demo ? h('div', { class: 'gap-16' }) : null, appSlot, h('div', { class: 'gap-16' }), coreSlot, h('div', { class: 'gap-16' }), explain);
      drawApp(); drawCore();
      await refreshCore();
      if (stale(my)) return;
      S.listeners.gameBoost = [(d) => { if (!stale(my)) { app = d; drawApp(); } }];
      startPoll(async () => {
        if (stale(my)) return stopPoll();
        app = await read('gameBoostStatus').catch(() => app);
        if (!stale(my)) drawApp();
        await refreshCore();
      }, 2000);
    }

    // ------------------------------------------------------------ Scorecard
    async function tabScorecard(my) {
      const st = await read('coreStatus').catch(() => null);
      if (stale(my)) return;
      if (!coreRunning(st)) {
        body.replaceChildren(h('div', { class: 'card' }, emptyState('The scorecard needs OpenSweep Core: it measures boot time, idle load, storage latency, stutter and crashes in the background. Nothing is shown until it has real measurements.', 'cpu', false),
          h('div', { class: 'row', style: { justifyContent: 'center', marginTop: '8px' } }, btn('Open OpenSweep Core', () => go('core'), { small: true, ic: 'cpu' }))));
        return;
      }
      let sc;
      try { sc = await read('coreScorecard'); }
      catch (e) {
        if (stale(my)) return;
        body.replaceChildren(h('div', { class: 'card' }, emptyState(isUnknownCmd(e) ? 'The scorecard needs a newer OpenSweep Core. Update OpenSweep to get it.' : `OpenSweep Core did not answer: ${errText(e)}`, 'info', false)));
        return;
      }
      if (stale(my)) return;
      const sim = !!(st && st.demo) || !!sc.simulated;
      const hours = num(sc.hoursMeasured) ? sc.hoursMeasured : num(sc.days) ? sc.days * 24 : null;
      const hText = hours === null ? '' : `${fmtNum(hours, hours < 10 ? 1 : 0)} h`;
      const base = sc.baseline || null, bm = (base && base.metrics) || {}, cm = (sc.current && sc.current.metrics) || {};
      // A metric without a value: still collecting during the first 24 h, otherwise not measured on this PC.
      const missing = () => h('span', { class: 'desc' }, hours === null || hours < 24 ? `not enough data yet${hText ? ` (${hText})` : ''}` : 'not measured yet');
      const change = (b, c) => {
        if (!num(b) || !num(c)) return h('span', { class: 'desc' }, '—');
        if (b === c || (b === 0 && c === 0)) return h('span', { class: 'desc' }, 'no change');
        const better = c < b; // every scorecard metric: lower is better
        const p = b === 0 ? null : Math.round(((c - b) / Math.abs(b)) * 100);
        const col = better ? 'var(--good)' : 'var(--bad)';
        return h('span', { class: 'row nowrap', style: { gap: '4px', color: col, fontWeight: '600', justifyContent: 'flex-end' }, title: better ? 'Better than before' : 'Worse than before' },
          h('span', { 'aria-hidden': 'true' }, c < b ? '↓' : '↑'), p === null ? (better ? 'lower' : 'higher') : `${p > 0 ? '+' : ''}${p} %`, h('span', { style: { fontWeight: '400', color: 'var(--muted)' } }, better ? 'better' : 'worse'));
      };
      const rows = SC.map(m => h('tr', {},
        h('td', {}, h('div', { style: { fontWeight: '600' } }, m.label), h('div', { class: 'desc' }, m.hint)),
        h('td', { class: 'num' }, !base ? h('span', { class: 'desc' }, 'not captured') : num(bm[m.id]) ? m.f(bm[m.id]) : h('span', { class: 'desc' }, 'not measured')),
        h('td', { class: 'num' }, num(cm[m.id]) ? m.f(cm[m.id]) : missing()),
        h('td', { class: 'num' }, base ? change(bm[m.id], cm[m.id]) : h('span', { class: 'desc' }, '—'))));
      const measured = SC.filter(m => num(bm[m.id]) && num(cm[m.id]));
      const better = measured.filter(m => cm[m.id] < bm[m.id]).length, worse = measured.filter(m => cm[m.id] > bm[m.id]).length;
      const admin = canAdmin(st);
      const capBtn = btn(base ? 'Capture new baseline' : 'Capture baseline', async (e) => {
        const r = await busy(e.currentTarget, () => call('coreCaptureBaseline'), 'Capturing…').catch(() => null);
        if (r && !r.cancelled) { toast('“Before” numbers saved'); render(); }
      }, { small: true, kind: base ? '' : 'primary', ic: 'download', disabled: !admin, title: admin ? null : 'Needs OpenSweep to run as administrator' });
      body.replaceChildren(
        sim ? h('div', { class: 'banner info' }, icon('info'), h('b', {}, 'Simulated data (demo mode).'), ' These numbers are made up to show how the scorecard works.') : null,
        sim ? h('div', { class: 'gap-16' }) : null,
        hours !== null && hours < 24 ? h('div', { class: 'banner' }, icon('info'), `Not enough data yet (${hText} of the 24 h needed). Numbers fill in as OpenSweep Core keeps measuring.`) : null,
        hours !== null && hours < 24 ? h('div', { class: 'gap-16' }) : null,
        h('div', { class: 'card' },
          h('div', { class: 'card-head' }, h('div', { class: 't' }, h('h2', {}, 'Before and now', simTag(sim)),
            h('div', { class: 'sub' }, base ? `“Before” captured ${when(base.capturedAt)} · “Now” = the last 24 hours${hText ? ` · ${hText} measured in total` : ''}` : 'No “Before” numbers yet. They are captured automatically 24 h after OpenSweep Core first starts — or capture them now, before you change anything.')),
            capBtn),
          base && measured.length ? h('div', { class: 'row', style: { gap: '8px', marginBottom: '8px' } },
            h('span', { class: 'tag good', style: { marginLeft: 0 } }, `${better} better`), h('span', { class: 'tag bad', style: { marginLeft: 0 } }, `${worse} worse`),
            measured.length - better - worse ? h('span', { class: 'tag', style: { marginLeft: 0 } }, `${measured.length - better - worse} unchanged`) : null,
            SC.length - measured.length ? h('span', { class: 'tag', style: { marginLeft: 0 } }, `${SC.length - measured.length} not comparable yet`) : null) : null,
          h('div', { class: 'table-wrap' }, h('table', {},
            h('thead', {}, h('tr', {}, h('th', {}, 'Measurement'), h('th', { class: 'num' }, 'Before'), h('th', { class: 'num' }, 'Now'), h('th', { class: 'num' }, 'Change'))),
            h('tbody', {}, rows))),
          h('div', { class: 'foot-note' }, 'Lower is better for every row. Green arrow = better than before, red = worse. A row stays empty until it has been measured on this PC — OpenSweep never fills in estimates.')));
    }

    drawTabs();
    await render();
  };
})();
