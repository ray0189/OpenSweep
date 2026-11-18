'use strict';
/* New pages: Clean everything, Space analyzer, Uninstaller, Reliability, Processes. Uses helpers from app.js. */

// Page-scoped event listeners (cleared when you leave the page).
function onEvent(name, fn) { (S.listeners[name] = S.listeners[name] || []).push(fn); }
const ago2 = (t) => { if (!t) return 'Never'; const d = (Date.now() - new Date(t).getTime()) / 86400e3; if (d < 1) return 'Today'; if (d < 2) return 'Yesterday'; if (d < 31) return `${Math.floor(d)} days ago`; if (d < 60) return '1 month ago'; if (d < 365) return `${Math.floor(d / 30)} months ago`; return `${(d / 365).toFixed(1)} years ago`; };
const fmtRate = (b) => (b >= 1024 * 1024 ? `${(b / 1024 / 1024).toFixed(1)} MB/s` : b >= 1024 ? `${Math.round(b / 1024)} KB/s` : '0');
const kpi = (label, value, cls = '', ic = null, extra = null) => h('div', { class: 'card tight kpi' }, h('div', { class: 'label' }, ic ? icon(ic, 'sm') : null, label), h('div', { class: 'value ' + cls }, value), extra);
const checkAll = (boxes, on) => boxes.forEach(b => { if (b.checked !== on) { b.checked = on; b.dispatchEvent(new Event('change')); } });

// =====================================================================================
// Clean everything
// =====================================================================================
PAGES.clean = async (main) => {
  const body = h('div');
  let scan = S.cache.cleanAll || null;
  const sel = new Set();
  const sched = h('select', { onchange: async (e) => { S.settings = await call('setSettings', { patch: { autoClean: e.target.value } }); toast(e.target.value === 'off' ? 'Scheduled cleaning turned off' : `OpenSweep will clean ${e.target.value} when your PC is idle`); } },
    [['off', 'Off'], ['weekly', 'Every week'], ['monthly', 'Every month']].map(([v, l]) => h('option', { value: v, selected: (S.settings.autoClean || 'off') === v }, l)));
  main.append(pageHead('Clean everything', 'One scan finds junk files, caches, old drivers, leftovers and more. Safe items are ticked for you.',
    h('span', { class: 'sub' }, 'Auto-clean:'), sched, btn('Pick locations…', () => go('cleanup'), { ic: 'list' })), body);

  function intro() {
    body.replaceChildren(h('div', { class: 'card hero', style: { justifyContent: 'center', flexDirection: 'column', textAlign: 'center', padding: '48px 24px' } },
      icon('sparkles', 'xl'), h('h2', { style: { fontSize: '20px' } }, 'Find everything you can safely remove'),
      h('div', { class: 'sub', style: { maxWidth: '520px' } }, 'Temp files, browser and app caches, Windows Update leftovers, crash dumps, old driver packages, old installers, empty folders and more — checked all at once.'),
      h('button', { class: 'btn primary huge', style: { marginTop: '10px' }, onclick: (e) => runScan(e.currentTarget) }, icon('search'), 'Scan my PC')));
  }
  async function runScan(b) {
    body.replaceChildren(h('div', { class: 'card' }, h('div', { class: 'row' }, h('span', { class: 'spinner' }), h('div', {}, h('div', { class: 'title' }, 'Scanning your PC…'), h('div', { class: 'sub' }, 'Checking caches, Windows leftovers and drivers. This takes about a minute.'))), h('div', { class: 'progress indeterminate', style: { marginTop: '14px' } }, h('div')), skeleton(5)));
    try { scan = S.cache.cleanAll = await call('cleanAllScan'); } catch { return intro(); }
    sel.clear(); scan.items.filter(i => i.safe).forEach(i => sel.add(i.id));
    draw();
  }
  function draw() {
    const groups = [...new Set(scan.items.map(i => i.group))];
    const total = () => scan.items.filter(i => sel.has(i.id)).reduce((a, i) => a + i.size, 0);
    const totalEl = h('div', { class: 'big-num' });
    const cleanBtn = h('button', { class: 'btn primary big' }, icon('sparkles'), 'Clean now');
    const refresh = () => { totalEl.replaceChildren(fmt(total()), h('small', {}, `selected of ${fmt(scan.items.reduce((a, i) => a + i.size, 0))} found`)); cleanBtn.disabled = !sel.size; };
    cleanBtn.onclick = () => runClean(cleanBtn);
    const rows = (g) => scan.items.filter(i => i.group === g).map(i => {
      const cb = h('input', { type: 'checkbox', checked: sel.has(i.id), onchange: (e) => { e.target.checked ? sel.add(i.id) : sel.delete(i.id); refresh(); } });
      const detail = h('div', { style: { display: 'none', marginTop: '8px' } }, (i.detail || []).slice(0, 30).map(d => h('div', { class: 'desc mono trunc' }, d.path || `${d.name} ${d.version || ''} · ${d.provider || ''} ${d.date || ''}`, d.size ? ` · ${fmt(d.size)}` : '')));
      return h('label', { class: 'item' }, cb,
        h('div', { class: 'body' }, h('div', { class: 'title' }, i.name, adminTag(i.admin), i.safe ? h('span', { class: 'tag good' }, 'safe') : h('span', { class: 'tag warn', title: i.why || '' }, 'check first')),
          h('div', { class: 'desc' }, i.desc), !i.safe && i.why ? h('div', { class: 'desc', style: { color: 'var(--warn)' } }, i.why) : null,
          i.detail && i.detail.length ? h('button', { class: 'btn ghost small', style: { marginTop: '4px', paddingLeft: 0 }, onclick: (e) => { e.preventDefault(); detail.style.display = detail.style.display === 'none' ? 'block' : 'none'; } }, icon('down'), `Show ${i.detail.length} item(s)`) : null, detail),
        h('div', { class: 'meta num' }, i.size ? fmt(i.size) : '—', i.count ? h('div', { class: 'desc' }, `${i.count.toLocaleString()} item(s)`) : null));
    });
    body.replaceChildren(
      h('div', { class: 'card hero' }, icon('sparkles', 'xl'), h('div', { style: { flex: 1 } }, totalEl, h('div', { class: 'sub' }, `Scanned ${new Date(scan.at).toLocaleTimeString()} · files in use are skipped automatically`)),
        btn('Rescan', (e) => runScan(e.currentTarget), { ic: 'refresh' }), cleanBtn),
      ...groups.map(g => h('div', { class: 'card' }, h('div', { class: 'card-head' }, h('h3', { class: 't' }, g), btn('Select all', () => { scan.items.filter(i => i.group === g).forEach(i => sel.add(i.id)); draw(); }, { small: true, kind: 'ghost' })), h('div', { class: 'list' }, rows(g)))));
    refresh();
  }
  async function runClean(b) {
    const picked = scan.items.filter(i => sel.has(i.id));
    if (!(await confirmDlg('Clean now?', `${picked.length} item(s), about ${fmt(picked.reduce((a, i) => a + i.size, 0))}.\nClose your browsers for the best result. Your own files go to the Recycle Bin.`, 'Clean'))) return;
    const prog = h('div', { class: 'progress' }, h('div', { style: { width: '0%' } }));
    const line = h('div', { class: 'sub' }, 'Starting…');
    body.prepend(h('div', { class: 'card' }, h('div', { class: 'row' }, h('span', { class: 'spinner' }), h('div', { class: 'title' }, 'Cleaning…')), line, h('div', { style: { height: '10px' } }), prog));
    onEvent('clean-progress', (p) => { prog.firstChild.style.width = `${Math.round(100 * p.done / Math.max(1, p.total))}%`; line.textContent = `Done: ${p.name}`; });
    b.disabled = true;
    let r;
    try { r = await call('cleanAllRun', { ids: [...sel] }); } catch { b.disabled = false; return; }
    S.cache.cleanAll = null;
    body.replaceChildren(h('div', { class: 'card hero' }, h('span', { class: 'state on', style: { width: '48px', height: '48px' } }, icon('check', 'lg')),
      h('div', { style: { flex: 1 } }, h('div', { class: 'big-num' }, fmt(r.freed), h('small', {}, 'freed')), h('div', { class: 'sub' }, 'Checked after cleaning. Details below and in History.')), btn('Scan again', (e) => runScan(e.currentTarget), { ic: 'refresh' })),
      h('div', { class: 'card' }, h('div', { class: 'list' }, r.report.map(x => h('div', { class: 'item' }, stateDot(/Failed/.test(x.note || '') ? 'fail' : 'on', x.note), h('div', { class: 'body' }, h('div', { class: 'title' }, x.name), x.note ? h('div', { class: 'desc' }, x.note) : null), h('div', { class: 'meta num' }, x.freed ? fmt(x.freed) : '—'))))));
    toast(`Freed ${fmt(r.freed)}`);
  }
  if (scan) draw(); else intro();
};

// =====================================================================================
// Space analyzer (+ duplicate, large-file and Downloads tools)
// =====================================================================================
const CAT_COLOR = { video: 'var(--c4)', image: 'var(--c3)', audio: 'var(--c10)', archive: 'var(--c5)', diskimage: 'var(--c6)', document: 'var(--c2)', code: 'var(--c8)', program: 'var(--c1)', system: 'var(--c9)', other: 'var(--c9)', folder: 'var(--c1)' };
const CAT_NAME = { video: 'Videos', image: 'Images', audio: 'Audio', archive: 'Archives', diskimage: 'Disk images', document: 'Documents', code: 'Code', program: 'Programs', system: 'System & logs', other: 'Other' };
const EXT_CAT = { video: ['.mp4', '.mkv', '.mov', '.avi', '.wmv', '.webm', '.m4v'], image: ['.jpg', '.jpeg', '.png', '.gif', '.webp', '.heic', '.raw', '.nef', '.psd'], audio: ['.mp3', '.wav', '.flac', '.m4a', '.ogg'], archive: ['.zip', '.rar', '.7z', '.gz', '.tar'], diskimage: ['.iso', '.img', '.vhd', '.vhdx', '.vmdk', '.wim'], document: ['.pdf', '.docx', '.xlsx', '.pptx', '.txt'], program: ['.exe', '.dll', '.msi', '.sys'] };
const catOfName = (n) => { const e = (n.match(/\.[^.\\/]+$/) || [''])[0].toLowerCase(); for (const [k, v] of Object.entries(EXT_CAT)) if (v.includes(e)) return k; return 'other'; };

