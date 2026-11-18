'use strict';
// Syntax-check every source file (works the same on Windows, macOS and Linux).
const { execFileSync } = require('child_process');
const fs = require('fs');
const path = require('path');
const dirs = ['src', 'src/backend', 'src/renderer', 'test'];
let bad = 0;
for (const d of dirs) for (const f of fs.readdirSync(d).filter(f => f.endsWith('.js'))) {
  try { execFileSync(process.execPath, ['--check', path.join(d, f)], { stdio: 'pipe' }); } catch (e) { bad++; console.error(String(e.stderr || e.message)); }
}
if (bad) process.exit(1);
console.log('All files OK');
