'use strict';
// Demo backend: realistic sample data so the UI can be developed and previewed on
// macOS/Linux (or with --demo on Windows). Nothing here touches the real system.
const C = require('./catalog');

const wait = (ms) => new Promise(r => setTimeout(r, ms));
const GB = 1024 ** 3, MB = 1024 ** 2;
const rnd = (a, b) => Math.round(a + Math.random() * (b - a));

const state = {
  tweaks: Object.fromEntries(C.TWEAKS.map(t => [t.id, ['explorer.extensions', 'gaming.gamemode'].includes(t.id)])),
  installed: new Set(C.BLOAT_APPS.filter((_, i) => i % 4 !== 3).map(a => a.pkg)),
  services: Object.fromEntries(C.SERVICES.map(s => [s.name, s.orig])),
  tasks: Object.fromEntries(C.SCHEDULED_TASKS.map(t => [t.path + t.name, 'Ready'])),
  junk: Object.fromEntries(C.CLEANUP.map(c => [c.id, c.special === 'recycle' ? 1.3 * GB : rnd(0, 900) * MB])),
  startup: [
    { id: 'HKCU|OneDrive', source: 'HKCU', name: 'OneDrive', command: '"C:\\Program Files\\Microsoft OneDrive\\OneDrive.exe" /background', enabled: true, admin: false, impact: 'High' },
    { id: 'HKCU|Discord', source: 'HKCU', name: 'Discord', command: 'C:\\Users\\you\\AppData\\Local\\Discord\\Update.exe --processStart Discord.exe', enabled: true, admin: false, impact: 'High' },
    { id: 'HKCU|Spotify', source: 'HKCU', name: 'Spotify', command: 'C:\\Users\\you\\AppData\\Roaming\\Spotify\\Spotify.exe /minimized', enabled: true, admin: false, impact: 'Medium' },
    { id: 'HKCU|MicrosoftEdgeAutoLaunch', source: 'HKCU', name: 'MicrosoftEdgeAutoLaunch', command: '"C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe" --no-startup-window', enabled: true, admin: false, impact: 'Medium' },
    { id: 'HKLM|SecurityHealth', source: 'HKLM', name: 'SecurityHealth', command: '%windir%\\system32\\SecurityHealthSystray.exe', enabled: true, admin: true, impact: 'Low' },
    { id: 'HKLM|RtkAudUService', source: 'HKLM', name: 'RtkAudUService', command: '"C:\\Windows\\System32\\RtkAudUService64.exe" -background', enabled: true, admin: true, impact: 'Low' },
    { id: 'HKLM32|Steam', source: 'HKLM32', name: 'Steam', command: '"C:\\Program Files (x86)\\Steam\\steam.exe" -silent', enabled: false, admin: true, impact: 'High' },
    { id: 'UserFolder|Notion.lnk', source: 'UserFolder', name: 'Notion', command: 'C:\\Users\\you\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu\\Programs\\Startup\\Notion.lnk', enabled: true, admin: false, impact: 'Medium' },
    { id: 'Task|\\AdobeGCInvoker-1.0', source: 'Task', name: 'AdobeGCInvoker-1.0', taskPath: '\\', command: 'C:\\Program Files (x86)\\Common Files\\Adobe\\AdobeGCClient\\AGCInvokerUtility.exe', enabled: true, admin: true, impact: 'Low' },
  ],
  procs: null,
};

const PROC_NAMES = [['chrome', 'Google LLC'], ['chrome', 'Google LLC'], ['chrome', 'Google LLC'], ['msedge', 'Microsoft Corporation'], ['Discord', 'Discord Inc.'], ['Spotify', 'Spotify AB'], ['explorer', 'Microsoft Corporation'], ['svchost', 'Microsoft Corporation'], ['svchost', 'Microsoft Corporation'], ['MsMpEng', 'Microsoft Corporation'], ['OneDrive', 'Microsoft Corporation'], ['Code', 'Microsoft Corporation'], ['steam', 'Valve'], ['SearchHost', 'Microsoft Corporation'], ['Widgets', 'Microsoft Corporation'], ['PhoneExperienceHost', 'Microsoft Corporation'], ['dwm', 'Microsoft Corporation'], ['Teams', 'Microsoft Corporation'], ['RuntimeBroker', 'Microsoft Corporation'], ['Copilot', 'Microsoft Corporation'], ['Notion', 'Notion Labs'], ['audiodg', 'Microsoft Corporation']];

