'use strict';
// Stage the native OpenSweep Core service for electron-builder.
// Copies core/build/Release/OpenSweepCore.exe into build-resources/core/ when it was built;
// otherwise leaves a placeholder README so "extraResources" never points at a missing path.
// Usage: node scripts/prepare-core.js [--require]   (--require fails when the exe is missing)
const fs = require('fs');
const path = require('path');

const root = path.join(__dirname, '..');
const src = path.join(root, 'core', 'build', 'Release', 'OpenSweepCore.exe');
const outDir = path.join(root, 'build-resources', 'core');
const dest = path.join(outDir, 'OpenSweepCore.exe');

fs.rmSync(outDir, { recursive: true, force: true });
fs.mkdirSync(outDir, { recursive: true });

if (fs.existsSync(src)) {
  fs.copyFileSync(src, dest);
  console.log(`prepare-core: bundled ${path.relative(root, src)} (${fs.statSync(dest).size} bytes) -> ${path.relative(root, dest)}`);
} else {
  fs.writeFileSync(path.join(outDir, 'README.txt'),
    'OpenSweep Core was not built for this package.\r\n' +
    'Build it with "npm run core:build" (Windows, CMake + MSVC) and run "node scripts/prepare-core.js" before electron-builder.\r\n');
  const msg = `prepare-core: ${path.relative(root, src)} not found; packaging without OpenSweep Core (placeholder README written)`;
  if (process.argv.includes('--require')) { console.error(msg); process.exit(1); }
  console.warn(msg);
}