// Squarified treemap layout: returns [{item, x, y, w, h}]
function squarify(items, x, y, w, hgt) {
  const out = [];
  const total = items.reduce((a, i) => a + i.value, 0);
  if (!total || w <= 0 || hgt <= 0) return out;
  const scale = (w * hgt) / total;
  let rest = items.map(i => ({ item: i, area: i.value * scale })).filter(r => r.area > 0);
  let rx = x, ry = y, rw = w, rh = hgt;
  const worst = (row, side) => { const s = row.reduce((a, r) => a + r.area, 0); const mx = Math.max(...row.map(r => r.area)), mn = Math.min(...row.map(r => r.area)); return Math.max((side * side * mx) / (s * s), (s * s) / (side * side * mn)); };
  while (rest.length) {
    const side = Math.min(rw, rh);
    const row = [rest[0]]; let i = 1;
    while (i < rest.length && worst(row.concat(rest[i]), side) <= worst(row, side)) { row.push(rest[i]); i++; }
    rest = rest.slice(i);
    const s = row.reduce((a, r) => a + r.area, 0);
    if (rw >= rh) { const cw = s / rh; let cy = ry; for (const r of row) { const ch = r.area / cw; out.push({ item: r.item, x: rx, y: cy, w: cw, h: ch }); cy += ch; } rx += cw; rw -= cw; }
    else { const ch = s / rw; let cx = rx; for (const r of row) { const cw = r.area / ch; out.push({ item: r.item, x: cx, y: ry, w: cw, h: ch }); cx += cw; } ry += ch; rh -= ch; }
  }
  return out;
}

PAGES.storage = async (main) => {
  let tab = S.cache.storageTab || 'analyzer';
  const wrap = h('div');
  main.append(pageHead('Space analyzer', 'See exactly what fills your drives, find what you can remove, and track how folders grow.'), wrap);
  const draw = async () => {
    wrap.replaceChildren(tabs([['analyzer', 'Analyzer', 'pie'], ['dupes', 'Duplicates', 'copy'], ['large', 'Large files', 'file'], ['downloads', 'Organise Downloads', 'download']], tab, (id) => { tab = S.cache.storageTab = id; draw(); }));
    const out = h('div'); wrap.append(out);
    if (tab === 'analyzer') await analyzer(out); else await storageTools(out, tab);
  };
  draw();
};

