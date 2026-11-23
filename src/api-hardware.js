'use strict';
// IPC methods for the Hardware check tab (Performance page). Wired in main.js:
//   Object.assign(api, require('./api-hardware')({ DEMO, logHistory, dialog, getWin: () => win }))
// hwCheck is read-only. hwFix only accepts ids from a fixed set and targets seen in the last check
// (validated in backend/hardware.js planFix); the renderer never supplies a rate, plan or command.

const hw = require('./backend/hardware');

module.exports = (ctx) => {
  const { DEMO, logHistory } = ctx;
  const mock = DEMO ? require('./backend/mock-hardware') : null;

  async function hwCheck() {
    const r = await hw.hwCheck(DEMO ? { raw: mock.raw() } : {});
    return { ...r, demo: !!DEMO };
  }

  async function hwFix(args) {
    const a = args || {};
    if (typeof a.id !== 'string' || !hw.FIX_IDS.includes(a.id)) throw new Error('Unknown hardware fix');
    if (a.target !== undefined && (typeof a.target !== 'string' || a.target.length > 300)) throw new Error('Invalid argument');
    const r = await hw.hwFix({ id: a.id, target: a.target }, DEMO ? mock.runner : undefined);
    if (logHistory) logHistory({ kind: 'hardware', summary: DEMO ? `${r.summary} (simulated)` : r.summary });
    return { ...r, demo: !!DEMO };
  }

  return { hwCheck, hwFix };
};