module.exports = {
  demo: true,

  async systemInfo() {
    await wait(300);
    return {
      os: 'Microsoft Windows 11 Pro', displayVersion: '24H2', version: '10.0.26100', build: '26100', arch: '64-bit',
      installDate: '2025-03-14T10:22:00', lastBoot: new Date(Date.now() - 9 * 86400e3).toISOString().slice(0, 19),
      manufacturer: 'Demo Computers', model: 'Gaming Desktop', computerName: 'DEMO-PC', user: 'you',
      cpu: 'AMD Ryzen 7 7800X3D 8-Core Processor', cores: 8, threads: 16, cpuMHz: 4200, ramTotal: 32 * GB,
      gpu: ['NVIDIA GeForce RTX 4070 SUPER'], isAdmin: false, bios: 'American Megatrends 1.40', board: 'ASUS TUF GAMING B650-PLUS',
      disks: [{ name: 'C:', label: 'Windows', total: 953 * GB, free: 74 * GB, fs: 'NTFS' }, { name: 'D:', label: 'Games', total: 1863 * GB, free: 612 * GB, fs: 'NTFS' }],
      physicalDisks: [{ name: 'Samsung SSD 990 PRO 1TB', media: 'SSD', bus: 'NVMe', health: 'Healthy', size: 1000 * 1e9 }, { name: 'WD Blue SN580 2TB', media: 'SSD', bus: 'NVMe', health: 'Healthy', size: 2000 * 1e9 }],
      battery: null,
    };
  },

  async processes() {
    await wait(250);
    if (!state.procs) state.procs = PROC_NAMES.map(([name, company], i) => ({ pid: 1000 + i * 37, name, company, path: `C:\\Program Files\\${name}\\${name}.exe`, title: '' }));
    return state.procs.map(p => ({ ...p, memMB: rnd(20, p.name === 'chrome' ? 900 : 400), cpu: Math.round(Math.random() * (p.name === 'chrome' ? 12 : 3) * 10) / 10 }));
  },
  async killProcess({ pid }) { await wait(200); state.procs = state.procs.filter(p => p.pid !== pid); return { ok: true }; },

  async boost() { await wait(1500); return { memFreedMB: rnd(600, 2400), processesTrimmed: rnd(80, 160), tempFreed: rnd(50, 400) * MB }; },

  async cleanupScan({ ids } = {}) {
    await wait(1400);
    return C.CLEANUP.filter(c => !ids || ids.includes(c.id)).map(c => ({ id: c.id, size: state.junk[c.id], files: Math.round(state.junk[c.id] / (180 * 1024)) }));
  },
  async cleanupRun({ ids }) {
    await wait(1800);
    return ids.map(id => { const freed = state.junk[id] * 0.97; state.junk[id] = 0; return { id, freed, skipped: rnd(0, 6) }; });
  },

  async largeFiles() {
    await wait(1200);
    const f = [['Videos\\OBS\\2025-10-02 21-14-55.mkv', 7.8 * GB], ['Downloads\\Win11_24H2_English_x64.iso', 5.4 * GB], ['Downloads\\ubuntu-24.04-desktop-amd64.iso', 5.7 * GB], ['Documents\\Backups\\phone-backup-2024.zip', 3.1 * GB], ['AppData\\Local\\Docker\\wsl\\data\\ext4.vhdx', 22.4 * GB], ['Videos\\Captures\\Valorant 2025-09-12.mp4', 1.9 * GB], ['Downloads\\DaVinci_Resolve_19_Windows.zip', 3.4 * GB], ['Downloads\\setup_cyberpunk_patch.exe', 1.2 * GB], ['Pictures\\RAW\\DSC_0412.NEF', 0.11 * GB]];
    return f.map(([p, s], i) => ({ path: `C:\\Users\\you\\${p}`, size: s, modified: `2025-0${(i % 9) + 1}-1${i}T12:00:00`, accessed: '2025-10-01T12:00:00', ext: p.slice(p.lastIndexOf('.')).toLowerCase() })).sort((a, b) => b.size - a.size);
  },
  async duplicates() {
    await wait(1600);
    return [
      { hash: 'A1', size: 1.2 * GB, files: [{ path: 'C:\\Users\\you\\Downloads\\setup_cyberpunk_patch.exe', modified: '2025-05-01T10:00:00' }, { path: 'C:\\Users\\you\\Downloads\\setup_cyberpunk_patch (1).exe', modified: '2025-05-02T10:00:00' }] },
      { hash: 'B2', size: 240 * MB, files: [{ path: 'C:\\Users\\you\\Videos\\clip.mp4', modified: '2025-01-01T10:00:00' }, { path: 'C:\\Users\\you\\Desktop\\clip.mp4', modified: '2025-01-03T10:00:00' }, { path: 'C:\\Users\\you\\OneDrive\\clip - Copy.mp4', modified: '2025-02-01T10:00:00' }] },
      { hash: 'C3', size: 8.4 * MB, files: [{ path: 'C:\\Users\\you\\Pictures\\IMG_2231.JPG', modified: '2024-08-01T10:00:00' }, { path: 'C:\\Users\\you\\Pictures\\Phone\\IMG_2231.JPG', modified: '2024-08-01T10:00:00' }] },
    ];
  },
  async folderSizes() {
    await wait(900);
    return [['AppData', 61], ['Videos', 48], ['Downloads', 37], ['Documents', 12], ['Pictures', 9], ['OneDrive', 6], ['Music', 3], ['Desktop', 1.2]].map(([n, g]) => ({ name: n, path: `C:\\Users\\you\\${n}`, size: g * GB }));
  },

  async installedApps() {
    await wait(700);
    const d = [['Google Chrome', 'Google LLC', '131.0', 450], ['Discord', 'Discord Inc.', '1.0.9170', 380], ['Steam', 'Valve Corporation', '2.10', 900], ['Spotify', 'Spotify AB', '1.2.50', 320], ['Visual Studio Code', 'Microsoft Corporation', '1.95.3', 380], ['7-Zip 24.08 (x64)', 'Igor Pavlov', '24.08', 6], ['NVIDIA Graphics Driver', 'NVIDIA Corporation', '566.14', 1300], ['McAfee LiveSafe', 'McAfee, LLC', '16.0', 820], ['WinRAR 7.01', 'win.rar GmbH', '7.01', 9], ['Adobe Acrobat (64-bit)', 'Adobe', '24.4', 1100], ['ExpressVPN', 'ExpressVPN', '12.88', 210], ['Notion', 'Notion Labs, Inc', '4.2', 290]];
    const desktop = d.map(([name, publisher, version, mb], i) => ({ id: 'w:' + i, kind: 'desktop', name, publisher, version, sizeKB: mb * 1024, installDate: `2025${String((i % 9) + 1).padStart(2, '0')}15` }));
    const store = C.BLOAT_APPS.filter(a => state.installed.has(a.pkg)).map(a => ({ id: 's:' + a.pkg, kind: 'store', pkg: a.pkg, name: a.pkg, publisher: 'CN=Microsoft Corporation', version: '1.0.0.0', sizeKB: null }));
    return desktop.concat(store);
  },
  async uninstallApp(app) { await wait(600); if (app.kind === 'store') state.installed.delete(app.pkg); return { ok: true, interactive: app.kind !== 'store' }; },

  async startupItems() { await wait(400); return state.startup.map(s => ({ ...s })); },
  async setStartup(item, enabled) { await wait(300); const s = state.startup.find(x => x.id === item.id); if (s) s.enabled = enabled; return { ok: true }; },

  async security() {
    await wait(500);
    return {
      defender: { realtime: true, antivirus: true, signatureAge: 4, signatureVersion: '1.421.112.0', quickScanAge: 12, fullScanAge: 210, tamper: true, behavior: true, network: true },
      firewall: [{ name: 'Domain', enabled: true }, { name: 'Private', enabled: true }, { name: 'Public', enabled: false }],
      uac: true, lastUpdate: new Date(Date.now() - 41 * 86400e3).toISOString().slice(0, 19), antivirus: ['Windows Defender'], smartscreen: 'Warn', bitlocker: 'Off', rdpEnabled: false, smb1: false,
    };
  },
  async defenderAction({ action }) { await wait(action === 'quick' ? 4000 : 1200); return { ok: true }; },
  async threats() { await wait(300); return [{ name: 'PUA:Win32/Presenoker', severity: 2, when: '2025-09-30T14:02:11', resources: ['file:_C:\\Users\\you\\Downloads\\free_converter_setup.exe'], action: true }]; },

  async network() {
    await wait(400);
    return [{ index: 12, name: 'Ethernet', desc: 'Realtek PCIe 2.5GbE Family Controller', ipv4: ['192.168.1.42'], gateway: ['192.168.1.1'], dns: ['192.168.1.1'], speed: '2.5 Gbps', mac: '00-1A-2B-3C-4D-5E' },
      { index: 18, name: 'Wi-Fi', desc: 'Intel(R) Wi-Fi 6E AX211', ipv4: ['192.168.1.77'], gateway: ['192.168.1.1'], dns: ['1.1.1.1', '1.0.0.1'], speed: '1.2 Gbps', mac: '00-1A-2B-3C-4D-5F' }];
  },
  async networkAction() { await wait(700); return { ok: true }; },

  async tweakStates() { await wait(350); return { ...state.tweaks }; },
  async setTweaks(changes) { await wait(600); for (const c of changes) state.tweaks[c.id] = !!c.on; return { ok: true }; },

  async bloatStatus() { await wait(400); return [...state.installed]; },
  async removeApps({ pkgs }) { await wait(1500); pkgs.forEach(p => state.installed.delete(p)); return pkgs.map(pkg => ({ pkg, ok: true })); },
  async servicesStatus() { await wait(300); return C.SERVICES.map(s => ({ name: s.name, status: state.services[s.name] === 'Disabled' ? 'Stopped' : 'Running', startType: state.services[s.name] })); },
  async setServices(changes) { await wait(800); changes.forEach(c => { state.services[c.name] = c.type; }); return changes.map(c => ({ name: c.name, ok: true })); },
  async tasksStatus() { await wait(300); return C.SCHEDULED_TASKS.map(t => ({ id: t.path + t.name, state: state.tasks[t.path + t.name] })); },
  async setTasks(changes) { await wait(600); changes.forEach(c => { state.tasks[c.path + c.name] = c.enable ? 'Ready' : 'Disabled'; }); return { ok: true }; },

  async updatesStatus() {
    await wait(400);
    const u = state.updates || (state.updates = { pausedUntil: null, activeStart: 8, activeEnd: 17, lockedVersion: null });
    return { ...u, displayVersion: '24H2', product: 'Windows 11', rebootPending: true,
      hotfixes: [['KB5046617', 'Security Update', 6], ['KB5046740', 'Update', 20], ['KB5044284', 'Security Update', 35], ['KB5043080', 'Security Update', 62], ['KB5042352', 'Update', 70]].map(([kb, desc, d]) => ({ kb, desc, installed: new Date(Date.now() - d * 86400e3).toISOString().slice(0, 19) })) };
  },
  async updatesAction({ action, days, start, end, version }) {
    await wait(500);
    const u = state.updates || (state.updates = {});
    if (action === 'pause') u.pausedUntil = new Date(Date.now() + days * 86400e3).toISOString().slice(0, 19) + 'Z';
    if (action === 'resume') u.pausedUntil = null;
    if (action === 'active-hours') { u.activeStart = start; u.activeEnd = end; }
    if (action === 'lock') u.lockedVersion = version;
    if (action === 'unlock') u.lockedVersion = null;
    return { ok: true };
  },
  async powerPlans() { await wait(200); return [{ guid: '381b4222-f694-41f0-9685-ff5bb260df2e', name: 'Balanced', active: true }, { guid: '8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c', name: 'High performance', active: false }, { guid: 'a1841308-3541-4fab-bc81-f71556f20b4a', name: 'Power saver', active: false }]; },
  async setPowerPlan() { await wait(200); return { ok: true }; },
  async runRepair() { await wait(800); return { ok: true }; },
  async launchTool() { await wait(150); return { ok: true }; },
};
