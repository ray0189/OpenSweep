'use strict';
// Demo data for the Hardware check: the same raw shape the PowerShell script returns, so the demo goes
// through the real analysis in hardware.js. Simulated PC: DDR5-6000 kit running at 4800 (XMP off),
// a 144 Hz monitor left at 60 Hz, C: with 9% free. Fixes change this in-memory state only.

const GiB = 1073741824;
const daysAgo = (d) => new Date(Date.now() - d * 86400e3).toISOString().slice(0, 10);

const state = {
  displays: [
    { device: '\\\\.\\DISPLAY1', adapter: 'NVIDIA GeForce RTX 4070', monitor: 'DELL S2721DGF', primary: true, width: 2560, height: 1440, bpp: 32, hz: 60, rates: [60, 75, 100, 120, 144] },
    { device: '\\\\.\\DISPLAY2', adapter: 'NVIDIA GeForce RTX 4070', monitor: 'DELL P2419H', primary: false, width: 1920, height: 1080, bpp: 32, hz: 60, rates: [50, 59, 60] },
  ],
  activePlan: '381b4222-f694-41f0-9685-ff5bb260df2e',
};

function raw() {
  const ok = (data, ms = 40) => ({ ok: true, data, ms });
  return {
    system: ok({ maker: 'ASUS', model: 'System Product Name', chassis: [3], battery: false, virtual: false }),
    ram: ok([
      { speed: 6000, configured: 4800, capacity: 16 * GiB, bank: 'P0 CHANNEL A', slot: 'DIMM 1', type: 34, formFactor: 8, maker: 'G Skill Intl', part: 'F5-6000J3038F16G' },
      { speed: 6000, configured: 4800, capacity: 16 * GiB, bank: 'P0 CHANNEL B', slot: 'DIMM 1', type: 34, formFactor: 8, maker: 'G Skill Intl', part: 'F5-6000J3038F16G' },
    ], 120),
    display: ok(state.displays.map(d => ({ ...d, rates: [...d.rates] })), 180),
    gpu: ok([{ name: 'NVIDIA GeForce RTX 4070', id: 'PCI\\VEN_10DE&DEV_2786&SUBSYS_00000000&REV_A1\\4&1A2B3C4D&0&0019', status: 'OK', CurrentLinkSpeed: 1, CurrentLinkWidth: 16, MaxLinkSpeed: 4, MaxLinkWidth: 16 }], 900),
    ssd: ok({
      disableDeleteNotify: 0,
      volumes: [
        { letter: 'C', label: 'Windows', fs: 'NTFS', size: 931 * GiB, free: 84 * GiB, media: 'SSD', bus: 'NVMe', disk: 'Samsung SSD 980 PRO 1TB' },
        { letter: 'D', label: 'Games', fs: 'NTFS', size: 1863 * GiB, free: 764 * GiB, media: 'SSD', bus: 'NVMe', disk: 'WD_BLACK SN850X 2000GB' },
      ],
    }, 700),
    power: ok({
      plans: [
        { guid: '381b4222-f694-41f0-9685-ff5bb260df2e', name: 'Balanced' },
        { guid: '8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c', name: 'High performance' },
        { guid: 'a1841308-3542-4fab-bc81-f71121e6c3d8', name: 'Power saver' },
      ].map(p => ({ ...p, active: p.guid === state.activePlan })),
      ac: 1, saver: 0, batteryPercent: 255, overlay: { ac: '00000000-0000-0000-0000-000000000000', dc: '00000000-0000-0000-0000-000000000000' },
    }, 150),
    thermal: ok({ events: [], firmwareLimited: 0, cpu: [{ name: 'AMD Ryzen 7 7800X3D 8-Core Processor', load: 6, cur: 4201, max: 4201 }] }, 300),
    drivers: ok([
      { name: 'NVIDIA GeForce RTX 4070', cls: 'DISPLAY', date: daysAgo(64), version: '32.0.15.6614', maker: 'NVIDIA', provider: 'NVIDIA' },
      { name: 'AMD SMBus', cls: 'SYSTEM', date: daysAgo(170), version: '5.12.0.44', maker: 'Advanced Micro Devices, Inc', provider: 'Advanced Micro Devices, Inc.' },
      { name: 'Standard NVM Express Controller', cls: 'SCSIADAPTER', date: '2006-06-21', version: '10.0.26100.1', maker: 'Standard NVM Express Controller', provider: 'Microsoft' },
    ], 2100),
    gaming: ok({ gameMode: 1, hwSchMode: null }, 20),
  };
}

// Same interface as hardware.js realRunner.
const runner = {
  async setRefresh(device, rate) {
    const d = state.displays.find(x => x.device === device);
    if (!d || !d.rates.includes(rate)) throw new Error(`${rate} Hz is not offered at the current resolution.`);
    await new Promise(r => setTimeout(r, 600));
    const before = d.hz; d.hz = rate;
    return { before, after: rate, restart: false };
  },
  async retrim() { await new Promise(r => setTimeout(r, 1500)); return { ok: true }; },
  async setPlan(guid) { await new Promise(r => setTimeout(r, 400)); state.activePlan = guid; return { ok: true }; },
};

module.exports = { raw, runner };
