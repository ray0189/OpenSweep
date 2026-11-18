'use strict';
const $ = (id) => document.getElementById(id);
async function tick() {
  const s = await window.api.call('stats');
  $('cpu').textContent = s.cpu + '%';
  $('ram').textContent = s.memPct + '%';
  $('cpub').style.width = s.cpu + '%';
  $('ramb').style.width = s.memPct + '%';
  $('ramb').style.background = s.memPct > 85 ? '#f87171' : '#2dd4bf';
}
$('boost').onclick = async () => { $('boost').textContent = '…'; const r = await window.api.call('boost'); $('boost').textContent = '⚡'; $('boost').title = `Freed ${r.memFreedMB} MB`; tick(); };
$('open').onclick = () => window.api.call('openMain');
$('close').onclick = () => window.api.call('closeWidget');
tick(); setInterval(tick, 2000);
