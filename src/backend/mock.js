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
  async setTweaks(changes) {
    await wait(600);
    for (const c of changes) if (c.id !== 'ai.recall-off') state.tweaks[c.id] = !!c.on; // one sample failure so the verify UI can be seen
    return this.verifyTweaks(changes);
  },
  async verifyTweaks(changes) {
    const failed = changes.filter(c => !!state.tweaks[c.id] !== !!c.on).map(c => ({ id: c.id, title: (C.TWEAKS.find(t => t.id === c.id) || {}).title || c.id, error: 'Demo: pretend a policy is overriding this one.' }));
    return { ok: !failed.length, applied: changes.length - failed.length, failed, states: { ...state.tweaks } };
  },

  async bloatStatus() { await wait(400); return [...state.installed]; },
  async removeApps({ pkgs }) { await wait(1500); pkgs.forEach(p => state.installed.delete(p)); return pkgs.map(pkg => ({ pkg, ok: true })); },
  async servicesStatus() { await wait(300); return C.SERVICES.map(s => ({ name: s.name, status: state.services[s.name] === 'Disabled' ? 'Stopped' : 'Running', startType: state.services[s.name] })); },
  async setServices(changes) { await wait(800); changes.forEach(c => { state.services[c.name] = c.type; }); return changes.map(c => ({ name: c.name, ok: true, error: null })); },
  async tasksStatus() { await wait(300); return C.SCHEDULED_TASKS.map(t => ({ id: t.path + t.name, state: state.tasks[t.path + t.name] })); },
  async setTasks(changes) { await wait(600); changes.forEach(c => { state.tasks[c.path + c.name] = c.enable ? 'Ready' : 'Disabled'; }); return { ok: true, results: changes.map(c => ({ id: c.path + c.name, ok: true, error: null })) }; },

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
  async isAdmin() { return true; },
  async stabilityCheck({ days = 7 } = {}) {
    await wait(900);
    const t = (h) => new Date(Date.now() - h * 3600e3).toISOString().slice(0, 19);
    return { checkedAt: t(0), days,
      devices: [{ id: 'USB\\VID_0BDA&PID_8771\\5&1A2B', name: 'Generic Bluetooth Adapter', cls: 'Bluetooth', code: 43, problem: 'Device reported a problem' }],
      events: [
        { key: 'Display|4101|9001', when: t(20), kind: 'Display driver reset', source: 'Display', eventId: 4101, message: 'Display driver nvlddmkm stopped responding and has successfully recovered.' },
        { key: 'Kernel-Power|41|8800', when: t(70), kind: 'Unexpected shutdown', source: 'Microsoft-Windows-Kernel-Power', eventId: 41, message: 'The system has rebooted without cleanly shutting down first.' },
      ],
      oldDrivers: [{ name: 'Realtek High Definition Audio', provider: 'Realtek', version: '6.0.1.7548', date: '2015-07-01' }],
      unsignedDrivers: [], thirdPartyDrivers: 41 };
  },
  async scanDevices() { await wait(800); return { ok: true }; },
  async setAutoStart() { await wait(200); return { ok: true }; },

  // ---------------- v0.3 features (sample data)
  async reliability({ days = 30 } = {}) {
    await wait(700);
    const { summarize } = require('./win-reliability');
    const recs = [];
    const day = (k, h = 12) => new Date(Date.now() - k * 86400e3 - h * 3600e3).toISOString().slice(0, 19);
    const add = (k, src, ev, product, msg) => recs.push({ id: String(recs.length), t: day(k, rnd(1, 20)), src, ev, product, msg });
    [1, 3, 3, 9, 15, 15, 22].forEach(k => add(k, 'Application Error', 1000, 'Discord', 'Faulting application name: Discord.exe, version: 1.0.9170'));
    [5, 12].forEach(k => add(k, 'Application Hang', 1002, 'Explorer', 'The program explorer.exe stopped interacting with Windows.'));
    add(8, 'Microsoft-Windows-WER-SystemErrorReporting', 1001, '', 'The computer has rebooted from a bugcheck. The bugcheck was: 0x00000116 (nvlddmkm.sys)');
    add(8, 'Microsoft-Windows-Kernel-Power', 41, '', 'The system has rebooted without cleanly shutting down first.');
    add(2, 'Microsoft-Windows-WindowsUpdateClient', 20, '2026-11 Cumulative Update for Windows 11 (KB5046617)', 'Installation Failure: 0x800f0922');
    for (let k = 0; k < days; k += 3) add(k, 'Microsoft-Windows-WindowsUpdateClient', 19, 'Security Intelligence Update for Microsoft Defender Antivirus', 'Installation Successful');
    add(6, 'MsiInstaller', 11707, 'Steam', 'Product: Steam -- Installation completed successfully.');
    const index = []; let v = 4.2;
    for (let k = days - 1; k >= 0; k--) { const hit = recs.filter(r => r.t.startsWith(day(k).slice(0, 10)) && /Error|Hang|WER|Kernel/.test(r.src)).length; v = Math.max(1, Math.min(10, v + 0.35 - hit * 1.1)); index.push({ t: day(k, 0), v: Math.round(v * 100) / 100 }); }
    return summarize({ index, records: recs }, days);
  },
  startProcessStream(onData) {
    const tick = async () => { const procs = await this.processes(); onData({ t: new Date().toISOString(), procs: procs.map(p => ({ ...p, mem: p.memMB, io: rnd(0, p.name === 'chrome' ? 4e6 : 2e5), threads: rnd(8, 90), desc: p.name, sig: p.name === 'Notion' ? 'NotSigned' : 'Valid', pri: 'Normal', path: p.name === 'Notion' ? 'C:\\Users\\you\\AppData\\Local\\Temp\\notion\\Notion.exe' : p.path })) }); };
    tick(); const t = setInterval(tick, 2000); return () => clearInterval(t);
  },
  async setPriority() { await wait(150); return { ok: true }; },
  async oldDrivers() { await wait(900); return [{ inf: 'oem41.inf', name: 'nv_dispi.inf', provider: 'NVIDIA', cls: 'Display', version: '32.0.15.6094', date: '2024-08-14', size: 1.1 * GB, newest: '32.0.15.6614' }, { inf: 'oem12.inf', name: 'nv_dispi.inf', provider: 'NVIDIA', cls: 'Display', version: '31.0.15.5222', date: '2024-03-02', size: 1.05 * GB, newest: '32.0.15.6614' }, { inf: 'oem7.inf', name: 'rt640x64.inf', provider: 'Realtek', cls: 'Net', version: '10.68.0', date: '2023-10-01', size: 4 * MB, newest: '10.72.0' }]; },
  async removeDrivers(infs) { await wait(1200); return infs.map((inf, i) => ({ inf, ok: i !== 1, msg: '' })); },
  async windowsOldSize() { await wait(300); return { exists: true, size: 18.6 * GB }; },
  async removeWindowsOld() { await wait(2000); return { ok: true }; },
  async componentCleanup() { await wait(2500); return { ok: true }; },
  oldInstallers() { return [['Win11_24H2_English_x64.iso', 5.4 * GB], ['DaVinci_Resolve_19_Windows.zip', 3.4 * GB], ['SteamSetup.exe', 2.3 * MB], ['ChromeSetup.exe', 1.4 * MB], ['vlc-3.0.21-win64.exe', 44 * MB]].map(([n, s], i) => ({ path: `C:\\Users\\you\\Downloads\\${n}`, name: n, size: s, modified: new Date(Date.now() - (60 + i * 30) * 86400e3).toISOString() })); },
  emptyFolders() { return ['Documents\\New folder', 'Documents\\New folder (2)', 'Pictures\\Screenshots\\old', 'Desktop\\stuff'].map(p => `C:\\Users\\you\\${p}`); },
  async compressPaths(paths) { await wait(800); return paths.map(p => ({ path: p, ok: true })); },
  async setCleanSchedule() { await wait(200); return { ok: true }; },
  async programs() {
    await wait(800);
    const d = [['Google Chrome', 'Google LLC', '131.0', 450, 0], ['Discord', 'Discord Inc.', '1.0.9170', 380, 1], ['Steam', 'Valve Corporation', '2.10', 900, 3], ['Spotify', 'Spotify AB', '1.2.50', 320, 2], ['Visual Studio Code', 'Microsoft Corporation', '1.95.3', 380, 0],
      ['7-Zip 24.08 (x64)', 'Igor Pavlov', '24.08', 6, 40], ['NVIDIA Graphics Driver', 'NVIDIA Corporation', '566.14', 1300, null], ['McAfee LiveSafe', 'McAfee, LLC', '16.0', 820, 400], ['WinRAR 7.01', 'win.rar GmbH', '7.01', 9, 220], ['Adobe Acrobat (64-bit)', 'Adobe', '24.4', 1100, 95],
      ['ExpressVPN', 'ExpressVPN', '12.88', 210, 260], ['Notion', 'Notion Labs, Inc', '4.2', 290, 5], ['OneLaunch', 'OneLaunch', '5.32', 160, 500], ['Driver Booster 11', 'IObit', '11.6', 95, 300]];
    const desktop = d.map(([name, publisher, version, mb, used], i) => ({ id: 'w:' + i, kind: 'desktop', name, publisher, version, sizeKB: mb * 1024, installDate: `2025${String((i % 9) + 1).padStart(2, '0')}15`, uninstall: i % 3 ? `"C:\\Program Files\\${name}\\unins000.exe"` : `MsiExec.exe /X{${'0'.repeat(8)}-0000-0000-0000-${String(i).padStart(12, '0')}}`, location: `C:\\Program Files\\${name}`, regKey: `HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\${name}`, lastUsed: used === null ? null : new Date(Date.now() - used * 86400e3).toISOString().slice(0, 19) }));
    const store = C.BLOAT_APPS.filter(a => state.installed.has(a.pkg)).map(a => ({ id: 's:' + a.pkg, kind: 'store', pkg: a.pkg, name: a.name, publisher: 'Microsoft Corporation', version: '1.0.0.0', sizeKB: null, lastUsed: null }));
    return desktop.filter(a => !(state.removed || new Set()).has(a.id)).concat(store);
  },
  silentCommand(a) { return a.kind === 'store' || /unins|MsiExec/i.test(a.uninstall || '') ? 'silent' : null; },
  async runUninstall(a) { await wait(1500); (state.removed = state.removed || new Set()).add(a.id); if (a.kind === 'store') state.installed.delete(a.pkg); return { ok: true, gone: true }; },
  async stillInstalled(a) { return !(state.removed || new Set()).has(a.id); },
  async leftoverScan(a) {
    await wait(1400);
    const n = a.name.replace(/\s*\d.*$/, '');
    const items = [
      { type: 'dir', path: `C:\\Program Files\\${n}`, level: 'safe', detail: 'Install folder', size: 48 * MB },
      { type: 'dir', path: `C:\\Users\\you\\AppData\\Roaming\\${n}`, level: 'safe', detail: 'Named after the app', size: 212 * MB },
      { type: 'dir', path: `C:\\Users\\you\\AppData\\Local\\${n}`, level: 'safe', detail: 'Named after the app', size: 640 * MB },
      { type: 'reg', path: `HKEY_CURRENT_USER\\Software\\${n}`, level: 'safe', detail: 'Settings key named after the app', size: 0 },
      { type: 'run', path: `HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run|${n}`, level: 'safe', detail: `Starts with Windows: "C:\\Users\\you\\AppData\\Local\\${n}\\Update.exe" --processStart`, size: 0 },
      { type: 'task', path: `\\${n}UpdateTaskMachine`, level: 'likely', detail: `Scheduled task: ${n}UpdateTaskMachine`, size: 0 },
      { type: 'dir', path: `C:\\ProgramData\\${(a.publisher || n).split(/[ ,]/)[0]}`, level: 'careful', detail: 'Publisher folder — may be shared with other apps', size: 3 * MB },
    ];
    return items.map((x, i) => ({ ...x, id: `${x.type}:${i}:${x.path}` }));
  },
  async removeLeftovers(items) { await wait(900); return { backup: 'C:\\ProgramData\\OpenSweep\\backups\\demo', results: items.map(i => ({ id: i.id, ok: true })) }; },
  async restoreBackup() { await wait(500); return { ok: true, restored: 2 }; },
  BACKUP_ROOT: () => 'C:\\ProgramData\\OpenSweep\\backups',
  async orphanScan() {
    await wait(1800);
    return [['C:\\Users\\you\\AppData\\Roaming\\Zoom', 412, 'Last changed 2024-02-11'], ['C:\\Program Files (x86)\\Origin', 1900, 'Last changed 2023-06-30'], ['C:\\Users\\you\\AppData\\Local\\Battle.net', 96, 'Last changed 2024-09-02'], ['C:\\ProgramData\\Epic', 64, 'Last changed 2025-01-12']]
      .map(([p, mb, d], i) => ({ id: `orphan:${i}:${p}`, type: 'dir', path: p, size: mb * MB, level: 'careful', detail: d }));
  },
  async browserExtensions() {
    await wait(500);
    return [['Chrome', 'uBlock Origin', '1.61.2', ['storage', 'tabs', '<all_urls>'], false], ['Chrome', 'Google Docs Offline', '1.84', ['storage'], false], ['Edge', 'Coupon Finder Pro', '3.2', ['tabs', 'webRequest', '<all_urls>', 'history'], true], ['Firefox', 'Bitwarden', '2024.11', ['storage', 'clipboardWrite'], false]]
      .map(([b, n, v, perm, forced], i) => ({ id: `${b}|Default|ext${i}`, browser: b, profile: 'Default', extId: `ext${i}`, name: n, version: v, desc: '', permissions: perm, forced, path: forced ? null : 'x' }));
  },
  async removeForcedExtension() { await wait(400); return { ok: true }; },
  async optionalFeatures() { await wait(900); return Object.keys(C.FEATURE_NAMES).map((name, i) => ({ name, state: [0, 3, 6, 9].includes(i) ? 'Enabled' : 'Disabled' })); },
  async setFeature({ enable }) { await wait(1500); return { ok: true, state: enable ? 'Enabled' : 'Disabled', restart: true }; },
  async trackStart(label) { await wait(1200); state.track = { id: 'demo-track', label, started: new Date().toISOString() }; return state.track; },
  async trackFinish() { await wait(1200); const t = { ...state.track, finished: new Date().toISOString(), diff: { dirs: ['C:\\Program Files\\CoolApp', 'C:\\Users\\you\\AppData\\Roaming\\CoolApp'], files: ['C:\\Users\\Public\\Desktop\\CoolApp.lnk'], reg: ['HKEY_CURRENT_USER\\Software\\CoolApp'], run: ['HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run|CoolAppHelper'], services: ['CoolAppUpdater'], tasks: [], firewall: [] } }; state.tracks = [t]; return t; },
  tracks() { return state.tracks || []; },
  trackItems(t) { return require('./win-uninstall').trackItems(t); },
  async hunt() { await wait(1500); return { pid: 1037, name: 'Discord', path: 'C:\\Program Files\\Discord\\Discord.exe', title: 'Discord', company: 'Discord Inc.' }; },
  async wingetUpgrades() { await wait(1200); return [{ name: 'Google Chrome', id: 'Google.Chrome', version: '130.0.6723.117', available: '131.0.6778.86' }, { name: '7-Zip', id: '7zip.7zip', version: '23.01', available: '24.08' }, { name: 'Discord', id: 'Discord.Discord', version: '1.0.9160', available: '1.0.9170' }]; },
  async wingetUpgrade() { await wait(2500); return { ok: true }; },

  // ---------------- OpenSweep Core (SIMULATED — no native service exists in demo mode)
  async coreStatus() {
    await wait(200);
    const c = state.core || (state.core = { installed: true, running: true, startedAt: Date.now() - 3 * 3600e3 - 17 * 60e3, requests: 1240, cpuMs: 4100 });
    const up = Math.round((Date.now() - c.startedAt) / 1000);
    if (c.running) { c.requests += rnd(1, 4); c.cpuMs += rnd(5, 30); }
    return {
      available: false, demo: true, simulated: true, exePresent: true, installed: c.installed, running: c.running,
      serviceState: !c.installed ? 'not_installed' : c.running ? 'running' : 'stopped',
      hello: c.running ? { service: 'OpenSweepCore', version: '0.1.0', protocol: 1, pid: 4312, mode: 'monitor', startedAt: new Date(c.startedAt).toISOString(), uptimeSec: up } : null,
      health: c.running ? { status: 'running', mode: 'monitor', uptimeSec: up, workingSetBytes: 6.8 * MB, privateBytes: 3.1 * MB, cpuTimeMs: c.cpuMs, clients: 1, requests: c.requests, errors: 0 } : null,
    };
  },
  async coreMetrics() {
    if (!state.core || !state.core.running) { const e = new Error('Simulated OpenSweep Core is stopped'); e.code = 'unavailable'; throw e; }
    const total = 32 * GB, load = rnd(38, 52), limit = 37.9 * GB, commit = Math.round((14 + Math.random() * 3) * GB);
    return { simulated: true, timestamp: new Date().toISOString(), cpu: { percent: Math.round(Math.random() * 180) / 10 + 3 },
      memory: { totalPhys: total, availPhys: Math.round(total * (1 - load / 100)), loadPercent: load, commitTotal: commit, commitLimit: limit, commitPeak: 22.4 * GB, systemCache: 6.2 * GB, kernelPaged: 610 * MB, kernelNonpaged: 280 * MB },
      system: { processes: rnd(238, 251), threads: rnd(3300, 3500), handles: rnd(118000, 124000) }, pageSize: 4096 };
  },
  async coreTopology() {
    await wait(100);
    return { simulated: true, logicalProcessors: 16, cores: 8, packages: 1, numaNodes: 1, groups: 1, hybrid: false, efficiencyClasses: [8], l3Caches: 1 };
  },
  async coreAction(action) {
    await wait(900);
    const c = state.core || (state.core = { installed: true, running: true, startedAt: Date.now(), requests: 0, cpuMs: 0 });
    if (action === 'install') { c.installed = true; }
    if (action === 'start') { if (!c.installed) throw new Error('Simulated: the service is not installed'); if (!c.running) { c.running = true; c.startedAt = Date.now(); c.requests = 0; c.cpuMs = 0; } }
    if (action === 'stop') c.running = false;
    if (action === 'uninstall') { c.installed = false; c.running = false; }
    return { ok: true, simulated: true };
  },
};