async function analyzer(out) {
  const st = S.cache.disk || (S.cache.disk = { segs: [], view: 'folders', summary: null });
  const drives = await call('diskDrives');
  const target = h('div', { class: 'pill-group' });
  let root = st.summary ? st.summary.root : drives[0].root;
  const drawTargets = () => target.replaceChildren(...drives.map(d => h('button', { class: 'chip' + (d.root === root ? ' active' : ''), onclick: () => { root = d.root; drawTargets(); } }, d.label)),
    h('button', { class: 'chip' + (drives.some(d => d.root === root) ? '' : ' active'), onclick: async () => { const f = await call('chooseFolder'); if (f) { root = f; drives.push({ root: f, label: f }); drawTargets(); } } }, icon('folder', 'sm'), ' Other folder…'));
  drawTargets();
  const scanBtn = h('button', { class: 'btn primary' }, icon('search'), 'Scan');
  const res = h('div');
  out.append(h('div', { class: 'card' }, h('div', { class: 'row' }, h('span', { class: 'sub' }, 'Scan:'), target, h('div', { class: 'spacer' }), scanBtn)), h('div', { class: 'gap-16' }), res);

  scanBtn.onclick = async () => {
    const files = h('span', { class: 'num' }, '0'), size = h('span', { class: 'num' }, '0 B'), cur = h('div', { class: 'desc mono trunc' }, root), time = h('span');
    res.replaceChildren(h('div', { class: 'card' }, h('div', { class: 'row' }, h('span', { class: 'spinner' }), h('div', { class: 'title' }, 'Scanning ', root), h('div', { class: 'spacer' }), btn('Cancel', () => call('diskCancel'), { small: true, ic: 'stop' })),
      h('div', { class: 'grid g3', style: { margin: '14px 0 8px' } }, kpi('Files found', files), kpi('Size so far', size), kpi('Time', time)), cur, h('div', { class: 'progress indeterminate', style: { marginTop: '10px' } }, h('div'))));
    onEvent('disk-progress', p => { files.textContent = p.files.toLocaleString(); size.textContent = fmt(p.bytes); cur.textContent = p.current; time.textContent = `${(p.ms / 1000).toFixed(1)} s`; });
    scanBtn.disabled = true;
    try { st.summary = await call('diskScan', { root }); st.segs = []; } catch { res.replaceChildren(); return; } finally { scanBtn.disabled = false; }
    render();
  };

  async function render() {
    const s = st.summary;
    if (!s) return res.replaceChildren(emptyState('Pick a drive or folder and press Scan.', 'pie', false));
    const views = [['folders', 'Folders', 'folder'], ['types', 'File types', 'layers'], ['largest', 'Largest files', 'file'], ['age', 'File age', 'history'], ['insights', 'Insights', 'bolt', s.insights.length], ['search', 'Search', 'search'], ['history', 'Growth', 'pulse']];
    const viewEl = h('div');
    res.replaceChildren(
      h('div', { class: 'grid g5' }, kpi('Total size', fmt(s.total), '', 'drive'), kpi('Files', s.files.toLocaleString(), '', 'file'), kpi('Folders', s.dirs.toLocaleString(), '', 'folder'),
        kpi('Reclaimable (insights)', fmt(s.insights.reduce((a, i) => a + i.size, 0)), 'good', 'bolt'), kpi('Scan time', `${(s.stats.ms / 1000).toFixed(1)} s`, '', 'zap', s.stats.denied ? h('div', { class: 'desc' }, `${s.stats.denied} folder(s) not readable`) : null)),
      s.cancelled ? h('div', { class: 'banner' }, icon('alert'), 'The scan was cancelled — results are incomplete.') : null,
      h('div', { class: 'gap-16' }),
      tabs(views, st.view, (v) => { st.view = v; render(); }), viewEl);
    await ({ folders, types, largest, age, insights, search, history }[st.view])(viewEl, s);
  }

  // ---- shared actions on paths
  async function act(kind, paths, after) {
    if (!paths.length) return;
    if (kind === 'trash') {
      if (!(await confirmDlg('Move to Recycle Bin?', `${paths.length} item(s) will be moved to the Recycle Bin. You can restore them from there.`, 'Move to Recycle Bin'))) return;
      const r = await call('diskTrash', { paths }); toast(`${r.filter(x => x.ok).length} item(s) moved to the Recycle Bin`);
    } else if (kind === 'move') { const r = await call('diskMove', { paths }); if (r) toast(`${r.filter(x => x.ok).length} item(s) moved`); }
    else if (kind === 'compress') { if (!(await confirmDlg('Compress with NTFS?', 'Files stay usable but take less space. Works best on documents and old files, not on videos or already-compressed files.', 'Compress'))) return; await call('diskCompress', { paths }); toast('Compressed'); }
    st.summary = await call('diskSummary');
    if (after) after(); else render();
  }
  const pathMenu = (e, p, isDir, onOpen) => menu(e, [
    ...(isDir && onOpen ? [{ label: 'Open folder here', icon: 'folder', run: onOpen }] : []),
    { label: 'Show in Explorer', icon: 'external', run: () => call('diskReveal', { path: p }) },
    { label: 'Copy path', icon: 'copy', run: () => { navigator.clipboard.writeText(p); toast('Path copied'); } },
    '-',
    { label: 'Move to another drive…', icon: 'move', run: () => act('move', [p]) },
    { label: 'Compress (NTFS)', icon: 'compress', run: () => act('compress', [p]) },
    { label: 'Move to Recycle Bin', icon: 'trash', danger: true, run: () => act('trash', [p]) },
  ]);

  async function folders(out) {
    const n = await call('diskNode', { segs: st.segs });
    const crumbs = h('div', { class: 'crumbs' }, h('button', { onclick: () => { st.segs = []; render(); } }, st.summary.root),
      st.segs.flatMap((sg, i) => [icon('chevron'), h('button', { onclick: () => { st.segs = st.segs.slice(0, i + 1); render(); } }, sg)]));
    // Treemap
    const tm = h('div', { class: 'treemap' });
    const items = [...n.children.map(c => ({ ...c, value: c.size })), ...n.fileItems.map(f => ({ ...f, value: f.size })), ...(n.rest && n.rest.size ? [{ name: `${n.rest.count} smaller files`, value: n.rest.size, rest: true }] : [])].filter(i => i.value > 0).slice(0, 80);
    requestAnimationFrame(() => {
      const W = tm.clientWidth || 900, H = tm.clientHeight || 380;
      for (const r of squarify(items, 0, 0, W, H)) {
        const it = r.item, col = it.rest ? 'var(--c9)' : it.file ? CAT_COLOR[catOfName(it.name)] : COLORS[items.indexOf(it) % COLORS.length];
        const cell = h('div', { class: 'tm', title: `${it.name} — ${fmt(it.value)}`, style: { left: r.x + 'px', top: r.y + 'px', width: r.w + 'px', height: r.h + 'px', background: col },
          onclick: () => { if (!it.file && !it.rest && it.hasKids) { st.segs = [...st.segs, it.name]; render(); } },
          oncontextmenu: (e) => { e.preventDefault(); if (!it.rest) pathMenu(e, `${n.path}${n.path.endsWith('\\') || n.path.endsWith('/') ? '' : (n.path.includes('\\') ? '\\' : '/')}${it.name}`, !it.file, () => { st.segs = [...st.segs, it.name]; render(); }); } });
        if (r.w > 60 && r.h > 22) cell.append(h('div', { class: 'lbl' }, it.name, h('small', {}, fmt(it.value))));
        // second level inside folders
        if (!it.file && it.kids && r.w > 70 && r.h > 60) {
          const pad = 4, top = 22;
          for (const k of squarify(it.kids.filter(k => k.size > 0).map(k => ({ ...k, value: k.size })), pad, top, r.w - pad * 2, r.h - top - pad)) {
            if (k.w < 3 || k.h < 3) continue;
            cell.append(h('div', { class: 'inner', title: `${k.item.name} — ${fmt(k.item.size)}`, style: { left: k.x + 'px', top: k.y + 'px', width: k.w + 'px', height: k.h + 'px', background: k.item.file ? CAT_COLOR[catOfName(k.item.name)] : 'rgba(255,255,255,.10)' } }));
          }
        }
        tm.append(cell);
      }
    });
    // Tree table with lazy expanding rows
    const tbody = h('tbody');
    const rowFor = (c, segs, depth, parentSize) => {
      const tr = h('tr', { oncontextmenu: (e) => { e.preventDefault(); pathMenu(e, joinPath(st.summary.root, segs.concat(c.name)), !c.file, () => { st.segs = segs.concat(c.name); render(); }); }, ondblclick: () => { if (!c.file && c.hasKids) { st.segs = segs.concat(c.name); render(); } } });
      const exp = h('button', { class: 'exp', 'aria-label': 'Expand', style: { visibility: !c.file && c.hasKids ? 'visible' : 'hidden' } }, icon('chevron'));
      let kids = null;
      exp.onclick = async () => {
        if (kids) { kids.forEach(k => k.remove()); kids = null; exp.classList.remove('open'); return; }
        const sub = await call('diskNode', { segs: segs.concat(c.name) });
        kids = [...sub.children.map(x => rowFor(x, segs.concat(c.name), depth + 1, c.size)), ...sub.fileItems.map(x => rowFor(x, segs.concat(c.name), depth + 1, c.size)), ...(sub.rest ? [restRow(sub.rest, depth + 1, c.size)] : [])];
        let after = tr; for (const k of kids) { after.after(k); after = k; }
        exp.classList.add('open');
      };
      const p = parentSize ? Math.round(100 * c.size / parentSize) : 0;
      tr.append(h('td', {}, h('span', { class: 'tw', style: { paddingLeft: depth * 18 + 'px' } }, exp, icon(c.file ? 'file' : 'folder', 'sm'), h('span', { class: 'name', title: c.name }, c.name), c.denied ? h('span', { class: 'tag warn' }, 'no access') : null)),
        h('td', { class: 'num' }, fmt(c.size)), h('td', {}, h('span', { class: 'pctbar' }, h('i', { style: { width: p + '%', background: c.file ? CAT_COLOR[catOfName(c.name)] : 'var(--accent)' } })), h('span', { class: 'sub num' }, `${p}%`)),
        h('td', { class: 'num' }, c.file ? '' : (c.files || 0).toLocaleString()), h('td', { class: 'num' }, c.file ? '' : (c.dirs || 0).toLocaleString()), h('td', {}, c.mtime ? new Date(c.mtime).toLocaleDateString() : ''),
        h('td', { class: 'num' }, h('button', { class: 'btn ghost small icon-only', title: 'Actions', onclick: (e) => { e.stopPropagation(); pathMenu(e, joinPath(st.summary.root, segs.concat(c.name)), !c.file, () => { st.segs = segs.concat(c.name); render(); }); } }, icon('dots'))));
      return tr;
    };
    const restRow = (r, depth, parentSize) => h('tr', {}, h('td', {}, h('span', { class: 'tw sub', style: { paddingLeft: (depth * 18 + 24) + 'px' } }, `${r.count.toLocaleString()} smaller files`)), h('td', { class: 'num' }, fmt(r.size)), h('td', {}, h('span', { class: 'sub' }, `${parentSize ? Math.round(100 * r.size / parentSize) : 0}%`)), h('td'), h('td'), h('td'), h('td'));
    n.children.forEach(c => tbody.append(rowFor(c, st.segs, 0, n.size)));
    n.fileItems.forEach(f => tbody.append(rowFor(f, st.segs, 0, n.size)));
    if (n.rest) tbody.append(restRow(n.rest, 0, n.size));
    out.append(h('div', { class: 'card' }, h('div', { class: 'card-head' }, h('div', { class: 't' }, crumbs), h('span', { class: 'sub' }, `${fmt(n.size)} · ${n.files.toLocaleString()} files`),
      st.segs.length ? btn('Up', () => { st.segs = st.segs.slice(0, -1); render(); }, { small: true, ic: 'up' }) : null, btn('Export CSV', async () => { const r = await call('diskExport', { segs: st.segs }); if (r) toast(`Saved ${r.path}`); }, { small: true, ic: 'download' })),
      tm, h('div', { class: 'foot-note' }, 'Click a box to open that folder. Right-click anything for actions. Colours show file types.')),
      h('div', { class: 'card flush', style: { marginTop: '12px' } }, h('div', { class: 'table-wrap' }, h('table', { class: 'tree' }, h('thead', {}, h('tr', {}, h('th', {}, 'Name'), h('th', { class: 'num' }, 'Size'), h('th', {}, '% of folder'), h('th', { class: 'num' }, 'Files'), h('th', { class: 'num' }, 'Folders'), h('th', {}, 'Modified'), h('th'))), tbody))));
  }
  function joinPath(root, segs) { const sep = root.includes('\\') ? '\\' : '/'; return [root.replace(/[\\/]+$/, ''), ...segs].join(sep); }

  async function types(out, s) {
    const byCat = {};
    for (const t of s.types) { const c = byCat[t.cat] || (byCat[t.cat] = { size: 0, count: 0 }); c.size += t.size; c.count += t.count; }
    const cats = Object.entries(byCat).sort((a, b) => b[1].size - a[1].size);
    const tot = s.total || 1;
    out.append(h('div', { class: 'card' }, h('h3', {}, 'By category'), h('div', { class: 'stackbar', style: { margin: '10px 0' } }, cats.map(([c, v]) => h('div', { title: `${CAT_NAME[c]} ${fmt(v.size)}`, style: { width: (100 * v.size / tot) + '%', background: CAT_COLOR[c] } }))),
      h('div', { class: 'legend' }, cats.map(([c, v]) => h('span', {}, h('span', { class: 'sw', style: { background: CAT_COLOR[c] } }), `${CAT_NAME[c]} · ${fmt(v.size)}`)))),
      h('div', { class: 'card flush' }, h('div', { class: 'table-wrap' }, h('table', {}, h('thead', {}, h('tr', {}, h('th', {}, 'Type'), h('th', {}, 'Category'), h('th', { class: 'num' }, 'Files'), h('th', { class: 'num' }, 'Size'), h('th', {}, 'Share'))),
        h('tbody', {}, s.types.map(t => h('tr', {}, h('td', { class: 'mono' }, t.ext), h('td', {}, h('span', { class: 'sw', style: { display: 'inline-block', width: '9px', height: '9px', borderRadius: '3px', marginRight: '6px', background: CAT_COLOR[t.cat] } }), CAT_NAME[t.cat]), h('td', { class: 'num' }, t.count.toLocaleString()), h('td', { class: 'num' }, fmt(t.size)),
          h('td', {}, h('span', { class: 'pctbar' }, h('i', { style: { width: (100 * t.size / tot) + '%', background: CAT_COLOR[t.cat] } }))))))))));
  }
  async function fileList(out, files, title, note) {
    const sel = new Set();
    const tb = h('tbody', {}, files.map(f => h('tr', { oncontextmenu: (e) => { e.preventDefault(); pathMenu(e, f.path, false); } },
      h('td', {}, h('input', { type: 'checkbox', onchange: (e) => (e.target.checked ? sel.add(f.path) : sel.delete(f.path)) })), h('td', {}, h('div', { class: 'mono trunc', style: { maxWidth: '620px' }, title: f.path }, f.path)),
      h('td', { class: 'num' }, fmt(f.size)), h('td', {}, f.mtime ? ago2(f.mtime) : ''), h('td', { class: 'num' }, h('button', { class: 'btn ghost small icon-only', onclick: (e) => pathMenu(e, f.path, false) }, icon('dots'))))));
    out.append(h('div', { class: 'card flush' }, h('div', { class: 'card-head', style: { padding: '14px 16px 0' } }, h('div', { class: 't' }, h('h3', {}, title), note ? h('div', { class: 'sub' }, note) : null),
      btn('Move…', () => act('move', [...sel]), { small: true, ic: 'move' }), btn('Recycle Bin', () => act('trash', [...sel]), { small: true, ic: 'trash', kind: 'danger' })),
      files.length ? h('div', { class: 'table-wrap' }, h('table', {}, h('thead', {}, h('tr', {}, h('th'), h('th', {}, 'File'), h('th', { class: 'num' }, 'Size'), h('th', {}, 'Last changed'), h('th'))), tb)) : emptyState('Nothing found.')));
  }
  async function largest(out) { await fileList(out, await call('diskTop'), 'Largest files', 'The 300 biggest files in this scan.'); }
  async function age(out, s) {
    const tot = s.age.reduce((a, x) => a + x.size, 0) || 1;
    const old = s.age.slice(3).reduce((a, x) => a + x.size, 0);
    out.append(h('div', { class: 'card' }, h('div', { class: 'card-head' }, h('div', { class: 't' }, h('h3', {}, 'How old are your files?'), h('div', { class: 'sub' }, `${fmt(old)} (${Math.round(100 * old / tot)}%) hasn't changed in over a year.`))),
      h('div', { class: 'bars' }, s.age.map((a, i) => h('div', { class: 'bar-row' }, h('div', {}, a.label), h('div', { class: 'meter' }, h('div', { style: { width: (100 * a.size / tot) + '%', background: COLORS[i] } })), h('div', { class: 'num', style: { textAlign: 'right' } }, fmt(a.size)))))));
    const files = await call('diskSearch', { minMB: 50, olderDays: 365 });
    const box = h('div'); out.append(h('div', { class: 'gap-16' }), box);
    await fileList(box, files, 'Big files untouched for over a year', 'Files over 50 MB not changed in 12 months — good candidates to archive or move.');
  }
  async function insights(out, s) {
    if (!s.insights.length) return out.append(h('div', { class: 'card' }, emptyState('No obvious space wasters found. Nice and tidy!')));
    for (const i of s.insights) {
      const sel = new Set(i.paths.map(p => p.path));
      out.append(h('div', { class: 'card' }, h('div', { class: 'card-head' }, h('span', { class: 'app-ico' }, icon(i.kind === 'media' ? 'file' : i.kind === 'dev' ? 'cpu' : i.kind === 'tidy' ? 'folder' : 'sparkles')),
        h('div', { class: 't' }, h('h3', {}, i.title, h('span', { class: 'tag' }, `${i.count}`)), h('div', { class: 'sub' }, i.why)), h('div', { class: 'stat' }, i.size ? fmt(i.size) : ''),
        btn('Recycle Bin', () => act('trash', [...sel]), { small: true, ic: 'trash', kind: 'danger' })),
        h('div', { class: 'list' }, i.paths.slice(0, 12).map(p => h('label', { class: 'item', style: { padding: '6px 2px' } }, h('input', { type: 'checkbox', checked: true, onchange: (e) => (e.target.checked ? sel.add(p.path) : sel.delete(p.path)) }), h('div', { class: 'body mono trunc', title: p.path }, p.path), h('div', { class: 'meta num' }, p.size ? fmt(p.size) : '')))),
        i.paths.length > 12 ? h('div', { class: 'foot-note' }, `+ ${i.paths.length - 12} more (all selected)`) : null));
    }
  }
  async function search(out) {
    const q = h('input', { type: 'search', placeholder: 'Name contains…' });
    const min = h('input', { type: 'number', value: 10, min: 0, style: { width: '80px' } });
    const older = h('select', {}, [[0, 'Any age'], [30, 'Older than 1 month'], [180, 'Older than 6 months'], [365, 'Older than 1 year'], [730, 'Older than 2 years']].map(([v, l]) => h('option', { value: v }, l)));
    const cat = h('select', {}, [['', 'Any type'], ...Object.entries(CAT_NAME)].map(([v, l]) => h('option', { value: v }, l)));
    const box = h('div');
    const run = async () => { box.replaceChildren(loading('Searching…')); const r = await call('diskSearch', { q: q.value, minMB: Number(min.value) || 0, olderDays: Number(older.value), cat: cat.value }); box.replaceChildren(); await fileList(box, r, `${r.length} file(s) found`, 'Searches files of 1 MB and up from the last scan.'); };
    out.append(h('div', { class: 'card' }, h('div', { class: 'row' }, h('label', { class: 'search' }, icon('search'), q), 'at least', min, 'MB', older, cat, btn('Search', run, { kind: 'primary', ic: 'search' }))), h('div', { class: 'gap-16' }), box);
    q.addEventListener('keydown', e => { if (e.key === 'Enter') run(); });
  }
  async function history(out) {
    const r = await call('diskHistory');
    if (r.snapshots.length < 2) return out.append(h('div', { class: 'card' }, emptyState('Scan this location again another day to see which folders grew or shrank.', 'pulse', false)));
    const max = Math.max(...r.snapshots.map(x => x.total));
    out.append(h('div', { class: 'card' }, h('h3', {}, 'Total size over time'), h('div', { class: 'bars', style: { marginTop: '10px' } }, r.snapshots.slice().reverse().map(x => h('div', { class: 'bar-row' }, h('div', {}, new Date(x.at).toLocaleDateString()), h('div', { class: 'meter' }, h('div', { style: { width: (100 * x.total / max) + '%' } })), h('div', { class: 'num', style: { textAlign: 'right' } }, fmt(x.total)))))),
      h('div', { class: 'card' }, h('h3', {}, `Biggest changes since ${new Date(r.since).toLocaleDateString()}`), r.growth.length ? h('div', { class: 'list' }, r.growth.map(g => h('div', { class: 'item' }, icon(g.change > 0 ? 'up' : 'down', 'sm'), h('div', { class: 'body mono trunc' }, g.path), h('div', { class: 'meta num', style: { color: g.change > 0 ? 'var(--warn)' : 'var(--good)' } }, `${g.change > 0 ? '+' : '−'}${fmt(Math.abs(g.change))}`), h('div', { class: 'meta num' }, `${fmt(g.before)} → ${fmt(g.now)}`)))) : emptyState('No folder changed by more than 50 MB.')));
  }
  render();
}

