'use strict';
// "Clean everything": extra cleanup sources on top of the catalog cleanup targets.
const fs = require('fs');
const os = require('os');
const path = require('path');
const { ps, elevated } = require('./pscore');

// Old driver packages: for each driver (same .inf name + provider + class) keep the newest
// and offer the older copies. pnputil without /force refuses drivers still in use.
async function oldDrivers() {
  const r = await ps(`
try { (Get-Process -Id $PID).PriorityClass = 'BelowNormal' } catch {}
$all = @(Get-WindowsDriver -Online -ErrorAction Stop | Where-Object { $_.Driver -like 'oem*.inf' })
$groups = $all | Group-Object { (Split-Path $_.OriginalFileName -Leaf) + '|' + $_.ProviderName + '|' + $_.ClassName }
Out-Json @(foreach ($g in $groups) {
  if ($g.Count -lt 2) { continue }
  $sorted = @($g.Group | Sort-Object @{ e={ $_.Date } ; Descending=$true }, @{ e={ $_.Version } ; Descending=$true })
  foreach ($d in ($sorted | Select-Object -Skip 1)) {
    $dir = Split-Path $d.OriginalFileName -Parent
    $size = Get-DirSize $dir 2000
    [pscustomobject]@{ inf=$d.Driver; name=(Split-Path $d.OriginalFileName -Leaf); provider=$d.ProviderName; cls=$d.ClassName; version=$d.Version; date=$(if ($d.Date) { $d.Date.ToString('yyyy-MM-dd') } else { $null }); size=[double]$size; newest=$sorted[0].Version }
  }
})`, undefined, { timeout: 5 * 60 * 1000 });
  return Array.isArray(r) ? r : r ? [r] : [];
}
async function removeDrivers(infs) {
  const r = await elevated(`
$res = @(foreach ($i in $a) { $out = & pnputil.exe /delete-driver $i 2>&1 | Out-String; [pscustomobject]@{ inf=$i; ok=($LASTEXITCODE -eq 0); msg=$out.Trim() } })
Out-Result $res`, infs, { timeout: 10 * 60 * 1000 });
  return Array.isArray(r) ? r : r ? [r] : [];
}

async function windowsOldSize() {
  const r = await ps(`$p = Join-Path $env:SystemDrive 'Windows.old'
if (Test-Path -LiteralPath $p) { Out-Json @{ exists=$true; size=(Get-DirSize $p 20000) } } else { Out-Json @{ exists=$false; size=0 } }`, undefined, { timeout: 10 * 60 * 1000 });
  return r || { exists: false, size: 0 };
}
async function removeWindowsOld() {
  return elevated(`$p = Join-Path $env:SystemDrive 'Windows.old'
if (Test-Path -LiteralPath $p) {
  & takeown.exe /F $p /R /A /D Y | Out-Null
  & icacls.exe $p /grant '*S-1-5-32-544:F' /T /C /Q | Out-Null
  Remove-Item -LiteralPath $p -Recurse -Force -ErrorAction SilentlyContinue
}
Out-Result @{ ok = -not (Test-Path -LiteralPath $p) }`, undefined, { timeout: 60 * 60 * 1000 });
}
async function componentCleanup() {
  return elevated('& dism.exe /Online /Cleanup-Image /StartComponentCleanup /Quiet | Out-Null; Out-Result @{ ok=($LASTEXITCODE -eq 0) }', undefined, { timeout: 60 * 60 * 1000 });
}

// ---- pure Node parts (also used by the demo on any OS)
const INSTALLER_EXT = ['.exe', '.msi', '.msix', '.msixbundle', '.appx', '.iso', '.img'];
function oldInstallers(downloadsDir, days = 30) {
  const out = [];
  let names = [];
  try { names = fs.readdirSync(downloadsDir); } catch { return out; }
  const cutoff = Date.now() - days * 86400e3;
  for (const n of names) {
    const full = path.join(downloadsDir, n);
    let st; try { st = fs.statSync(full); } catch { continue; }
    if (st.isFile() && INSTALLER_EXT.includes(path.extname(n).toLowerCase()) && st.mtimeMs < cutoff) out.push({ path: full, name: n, size: st.size, modified: new Date(st.mtimeMs).toISOString() });
  }
  return out.sort((a, b) => b.size - a.size);
}

// Empty folders in the user's own folders (not AppData, not hidden, not system), bottom-up.
function emptyFolders(home, { maxDepth = 6, limit = 2000 } = {}) {
  const skip = new Set(['appdata', 'onedrive', '.git', 'node_modules', '.vscode', 'saved games', 'searches', 'links', 'contacts', 'favorites']);
  const out = [];
  function walk(dir, depth) {
    let ents; try { ents = fs.readdirSync(dir, { withFileTypes: true }); } catch { return false; }
    let empty = ents.length === 0;
    let allChildrenEmpty = true;
    for (const e of ents) {
      if (e.isSymbolicLink()) { allChildrenEmpty = false; continue; }
      if (e.isDirectory()) {
        if (e.name.startsWith('.') || skip.has(e.name.toLowerCase()) || depth >= maxDepth) { allChildrenEmpty = false; continue; }
        if (!walk(path.join(dir, e.name), depth + 1)) allChildrenEmpty = false;
      } else allChildrenEmpty = false;
    }
    empty = empty || allChildrenEmpty;
    if (empty && depth > 1 && out.length < limit) out.push(dir);
    return empty;
  }
  for (const top of ['Desktop', 'Documents', 'Downloads', 'Pictures', 'Music', 'Videos']) walk(path.join(home, top), 1);
  // Only report the top-most empty folder of each empty branch.
  const set = new Set(out);
  return out.filter(p => !set.has(path.dirname(p)));
}

async function compressPaths(paths) {
  const r = await elevated(`$res = @(foreach ($p in $a) { if (Test-Path -LiteralPath $p -PathType Container) { & compact.exe /c /s:"$p" /i /q | Out-Null } else { & compact.exe /c "$p" /i /q | Out-Null }; [pscustomobject]@{ path=$p; ok=($LASTEXITCODE -eq 0) } })
Out-Result $res`, paths, { timeout: 60 * 60 * 1000 });
  return Array.isArray(r) ? r : r ? [r] : [];
}

module.exports = { compressPaths, oldDrivers, removeDrivers, windowsOldSize, removeWindowsOld, componentCleanup, oldInstallers, emptyFolders, homedir: os.homedir };
