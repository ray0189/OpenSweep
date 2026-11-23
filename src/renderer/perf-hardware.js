'use strict';
/* Performance page → Hardware check tab. Uses helpers from app.js (h, icon, btn, call, toast, busy,
   confirmDlg, loading, reportTweaks, $, S, go). The page shell (perf-page.js) calls PERF_HARDWARE(container). */

(function () {
  const STATE = {
    ok: ['on', 'check', 'Looks good'],
    warn: ['fail', 'alert', 'Holding your PC back'],
    bad: ['fail', 'x', 'Problem'],
    unknown: ['off', null, 'Could not be checked'],
  };
  // Status circle in the app's style: ✓ ok, ! warn, ✗ bad, ? unknown.
  function hwDot(st) {
    const [cls, ic, tip] = STATE[st] || STATE.unknown;
    const el = h('span', { class: 'state ' + cls, title: tip, role: 'img', 'aria-label': tip }, ic ? icon(ic) : '?');
    if (st === 'bad') Object.assign(el.style, { color: 'var(--bad)', borderColor: 'var(--bad)', background: 'var(--bad-soft)' });
    if (!ic) Object.assign(el.style, { fontSize: '12px', fontWeight: '700' });
    return el;
  }

  // "Keep this refresh rate?" with a countdown, like Windows' own dialog. Always asks (ignores the
  // "confirm actions" setting) and resolves false on timeout, Esc or Revert.
  function keepDialog(text, seconds) {
    const d = $('#dlg'), ok = $('#dlg-ok'), cancel = $('#dlg-cancel'), body = $('#dlg-text');
    const oldCancel = cancel.textContent;
    $('#dlg-title').textContent = 'Keep this refresh rate?';
    ok.textContent = 'Keep'; cancel.textContent = 'Revert';
    return new Promise(res => {
      let left = seconds, timer = null;
      const draw = () => { body.textContent = `${text}\nReverting in ${left} s unless you choose Keep.`; };
      const done = (v) => {
        clearInterval(timer); ok.onclick = cancel.onclick = null; d.oncancel = null; cancel.textContent = oldCancel;
        if (d.open) d.close();
        res(v);
      };
      draw();
      timer = setInterval(() => { left -= 1; if (left <= 0) done(false); else draw(); }, 1000);
      ok.onclick = () => done(true); cancel.onclick = () => done(false);
      d.oncancel = (e) => { e.preventDefault(); done(false); };
      if (d.open) d.close();
      d.showModal();
    });
  }

  window.PERF_HARDWARE = async function (container) {
    let data = S.cache.hwCheck || null;
    const summary = h('div');
    const list = h('div');
    const recheck = btn('Re-check', (e) => load(e.currentTarget), { ic: 'refresh' });
    container.replaceChildren(
      h('div', { class: 'card' }, h('div', { class: 'row' }, summary, h('div', { class: 'spacer' }), recheck)),
      list);

    async function load(b) {
      list.replaceChildren(h('div', { class: 'card' }, loading('Checking memory, monitors, graphics card, drives, power, temperatures and drivers…')));
      summary.replaceChildren(h('div', { class: 'sub' }, 'Checking your hardware…'));
      try { data = S.cache.hwCheck = await (b ? busy(b, () => call('hwCheck'), 'Checking…') : call('hwCheck')); }
      catch (e) { list.replaceChildren(h('div', { class: 'banner bad' }, icon('alert'), 'The hardware check did not run: ' + String(e.message || e).replace(/^Error invoking remote method 'api': (Error: )?/, ''))); summary.replaceChildren(); return; }
      draw();
    }

    function draw() {
      if (!container.isConnected) return;
      const checks = data.checks || [];
      const held = checks.filter(c => c.status === 'warn' || c.status === 'bad').length;
      const unknown = checks.filter(c => c.status === 'unknown').length;
      const demo = data.demo || (S.info && S.info.demo);
      summary.replaceChildren(h('div', {},
        h('h2', {}, held ? `${held} thing${held > 1 ? 's are' : ' is'} holding your PC back` : 'Nothing found that holds your PC back',
          demo ? h('span', { class: 'demo-badge', style: { marginLeft: '8px', verticalAlign: '3px' } }, 'Simulated') : null),
        h('div', { class: 'sub' }, `${checks.length - unknown} of ${checks.length} checks could be read${unknown ? ` · ${unknown} unknown` : ''} · checked ${new Date(data.checkedAt).toLocaleTimeString()} · read-only until you press a fix`)));
      const order = { bad: 0, warn: 1, unknown: 3, ok: 2 };
      list.replaceChildren(...checks.slice().sort((a, b) => order[a.status] - order[b.status]).map(card));
    }

    function card(c) {
      const needs = c.status === 'warn' || c.status === 'bad';
      const items = (c.items || []).length > 1 || (c.items || []).some(i => i.fix) ? h('div', { class: 'list', style: { marginTop: '10px' } }, c.items.map(i => h('div', { class: 'item' }, hwDot(i.status),
        h('div', { class: 'body' }, h('div', { class: 'title' }, i.label), h('div', { class: 'desc' }, i.found, i.expected && i.status !== 'ok' ? ` · expected ${i.expected}` : '')),
        h('div', { class: 'controls' }, i.fix && (i.status !== 'ok' || i.fix.id === 'ssd_retrim') ? fixButton(c, i.fix, i.status === 'ok') : null,
          c.id === 'ssd' && i.lowSpace ? btn('Clean up', () => go('clean'), { small: true, ic: 'sparkles' }) : null)))) : null;
      const itemFixes = (c.items || []).some(i => i.fix);
      const fix = c.fix;
      const showMain = fix && !(itemFixes && fix.kind === 'button') && (needs || fix.optional);
      return h('div', { class: 'card' },
        h('div', { class: 'row' }, hwDot(c.status),
          h('div', { style: { flex: '1', minWidth: '0' } }, h('h3', {}, c.title), h('div', { class: 'sub' }, STATE[c.status] ? STATE[c.status][2] : '')),
          showMain && fix.kind !== 'steps' ? fixButton(c, fix, !needs) : null,
          needs && c.altFix ? fixButton(c, c.altFix, true) : null),
        h('div', { class: 'kv', style: { marginTop: '10px' } }, h('div', {}, 'Found'), h('div', {}, c.found || '—'), h('div', {}, 'Expected'), h('div', {}, c.expected || '—')),
        h('div', { class: 'desc', style: { marginTop: '8px' } }, c.why),
        c.note ? h('div', { class: 'desc', style: { marginTop: '4px', color: 'var(--muted)' } }, c.note) : null,
        items,
        showMain && fix.kind === 'steps' ? h('div', { style: { marginTop: '10px' } }, h('div', { class: 'title' }, fix.label),
          h('ol', { class: 'desc', style: { margin: '6px 0 0', paddingLeft: '20px' } }, fix.steps.map(s => h('li', { style: { marginBottom: '3px' } }, s))),
          /bios/i.test(fix.label) ? h('div', { class: 'desc', style: { marginTop: '6px' } }, 'OpenSweep cannot change BIOS settings for you; these are instructions only.') : null) : null,
        showMain && fix.kind === 'link' && fix.vendorUrl ? h('div', { class: 'desc', style: { marginTop: '6px' } }, `Or get the latest ${fix.vendor} driver from `, h('span', { class: 'mono', style: { userSelect: 'all' } }, fix.vendorUrl)) : null);
    }

    function fixButton(c, f, secondary) {
      if (f.kind === 'link') return btn(f.label, () => call('openUri', { uri: f.url }).catch(() => {}), { small: true, ic: 'external' });
      if (f.kind !== 'button') return null;
      const b = btn(f.label, () => runFix(c, f, b), { small: true, kind: secondary ? '' : 'primary', ic: f.tweak ? 'game' : 'zap' });
      return b;
    }

    async function runFix(c, f, b) {
      if (f.tweak) {
        if (!(await confirmDlg(f.label, `${c.title}: this changes one Windows setting. You can undo it from History & undo or the Optimize page.`, 'Apply'))) return;
        try { const r = await busy(b, () => call('setTweaks', { changes: [{ id: f.tweak, on: true }], label: `${f.label} (Hardware check)` }), 'Applying…'); reportTweaks(r, f.label); } catch { return; }
        return load();
      }
      if (!(await confirmDlg(f.label, f.confirm || `${c.title}: ${f.label}?`, 'Apply'))) return;
      let r;
      try { r = await busy(b, () => call('hwFix', { id: f.id, target: f.target }), f.id === 'ssd_retrim' ? 'Running TRIM…' : 'Applying…'); } catch { return load(); }
      if (f.id === 'refresh_rate') {
        const keep = await keepDialog(`The display now runs at ${r.rate} Hz (was ${r.previous} Hz). If the picture looks wrong or the screen stays black, wait and it comes back.`, 15);
        if (!keep) {
          try { await call('hwFix', { id: 'refresh_revert', target: f.target }); toast(`Refresh rate set back to ${r.previous} Hz`); } catch { /* toast shown */ }
        } else toast(`✓ ${r.summary}`);
      } else toast(`✓ ${r.summary}`);
      return load();
    }

    if (data && data.checks) draw(); else await load();
  };
})();