// =====================================================================================
// Uninstaller
// =====================================================================================
PAGES.apps = async (main) => {
  let tab = S.cache.appsTab || 'programs';
  const wrap = h('div');
  const huntBtn = btn('Hunter mode', () => hunter(huntBtn), { ic: 'crosshair', title: 'Click any window to find and remove the app behind it' });
  main.append(pageHead('Uninstaller', 'Remove apps completely — including the leftovers other uninstallers miss — with backups so nothing is lost.', huntBtn, btn('Forced uninstall', forced, { ic: 'trash' })), wrap);
  async function draw() {
    wrap.replaceChildren(tabs([['programs', 'Programs', 'package'], ['leftovers', 'Old leftovers', 'folder'], ['extensions', 'Browser extensions', 'plug'], ['features', 'Windows features', 'layers'], ['tracker', 'Install tracker', 'eye'], ['updates', 'App updates', 'download']], tab, (id) => { tab = S.cache.appsTab = id; draw(); }));
    const out = h('div'); wrap.append(out);
    await ({ programs, leftovers, extensions, features, tracker, updates }[tab])(out);
  }

  // ---------- programs
  async function programs(out) {
    out.append(h('div', { class: 'card' }, skeleton(6)));
    const list = await call('programs');
    out.replaceChildren();
    const st = S.cache.prog || (S.cache.prog = { q: '', filter: 'all', sort: 'name' });
    const sel = new Set();
    const [sb, q] = searchBox('Search apps…', () => { st.q = q.value; drawTable(); }, st.q);
    const filters = [['all', 'All'], ['desktop', 'Desktop'], ['store', 'Store'], ['flagged', 'Flagged'], ['unused', 'Not used in 6 months'], ['large', 'Over 500 MB']];
    const chips = h('div', { class: 'pill-group' });
    const sortSel = h('select', { onchange: (e) => { st.sort = e.target.value; drawTable(); } }, [['name', 'Name'], ['size', 'Size'], ['used', 'Last used'], ['date', 'Install date']].map(([v, l]) => h('option', { value: v, selected: st.sort === v }, `Sort: ${l}`)));
    const batchBtn = btn('Uninstall selected', () => batch([...sel]), { kind: 'primary', ic: 'trash', disabled: true });
    const tbody = h('tbody');
    const total = list.reduce((a, x) => a + (x.sizeKB || 0) * 1024, 0);
    out.append(h('div', { class: 'grid g4' }, kpi('Installed apps', String(list.length), '', 'package'), kpi('Space used', fmt(total), '', 'drive'), kpi('Flagged', String(list.filter(a => a.flag).length), list.some(a => a.flag) ? 'warn' : 'good', 'alert'),
      kpi('Unused 6+ months', String(list.filter(a => a.lastUsed && Date.now() - new Date(a.lastUsed) > 182 * 86400e3).length), '', 'history')), h('div', { class: 'gap-16' }),
      h('div', { class: 'card flush' }, h('div', { class: 'card-head', style: { padding: '14px 16px' } }, sb, chips, h('div', { class: 'spacer' }), sortSel, batchBtn),
        h('div', { class: 'table-wrap', style: { maxHeight: 'none' } }, h('table', {}, h('thead', {}, h('tr', {}, h('th', { style: { width: '30px' } }), h('th', {}, 'App'), h('th', { class: 'num' }, 'Size'), h('th', {}, 'Last used'), h('th', {}, 'Installed'), h('th', {}, 'Version'), h('th'))), tbody))));
    const icons = S.cache.appIcons || (S.cache.appIcons = {});
    function drawTable() {
      chips.replaceChildren(...filters.map(([id, l]) => h('button', { class: 'chip' + (st.filter === id ? ' active' : ''), onclick: () => { st.filter = id; drawTable(); } }, l)));
      const ql = st.q.toLowerCase();
      let rows = list.filter(a => (!ql || `${a.name} ${a.publisher || ''}`.toLowerCase().includes(ql)) && ({
        all: true, desktop: a.kind === 'desktop', store: a.kind === 'store', flagged: !!a.flag, unused: a.lastUsed && Date.now() - new Date(a.lastUsed) > 182 * 86400e3, large: (a.sizeKB || 0) > 500 * 1024 }[st.filter]));
      rows.sort((a, b) => ({ name: () => a.name.localeCompare(b.name), size: () => (b.sizeKB || 0) - (a.sizeKB || 0), used: () => String(a.lastUsed || '').localeCompare(String(b.lastUsed || '')), date: () => String(b.installDate || '').localeCompare(String(a.installDate || '')) }[st.sort])());
      tbody.replaceChildren(...rows.map(a => {
        const ico = h('span', { class: 'app-ico' }, icons[a.id] ? h('img', { src: icons[a.id], alt: '' }) : icon(a.kind === 'store' ? 'grid' : 'package'));
        if (icons[a.id] === undefined) { icons[a.id] = null; call('appIcon', { id: a.id }).then(u => { icons[a.id] = u; if (u) ico.replaceChildren(h('img', { src: u, alt: '' })); }).catch(() => {}); }
        const date = a.installDate && /^\d{8}$/.test(a.installDate) ? `${a.installDate.slice(0, 4)}-${a.installDate.slice(4, 6)}-${a.installDate.slice(6)}` : '';
        return h('tr', { class: sel.has(a.id) ? 'sel' : '' },
          h('td', {}, h('input', { type: 'checkbox', checked: sel.has(a.id), title: a.canSilent ? 'Select for batch uninstall' : 'This app has no silent uninstall — use its Uninstall button', disabled: !a.canSilent, onchange: (e) => { e.target.checked ? sel.add(a.id) : sel.delete(a.id); batchBtn.disabled = !sel.size; batchBtn.lastChild.textContent = sel.size ? `Uninstall ${sel.size} selected` : 'Uninstall selected'; e.target.closest('tr').classList.toggle('sel', e.target.checked); } })),
          h('td', {}, h('div', { class: 'row nowrap' }, ico, h('div', { style: { minWidth: 0 } }, h('div', { class: 'title trunc', style: { maxWidth: '380px' } }, a.name, a.flag ? h('span', { class: `tag ${a.flag.level === 'adware' ? 'bad' : 'warn'}`, title: a.flag.why }, a.flag.level) : null, a.kind === 'store' ? h('span', { class: 'tag' }, 'Store') : null), h('div', { class: 'desc trunc', style: { maxWidth: '380px' } }, a.publisher || '')))),
          h('td', { class: 'num' }, a.sizeKB ? fmt(a.sizeKB * 1024) : '—'), h('td', {}, a.kind === 'store' ? '—' : ago2(a.lastUsed)), h('td', {}, date || '—'), h('td', { class: 'mono' }, a.version || ''),
          h('td', { class: 'num' }, h('button', { class: 'btn small danger', onclick: () => uninstallFlow(a) }, 'Uninstall')));
      }));
      if (!rows.length) tbody.replaceChildren(h('tr', {}, h('td', { colspan: 7 }, emptyState('No apps match.', 'search', false))));
    }
    drawTable();
  }

  // ---------- deep uninstall flow
  async function uninstallFlow(a) {
    const panel = h('div', { class: 'card', style: { borderColor: 'var(--accent)' } });
    wrap.prepend(panel);
    panel.scrollIntoView({ behavior: 'smooth' });
    const step = (title, sub, extra) => panel.replaceChildren(h('div', { class: 'card-head' }, h('span', { class: 'spinner' }), h('div', { class: 't' }, h('h3', {}, title), h('div', { class: 'sub' }, sub)), extra || null));
    if (!(await confirmDlg(`Uninstall ${a.name}?`, a.kind === 'store' ? 'The app will be removed for your account, then OpenSweep checks for leftovers.' : `${a.canSilent ? 'You can let OpenSweep remove it quietly, or use the app\'s own uninstaller.' : "The app's own uninstaller will open."}\nAfterwards OpenSweep scans for leftover files, settings, services and tasks.`, 'Uninstall'))) return panel.remove();
    try {
      if (a.kind === 'store' || a.canSilent) { step(`Removing ${a.name}…`, 'Running the uninstaller quietly.'); await call('uninstall', { id: a.id, silent: true }); }
      else {
        await call('uninstall', { id: a.id, silent: false });
        let done = false;
        await new Promise(res => {
          step(`Finish the ${a.name} uninstaller`, 'Follow its window. OpenSweep continues automatically when it is gone.', btn("It's done — continue", () => { done = true; res(); }, { small: true }));
          const t = setInterval(async () => { if (done) return clearInterval(t); const still = await window.api.call('stillInstalled', { id: a.id }).catch(() => true); if (!still) { clearInterval(t); res(); } }, 3000);
        });
      }
    } catch { return panel.remove(); }
    step('Looking for leftovers…', 'Files, folders, registry keys, startup entries, services, scheduled tasks, shortcuts and firewall rules.');
    const items = await call('leftoverScan', { id: a.id }).catch(() => []);
    leftoverPicker(panel, items, a.name, () => { panel.remove(); if (tab === 'programs') draw(); });
  }

  // Shared picker: leftovers grouped by confidence, safe + likely ticked, careful unticked.
  function leftoverPicker(panel, items, label, done) {
    const sel = new Set(items.filter(i => i.level !== 'careful').map(i => i.id));
    const LEVEL = { safe: ['Definitely leftovers', 'good'], likely: ['Very likely leftovers', 'info'], careful: ['Check before removing', 'warn'] };
    const TYPE = { dir: 'folder', file: 'file', reg: 'layers', run: 'power', service: 'gear', task: 'history', firewall: 'shield' };
    if (!items.length) { panel.replaceChildren(h('div', { class: 'card-head' }, stateDot('on'), h('div', { class: 't' }, h('h3', {}, `${label}: nothing left behind`), h('div', { class: 'sub' }, 'No leftover files, settings or services were found.')), btn('Close', done, { small: true }))); return; }
    const sizeEl = h('span', { class: 'sub' });
    const upd = () => { sizeEl.textContent = `${sel.size} selected · ${fmt(items.filter(i => sel.has(i.id)).reduce((a, i) => a + (i.size || 0), 0))}`; };
    panel.replaceChildren(h('div', { class: 'card-head' }, icon('trash', 'lg'), h('div', { class: 't' }, h('h3', {}, `${items.length} leftover(s) found for ${label}`), h('div', { class: 'sub' }, 'Files go to the Recycle Bin. Registry, services and tasks are backed up first, so you can undo from History.')), sizeEl,
      btn('Skip', done, { small: true }), btn('Remove selected', async (e) => {
        const r = await busy(e.currentTarget, () => call('removeLeftovers', { ids: [...sel], label }), 'Removing…');
        reportList(r.results.map(x => ({ ...x, name: (items.find(i => i.id === x.id) || {}).path })), `Leftovers of ${label}`, x => x.name);
        done();
      }, { small: true, kind: 'primary', ic: 'trash' })),
      ...['safe', 'likely', 'careful'].filter(l => items.some(i => i.level === l)).map(l => h('div', { style: { marginTop: '10px' } }, h('div', { class: 'section-title', style: { margin: '8px 0 4px' } }, h('span', { class: `tag ${LEVEL[l][1]}`, style: { marginLeft: 0 } }, LEVEL[l][0])),
        h('div', { class: 'list' }, items.filter(i => i.level === l).map(i => h('label', { class: 'item', style: { padding: '7px 2px' } }, h('input', { type: 'checkbox', checked: sel.has(i.id), onchange: (e) => { e.target.checked ? sel.add(i.id) : sel.delete(i.id); upd(); } }), icon(TYPE[i.type] || 'file', 'sm'),
          h('div', { class: 'body' }, h('div', { class: 'mono trunc', title: i.path }, i.path), h('div', { class: 'desc' }, i.detail)), h('div', { class: 'meta num' }, i.size ? fmt(i.size) : '')))))));
    upd();
  }

  async function batch(ids) {
    if (!(await confirmDlg(`Uninstall ${ids.length} apps?`, 'They will be removed one by one, quietly. Afterwards OpenSweep scans each for leftovers.', 'Uninstall all'))) return;
    const panel = h('div', { class: 'card' }); wrap.prepend(panel);
    const line = h('div', { class: 'sub' }); const bar = h('div', { class: 'progress' }, h('div', { style: { width: '0%' } }));
    panel.append(h('div', { class: 'card-head' }, h('span', { class: 'spinner' }), h('div', { class: 't' }, h('h3', {}, 'Batch uninstall'), line)), bar);
    onEvent('batch-progress', p => { line.textContent = `${p.index + 1} of ${p.total}: ${p.name}`; bar.firstChild.style.width = `${Math.round(100 * p.index / p.total)}%`; });
    const r = await call('uninstallBatch', { ids }).catch(() => null);
    if (!r) return panel.remove();
    reportList(r, 'Batch uninstall', x => x.name);
    const all = [];
    for (const x of r.filter(x => x.ok)) { line.textContent = `Scanning leftovers of ${x.name}…`; all.push(...(await call('leftoverScan', { id: x.id }).catch(() => []))); }
    leftoverPicker(panel, all, `${r.filter(x => x.ok).length} apps`, () => { panel.remove(); draw(); });
  }

  async function forced() {
    const name = h('input', { type: 'text', placeholder: 'App name, e.g. "Zoom"' });
    const folder = h('input', { type: 'text', placeholder: 'or its folder (optional)' });
    const panel = h('div', { class: 'card', style: { borderColor: 'var(--accent)' } }, h('div', { class: 'card-head' }, icon('trash', 'lg'), h('div', { class: 't' }, h('h3', {}, 'Forced uninstall'), h('div', { class: 'sub' }, "For broken apps with no working uninstaller, or apps that aren't in the list. OpenSweep finds everything that belongs to it."))),
      h('div', { class: 'row' }, name, folder, btn('Browse…', async () => { const f = await call('chooseFolder'); if (f) folder.value = f; }, { small: true, ic: 'folder' }),
        btn('Find everything', async (e) => { const items = await busy(e.currentTarget, () => call('forcedScan', { name: name.value, folder: folder.value || null }), 'Scanning…'); leftoverPicker(panel, items, name.value || folder.value, () => panel.remove()); }, { kind: 'primary', ic: 'search' }), btn('Cancel', () => panel.remove(), { kind: 'ghost' })));
    wrap.prepend(panel); name.focus();
  }

  async function hunter(b) {
    toast('Hunter mode: click on any window in the next 20 seconds');
    const r = await busy(b, () => call('hunt'), 'Click a window…');
    if (!r || r.timeout) return toast('Hunter mode timed out', true);
    const panel = h('div', { class: 'card', style: { borderColor: 'var(--accent)' } }, h('div', { class: 'card-head' }, icon('crosshair', 'lg'), h('div', { class: 't' }, h('h3', {}, `${r.title || r.name} — ${r.name}.exe`), h('div', { class: 'sub mono' }, r.path || ''), r.company ? h('div', { class: 'sub' }, r.company) : null),
      r.app ? btn(`Uninstall ${r.app.name}`, async () => { panel.remove(); const list = await call('programs'); const a = list.find(x => x.id === r.app.id); if (a) uninstallFlow(a); }, { kind: 'danger solid', ic: 'trash', small: true }) : h('span', { class: 'sub' }, 'Not an installed program'),
      btn('End it', async () => { await call('killProcess', { pid: r.pid, name: r.name }); toast(`Ended ${r.name}`); }, { small: true, ic: 'stop' }),
      btn('Startup apps', () => go('startup'), { small: true, ic: 'power' }), btn('Close', () => panel.remove(), { small: true, kind: 'ghost' })));
    wrap.prepend(panel);
  }

  // ---------- other tabs
  async function leftovers(out) {
    const panel = h('div', { class: 'card' }, h('div', { class: 'card-head' }, icon('folder', 'lg'), h('div', { class: 't' }, h('h3', {}, 'Leftovers of apps you removed in the past'), h('div', { class: 'sub' }, 'Folders in Program Files, ProgramData and AppData that no installed app owns and that have not changed for a month. Check each one before removing.')),
      btn('Scan', async (e) => { const items = await busy(e.currentTarget, () => call('orphanScan'), 'Scanning…'); leftoverPicker(panel, items, 'old apps', () => draw()); }, { kind: 'primary', ic: 'search' })));
    out.append(panel);
  }
  async function extensions(out) {
    out.append(loading('Reading browser extensions…'));
    const list = await call('browserExtensions');
    out.replaceChildren();
    const risky = (p) => p.some(x => /<all_urls>|\*:\/\/\*\/\*|webRequest|history|cookies|clipboardRead|debugger|proxy|management/.test(x));
    if (!list.length) return out.append(h('div', { class: 'card' }, emptyState('No browser extensions found.')));
    const browsers = [...new Set(list.map(e => e.browser))];
    for (const b of browsers) {
      out.append(h('div', { class: 'card' }, h('h3', {}, b), h('div', { class: 'list' }, list.filter(e => e.browser === b).map(e => h('div', { class: 'item' }, h('span', { class: 'app-ico' }, icon('plug')),
        h('div', { class: 'body' }, h('div', { class: 'title' }, e.name, e.version ? h('span', { class: 'tag' }, e.version) : null, e.forced ? h('span', { class: 'tag bad', title: 'Installed by a policy — a common adware trick' }, 'forced by policy') : null, risky(e.permissions) ? h('span', { class: 'tag warn', title: e.permissions.join(', ') }, 'can read your browsing') : null),
          h('div', { class: 'desc' }, e.desc || e.extId, ` · ${e.profile}`)),
        btn('Remove', async (ev) => { if (!(await confirmDlg(`Remove ${e.name}?`, `Close ${e.browser} first. The extension folder goes to the Recycle Bin${e.forced ? ' and the policy that forces it is deleted' : ''}.`, 'Remove'))) return; const r = await busy(ev.currentTarget, () => call('removeExtension', { id: e.id })); toast(r.note); draw(); }, { small: true, kind: 'danger' }))))));
    }
  }
  async function features(out) {
    out.append(loading('Reading Windows features (this can take up to a minute)…'));
    const list = await call('optionalFeatures');
    out.replaceChildren();
    const names = S.C.FEATURE_NAMES || {};
    let showAll = false;
    const box = h('div');
    const drawF = () => {
      const rows = list.filter(f => showAll || names[f.name]).sort((a, b) => (names[a.name] || a.name).localeCompare(names[b.name] || b.name));
      box.replaceChildren(h('div', { class: 'list' }, rows.map(f => {
        const on = /Enabled/.test(f.state);
        return h('div', { class: 'item' }, h('div', { class: 'body' }, h('div', { class: 'title' }, names[f.name] || f.name, adminTag(true)), h('div', { class: 'desc mono' }, f.name)), h('span', { class: 'meta' }, f.state),
          h('div', { class: 'controls' }, toggle(on, async (v, input) => { input.disabled = true; try { const r = await call('setFeature', { name: f.name, enable: v }); f.state = r.state; toast(`${names[f.name] || f.name}: ${r.state}${r.restart ? ' — restart to finish' : ''}`); } catch { input.checked = !v; } drawF(); }), stateDot(on ? 'on' : 'off')));
      })));
    };
    out.append(h('div', { class: 'card' }, h('div', { class: 'card-head' }, h('div', { class: 't' }, h('h3', {}, 'Windows optional features'), h('div', { class: 'sub' }, 'Turn built-in Windows parts on or off. Most changes need a restart.')),
      h('label', { class: 'row' }, toggle(false, (v) => { showAll = v; drawF(); }), 'Show all features')), box));
    drawF();
  }
  async function tracker(out) {
    const list = await call('tracks');
    const active = list.find(t => !t.finished);
    const label = h('input', { type: 'text', placeholder: 'What are you installing? e.g. "Zoom"' });
    out.append(h('div', { class: 'card' }, h('div', { class: 'card-head' }, icon('eye', 'lg'), h('div', { class: 't' }, h('h3', {}, 'Install tracker'), h('div', { class: 'sub' }, 'Take a snapshot, install an app, then finish. OpenSweep records exactly what the installer added — folders, registry keys, services, tasks, startup entries, shortcuts and firewall rules — so you can remove all of it later.'))),
      active ? h('div', { class: 'banner info' }, icon('eye'), `Tracking "${active.label}" since ${new Date(active.started).toLocaleTimeString()}. Install the app now, then press Finish.`, btn('Finish', async (e) => { const t = await busy(e.currentTarget, () => call('trackFinish', { id: active.id }), 'Comparing…'); toast(`Recorded ${Object.values(t.diff).reduce((a, x) => a + x.length, 0)} change(s)`); draw(); }, { kind: 'primary', small: true }))
        : h('div', { class: 'row' }, label, btn('Start tracking', async (e) => { await busy(e.currentTarget, () => call('trackStart', { label: label.value || 'Install' }), 'Taking snapshot…'); toast('Snapshot taken — install your app now'); draw(); }, { kind: 'primary', ic: 'play' }))));
    const done = list.filter(t => t.finished);
    if (done.length) out.append(h('div', { class: 'section-title' }, 'Install logs'), h('div', { class: 'card' }, h('div', { class: 'list' }, done.map(t => {
      const n = Object.values(t.diff || {}).reduce((a, x) => a + x.length, 0);
      return h('div', { class: 'item' }, h('div', { class: 'body' }, h('div', { class: 'title' }, t.label), h('div', { class: 'desc' }, `${new Date(t.started).toLocaleString()} · ${n} change(s): ${Object.entries(t.diff || {}).filter(([, v]) => v.length).map(([k, v]) => `${v.length} ${k}`).join(', ') || 'nothing'}`)),
        btn('Remove everything it added', async () => { const items = await call('trackItems', { id: t.id }); const panel = h('div', { class: 'card', style: { borderColor: 'var(--accent)' } }); wrap.prepend(panel); leftoverPicker(panel, items, t.label, () => panel.remove()); }, { small: true, kind: 'danger', ic: 'trash', disabled: !n }));
    }))));
  }
  async function updates(out) {
    out.append(loading('Checking for app updates with winget…'));
    const list = await call('wingetUpgrades').catch(() => []);
    out.replaceChildren(h('div', { class: 'card' }, h('div', { class: 'card-head' }, icon('download', 'lg'), h('div', { class: 't' }, h('h3', {}, `${list.length} app update(s) available`), h('div', { class: 'sub' }, "Uses Windows' own package manager (winget). Updates install quietly.")),
      list.length ? btn('Update all', async (e) => { for (const u of list) { await busy(e.currentTarget, () => call('wingetUpgrade', { id: u.id }).catch(() => null), `Updating ${u.name}…`); } toast('Updates finished'); draw(); }, { kind: 'primary', ic: 'download' }) : null),
      list.length ? h('div', { class: 'list' }, list.map(u => h('div', { class: 'item' }, h('span', { class: 'app-ico' }, icon('package')), h('div', { class: 'body' }, h('div', { class: 'title' }, u.name), h('div', { class: 'desc mono' }, u.id)), h('span', { class: 'meta num' }, `${u.version} → `, h('b', {}, u.available)),
        btn('Update', async (e) => { const r = await busy(e.currentTarget, () => call('wingetUpgrade', { id: u.id }), 'Updating…'); toast(r.ok ? `${u.name} updated` : `${u.name}: update failed`, !r.ok); }, { small: true })))) : emptyState('All your apps are up to date.')));
  }
  draw();
};

// =====================================================================================
// Reliability (stability history + drivers & hardware)
// =====================================================================================
PAGES.reliability = async (main) => {
  let tab = S.cache.relTab || 'history';
  const wrap = h('div');
  main.append(pageHead('Reliability', "Your PC's stability score, crashes, failed updates and driver problems — explained in plain English.", btn('Check now', () => draw(true), { ic: 'refresh' })), wrap);
  async function draw(refresh) {
    wrap.replaceChildren(tabs([['history', 'Stability history', 'pulse'], ['drivers', 'Drivers & hardware', 'cpu']], tab, (id) => { tab = S.cache.relTab = id; draw(); }));
    const out = h('div'); wrap.append(out);
    if (tab === 'history') await history(out, refresh); else await drivers(out, refresh);
  }
  async function history(out, refresh) {
    out.append(h('div', { class: 'card' }, loading('Reading the reliability history (can take 10–20 seconds)…')));
    const r = (!refresh && S.cache.rel) || (S.cache.rel = await call('reliability', { days: 30 }));
    const sum = (k) => r.days.reduce((a, d) => a + d[k], 0);
    const score = r.current;
    out.replaceChildren(h('div', { class: 'grid g4' },
      kpi('Stability score', score === null ? '—' : h('span', {}, score.toFixed(1), h('small', {}, ' / 10')), score === null ? '' : score >= 8 ? 'good' : score >= 5 ? 'warn' : 'bad', 'pulse'),
      kpi('App crashes (30 days)', String(sum('app')), sum('app') ? 'warn' : 'good', 'alert'), kpi('Windows failures', String(sum('windows')), sum('windows') ? 'bad' : 'good', 'shield'), kpi('Failed updates', String(sum('warning')), sum('warning') ? 'warn' : 'good', 'refresh')));
    // chart: line + event columns
    const W = 1000, H = 120;
    const pts = r.days.map((d, i) => d.index === null ? null : [i * (W / (r.days.length - 1)), H - (d.index / 10) * (H - 10) - 5]);
    const line = pts.filter(Boolean).map(p => p.join(',')).join(' ');
    const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
    svg.setAttribute('viewBox', `0 0 ${W} ${H}`); svg.setAttribute('preserveAspectRatio', 'none'); svg.setAttribute('class', 'chart'); svg.style.height = '120px';
    svg.innerHTML = `${[2.5, 5, 7.5].map(v => `<line class="grid-line" x1="0" x2="${W}" y1="${H - (v / 10) * (H - 10) - 5}" y2="${H - (v / 10) * (H - 10) - 5}" vector-effect="non-scaling-stroke"/>`).join('')}
      ${line ? `<polyline points="0,${H} ${line} ${W},${H}" fill="var(--accent)" fill-opacity=".10"/><polyline points="${line}" fill="none" stroke="var(--accent)" stroke-width="2.5" vector-effect="non-scaling-stroke" stroke-linejoin="round"/>` : ''}`;
    const detail = h('div');
    let selDay = r.days[r.days.length - 1].day;
    const cols = h('div', { class: 'rel' });
    const drawCols = () => cols.replaceChildren(...r.days.map(d => h('div', { class: 'col' + (d.day === selDay ? ' sel' : ''), title: `${d.day} · score ${d.index ?? '—'}`, onclick: () => { selDay = d.day; drawCols(); drawDetail(); } },
      ...['windows', 'app', 'misc', 'warning', 'info'].filter(k => d[k]).map(k => h('span', { class: 'ev ' + k, title: `${d[k]} ${k}` })), h('span', { class: 'd' }, d.day.slice(8)))));
    const KIND = { app: ['App crash', 'bad', 'alert'], windows: ['Windows failure', 'bad', 'shield'], misc: ['Other failure', 'warn', 'alert'], warning: ['Warning', 'warn', 'alert'], info: ['Information', 'info', 'info'] };
    const drawDetail = () => {
      const recs = r.records.filter(x => x.day === selDay);
      detail.replaceChildren(h('h3', {}, new Date(selDay + 'T12:00:00').toLocaleDateString(undefined, { weekday: 'long', day: 'numeric', month: 'long' })),
        recs.length ? h('div', { class: 'list' }, recs.map(x => h('div', { class: 'item' }, h('span', { class: `tag ${KIND[x.kind][1]}`, style: { marginLeft: 0, minWidth: '110px', justifyContent: 'center' } }, KIND[x.kind][0]),
          h('div', { class: 'body' }, h('div', { class: 'title' }, x.title), h('div', { class: 'desc' }, x.msg.split(/\r?\n/)[0].slice(0, 220))), h('span', { class: 'meta' }, x.t.slice(11, 16))))) : emptyState('Nothing happened on this day.'));
    };
    drawCols(); drawDetail();
    out.append(h('div', { class: 'gap-16' }), h('div', { class: 'card' }, h('div', { class: 'card-head' }, h('div', { class: 't' }, h('h3', {}, 'Stability over the last 30 days'), h('div', { class: 'sub' }, 'Click a day to see what happened.')),
      h('div', { class: 'legend' }, [['windows', 'Windows failure'], ['app', 'App crash'], ['misc', 'Other failure'], ['warning', 'Warning'], ['info', 'Info']].map(([k, l]) => h('span', {}, h('span', { class: 'sw ev ' + k, style: { borderRadius: '50%' } }), l)))),
      svg, cols), h('div', { class: 'grid g2', style: { marginTop: '12px', alignItems: 'start' } }, h('div', { class: 'card' }, detail),
      h('div', { class: 'card' }, h('h3', {}, 'Apps that crash the most'), r.crashers.length ? h('div', { class: 'list' }, r.crashers.map(c => h('div', { class: 'item' }, h('span', { class: 'app-ico' }, icon('alert')),
        h('div', { class: 'body' }, h('div', { class: 'title' }, c.name), h('div', { class: 'desc' }, `${c.count} crash${c.count > 1 ? 'es' : ''}${c.hangs ? ` (${c.hangs} froze)` : ''} · last ${ago2(c.last).toLowerCase()}`)),
        btn('Fix', (e) => menu(e, [
          { label: 'Check for an update', icon: 'download', run: () => { S.cache.appsTab = 'updates'; go('apps'); } },
          { label: 'Reinstall or uninstall it', icon: 'package', run: () => { S.cache.appsTab = 'programs'; S.cache.prog = { q: c.name.replace(/\.exe$/i, ''), filter: 'all', sort: 'name' }; go('apps'); } },
          { label: 'Repair Windows files (SFC)', icon: 'repair', run: () => go('repair') },
        ]), { small: true })))) : emptyState('No app crashes in 30 days.'))));
  }
  async function drivers(out, refresh) {
    out.append(h('div', { class: 'card' }, loading('Checking drivers and the event log…')));
    const r = await call('stability', { refresh: !!refresh });
    const toolIds = new Set((S.C.TOOLBOX || []).map(t => t.id));
    out.replaceChildren(h('div', { class: 'grid g3' }, kpi('Devices with problems', String(r.devices.length), r.devices.length ? 'bad' : 'good', 'cpu'), kpi(`Hardware & crash events (${r.days} days)`, String(r.events.length), r.events.length ? 'warn' : 'good', 'alert'), kpi('Third-party drivers', String(r.thirdPartyDrivers), '', 'layers')),
      h('div', { class: 'gap-16' }),
      h('div', { class: 'card' }, h('div', { class: 'card-head' }, h('h3', { class: 't' }, 'Devices with driver problems'), btn('Scan for hardware changes', async (e) => { await busy(e.currentTarget, () => call('scanDevices')); toast('Scanned for hardware changes'); draw(true); }, { small: true, ic: 'refresh' })),
        r.devices.length ? h('div', { class: 'list' }, r.devices.map(d => h('div', { class: 'item' }, stateDot('fail'), h('div', { class: 'body' }, h('div', { class: 'title' }, d.name), h('div', { class: 'desc' }, `${d.problem}${d.cls ? ' · ' + d.cls : ''} · code ${d.code}`))))) : emptyState('All devices are working.'),
        h('div', { class: 'row', style: { marginTop: '10px' } }, btn('Optional driver updates', () => call('openUri', { uri: 'ms-settings:windowsupdate-optionalupdates' }), { small: true, ic: 'download' }), toolIds.has('devmgmt') ? btn('Device Manager', () => call('launchTool', { id: 'devmgmt' }), { small: true, ic: 'external' }) : null)),
      h('div', { class: 'card' }, h('h3', {}, 'Crashes & hardware errors'), r.events.length ? h('div', { class: 'list' }, r.events.map(e => h('div', { class: 'item' }, h('span', { class: 'tag ' + (e.kind === 'Blue screen' || e.kind === 'Hardware error' ? 'bad' : 'warn'), style: { marginLeft: 0, minWidth: '140px', justifyContent: 'center' } }, e.kind),
        h('div', { class: 'body' }, h('div', { class: 'desc' }, e.message), h('div', { class: 'desc mono' }, `${new Date(e.when).toLocaleString()} · ${e.source} ${e.eventId}`))))) : emptyState('No crashes or hardware errors.')),
      r.oldDrivers.length || r.unsignedDrivers.length ? h('div', { class: 'card' }, h('h3', {}, 'Drivers worth updating'), h('div', { class: 'list' },
        r.unsignedDrivers.map(d => h('div', { class: 'item' }, h('span', { class: 'tag bad', style: { marginLeft: 0 } }, 'unsigned'), h('div', { class: 'body' }, h('div', { class: 'title' }, d.name), h('div', { class: 'desc' }, `${d.provider} · ${d.version}`)))),
        r.oldDrivers.map(d => h('div', { class: 'item' }, h('span', { class: 'tag warn', style: { marginLeft: 0 } }, d.date.slice(0, 4)), h('div', { class: 'body' }, h('div', { class: 'title' }, d.name), h('div', { class: 'desc' }, `${d.provider} · ${d.version} · over 5 years old — check the maker's website`)))))) : null,
      h('div', { class: 'foot-note' }, `Last checked ${new Date(r.checkedAt).toLocaleString()}. OpenSweep checks this in the background about once an hour.`));
  }
  draw();
};

// =====================================================================================
// Processes (live)
// =====================================================================================
PAGES.processes = async (main) => {
  const C = S.C;
  const st = S.cache.procView || (S.cache.procView = { q: '', sort: 'cpu', grouped: true });
  const open = new Set();
  const hist = { cpu: [], mem: [] };
  const cpuK = h('div', { class: 'value' }), memK = h('div', { class: 'value' }), cntK = h('div', { class: 'value' }), flagK = h('div', { class: 'value' });
  const cpuSp = h('div'), memSp = h('div');
  const [sb, q] = searchBox('Search processes…', () => { st.q = q.value; drawTable(); }, st.q);
  const sortChips = h('div', { class: 'pill-group' });
  const tbody = h('tbody');
  let procs = [];
  const status = h('span', { class: 'sub' }, 'Starting live view…');
  main.append(pageHead('Processes', 'Live view of everything running, grouped by app, updated every 2 seconds. Critical Windows processes are protected.', status),
    h('div', { class: 'grid g4' },
      h('div', { class: 'card tight kpi' }, h('div', { class: 'label' }, icon('cpu', 'sm'), 'CPU'), cpuK, cpuSp), h('div', { class: 'card tight kpi' }, h('div', { class: 'label' }, icon('memory', 'sm'), 'Memory'), memK, memSp),
      h('div', { class: 'card tight kpi' }, h('div', { class: 'label' }, icon('activity', 'sm'), 'Processes'), cntK), h('div', { class: 'card tight kpi' }, h('div', { class: 'label' }, icon('alert', 'sm'), 'Worth a look'), flagK, h('div', { class: 'desc' }, 'Unsigned, no publisher, or running from Temp/Downloads'))),
    h('div', { class: 'gap-16' }),
    h('div', { class: 'card flush' }, h('div', { class: 'card-head', style: { padding: '14px 16px' } }, sb, sortChips, h('div', { class: 'spacer' }), h('label', { class: 'row' }, toggle(st.grouped, (v) => { st.grouped = v; drawTable(); }), 'Group by app')),
      h('div', { class: 'table-wrap', style: { maxHeight: 'none' } }, h('table', {}, h('thead', {}, h('tr', {}, h('th', {}, 'Name'), h('th', { class: 'num' }, 'CPU'), h('th', { class: 'num' }, 'Memory'), h('th', { class: 'num' }, 'Disk'), h('th', {}, 'Publisher'), h('th'))), tbody))));

  const suspicious = (p) => {
    const why = [];
    if (p.sig && /NotSigned|HashMismatch|NotTrusted/.test(p.sig)) why.push('not signed');
    const lp = String(p.path || '').toLowerCase();
    if (/\\(temp|tmp)\\|\\downloads\\|\\appdata\\local\\temp\\/.test(lp)) why.push('runs from Temp/Downloads');
    if (p.path && !p.company) why.push('no publisher');
    return why;
  };
  function procMenu(e, g) {
    const p = g.items[0], prot = C.PROTECTED_PROCESSES.includes(g.name.toLowerCase());
    menu(e, [
      { label: 'Open file location', icon: 'folder', run: () => call('processLocation', { pid: p.pid }) },
      { label: 'Search online', icon: 'globe', run: () => call('processSearch', { pid: p.pid }) },
      '-',
      ...(prot ? [] : [['Low', 'Idle'], ['Below normal', 'BelowNormal'], ['Normal', 'Normal'], ['Above normal', 'AboveNormal'], ['High', 'High']].map(([l, v]) => ({ label: `Priority: ${l}${p.pri === v ? ' ✓' : ''}`, icon: 'sliders', run: async () => { for (const x of g.items) await call('setPriority', { pid: x.pid, priority: v }).catch(() => {}); toast(`${g.name}: priority ${l}`); } }))),
      ...(prot ? [] : ['-', { label: g.items.length > 1 ? `End all ${g.items.length} processes` : 'End task', icon: 'stop', danger: true, run: async () => {
        if (!(await confirmDlg(`End ${g.name}?`, `${g.items.length} process(es) will be closed. Unsaved work in that app will be lost.`, 'End task'))) return;
        for (const x of g.items) await call('killProcess', { pid: x.pid, name: g.name }).catch(() => {}); toast(`Ended ${g.name}`); } }]),
    ]);
  }
  function drawTable() {
    sortChips.replaceChildren(...[['cpu', 'CPU'], ['mem', 'Memory'], ['io', 'Disk'], ['name', 'Name']].map(([k, l]) => h('button', { class: 'chip' + (st.sort === k ? ' active' : ''), onclick: () => { st.sort = k; drawTable(); } }, l)));
    const ql = st.q.toLowerCase();
    const groups = {};
    for (const p of procs) {
      if (ql && !`${p.name} ${p.desc || ''} ${p.company || ''} ${p.title || ''}`.toLowerCase().includes(ql)) continue;
      const key = st.grouped ? p.name : `${p.name}#${p.pid}`;
      const g = groups[key] || (groups[key] = { key, name: p.name, desc: p.desc, company: p.company, items: [], cpu: 0, mem: 0, io: 0, flags: new Set() });
      g.items.push(p); g.cpu += p.cpu; g.mem += p.mem; g.io += p.io || 0; suspicious(p).forEach(f => g.flags.add(f));
      if (!g.desc && p.desc) g.desc = p.desc; if (!g.company && p.company) g.company = p.company;
    }
    const rows = Object.values(groups).sort((a, b) => ({ cpu: b.cpu - a.cpu, mem: b.mem - a.mem, io: b.io - a.io, name: a.name.localeCompare(b.name) }[st.sort]) || b.mem - a.mem).slice(0, 250);
    const heat = (v, hi) => (v >= hi ? { color: 'var(--bad)', fontWeight: '600' } : v >= hi / 3 ? { color: 'var(--warn)' } : {});
    const out = [];
    for (const g of rows) {
      const prot = C.PROTECTED_PROCESSES.includes(g.name.toLowerCase());
      const isOpen = open.has(g.key);
      out.push(h('tr', { oncontextmenu: (e) => { e.preventDefault(); procMenu(e, g); } },
        h('td', {}, h('span', { class: 'tw row nowrap', style: { gap: '6px' } }, st.grouped && g.items.length > 1 ? h('button', { class: 'btn ghost small icon-only', onclick: () => { isOpen ? open.delete(g.key) : open.add(g.key); drawTable(); } }, icon(isOpen ? 'down' : 'chevron')) : h('span', { style: { width: '26px', display: 'inline-block' } }),
          h('div', { style: { minWidth: 0 } }, h('div', { class: 'title trunc', style: { maxWidth: '360px' } }, g.desc && g.desc !== g.name ? g.desc : g.name, g.items.length > 1 ? h('span', { class: 'tag' }, `×${g.items.length}`) : null, prot ? h('span', { class: 'tag', title: 'Critical Windows process — protected' }, icon('lock'), 'system') : null, g.flags.size ? h('span', { class: 'tag warn', title: [...g.flags].join(', ') }, icon('alert'), [...g.flags][0]) : null),
            h('div', { class: 'desc mono' }, `${g.name}.exe`)))),
        h('td', { class: 'num', style: heat(g.cpu, 25) }, `${g.cpu.toFixed(1)} %`), h('td', { class: 'num', style: heat(g.mem, 1500) }, `${Math.round(g.mem).toLocaleString()} MB`), h('td', { class: 'num' }, g.io ? fmtRate(g.io) : '—'),
        h('td', {}, h('div', { class: 'trunc', style: { maxWidth: '200px' } }, g.company || '')),
        h('td', { class: 'num' }, h('button', { class: 'btn ghost small icon-only', title: 'Actions', onclick: (e) => { e.stopPropagation(); procMenu(e, g); } }, icon('dots')))));
      if (isOpen) for (const p of g.items.sort((a, b) => b.mem - a.mem)) out.push(h('tr', { class: 'sub' }, h('td', {}, h('span', { style: { paddingLeft: '40px' }, class: 'mono' }, `PID ${p.pid}`, p.title ? ` · ${p.title}` : '')), h('td', { class: 'num' }, `${p.cpu.toFixed(1)} %`), h('td', { class: 'num' }, `${Math.round(p.mem)} MB`), h('td', { class: 'num' }, p.io ? fmtRate(p.io) : '—'), h('td', { class: 'mono trunc', style: { maxWidth: '200px' }, title: p.path || '' }, p.path || ''), h('td')));
    }
    tbody.replaceChildren(...(out.length ? out : [h('tr', {}, h('td', { colspan: 6 }, procs.length ? emptyState('No process matches.', 'search', false) : loading('Waiting for the first update…')))]));
  }
  drawTable();
  const tickStats = async () => {
    const s = await window.api.call('stats').catch(() => null); if (!s) return;
    hist.cpu.push(s.cpu); hist.mem.push(s.memPct); if (hist.cpu.length > 60) { hist.cpu.shift(); hist.mem.shift(); }
    cpuK.replaceChildren(`${s.cpu}`, h('small', {}, ' %')); memK.replaceChildren(`${s.memPct}`, h('small', {}, ` %  ·  ${fmt(s.memUsed)} of ${fmt(s.memTotal)}`));
    cpuSp.replaceChildren(sparkline(hist.cpu)); memSp.replaceChildren(sparkline(hist.mem, { color: 'var(--c2)' }));
  };
  tickStats(); S.timers.push(setInterval(tickStats, 2000));
  onEvent('procs', (d) => {
    procs = d.procs || [];
    status.replaceChildren(h('span', { class: 'state on', style: { width: '10px', height: '10px', border: 0, background: 'var(--good)', display: 'inline-block', marginRight: '6px' } }), `Live · ${new Date().toLocaleTimeString()}`);
    cntK.textContent = String(procs.length);
    const flagged = new Set(procs.filter(p => suspicious(p).length).map(p => p.name));
    flagK.textContent = String(flagged.size); flagK.className = 'value ' + (flagged.size ? 'warn' : 'good');
    if (!document.querySelector('.menu') && document.activeElement !== q) drawTable();
  });
  await call('procStart');
  S.timers.push(setInterval(() => call('procKeepAlive').catch(() => {}), 60e3));
  S.onLeave = () => call('procStop').catch(() => {});
};
